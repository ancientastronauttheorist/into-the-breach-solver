// The Lua host's call layer: loading, CallMethod dispatch, result
// conversion and the native -> Lua -> native re-entry points.

#include "itb/lua_host.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <variant>

#include "itb/tile_rules.hpp"
#include "lua_env.hpp"
#include "lua_host_internal.hpp"
#include "lua_native.hpp"
#include "weapon_ports.hpp"

extern "C" {
#include "lauxlib.h"
}

namespace itb {

using lua::Cls;
using lua::HostContext;
using lua::Instance;

// ---- engine conversion -----------------------------------------------------------

SpaceDamage to_engine(const LuaSpaceDamage& sd) {
  SpaceDamage out;
  out.loc = sd.loc;
  out.damage = sd.iDamage;
  out.push = static_cast<Dir>(sd.iPush);
  out.shield = sd.iShield;
  out.fire = static_cast<StatusChange>(sd.iFire);
  out.frozen = static_cast<StatusChange>(sd.iFrozen);
  out.injure = static_cast<StatusChange>(sd.iInjure);
  out.smoke = static_cast<StatusChange>(sd.iSmoke);
  out.acid = static_cast<StatusChange>(sd.iAcid);
  out.crack = sd.iCrack == 1;
  out.ko_effect = sd.bKO_Effect;
  out.boosted = sd.boosted;
  out.terrain = sd.iTerrain;
  if (!sd.sPawn.empty()) out.spawn_pawn = intern(sd.sPawn);
  out.spawn_team = static_cast<Team>(sd.iPawnTeam);
  out.owner_team = static_cast<Team>(sd.owner_team);
  out.evacuate = sd.bEvacuate;
  out.delay = sd.fDelay;
  if (!sd.sAnimation.empty()) out.animation = intern(sd.sAnimation);
  out.anim_flags = static_cast<uint8_t>(sd.anim_flags);
  out.mode_override = static_cast<int8_t>(sd.mode_override);
  out.projectile = static_cast<ProjectileKind>(sd.projectile_kind);
  out.projectile_source = sd.projectile_source;
  if (!sd.projectile_art.empty()) out.art = intern(sd.projectile_art);
  out.move_kind = static_cast<MoveKind>(sd.move_kind);
  out.path = sd.path;
  out.grapple_source = sd.grapple_source;
  if (!sd.sItem.empty()) out.item = intern(sd.sItem);
  out.script = sd.sScript;
  return out;
}

SkillEffect to_engine(const LuaSkillEffect& se) {
  SkillEffect out;
  out.effect.reserve(se.effect.size());
  for (const LuaSpaceDamage& sd : se.effect) out.effect.push_back(to_engine(sd));
  out.q_effect.reserve(se.q_effect.size());
  for (const LuaSpaceDamage& sd : se.q_effect) out.q_effect.push_back(to_engine(sd));
  out.origin = se.piOrigin;
  out.target = se.target;
  out.owner = se.iOwner;
  out.team = static_cast<Team>(se.team);
  out.follow_up = se.follow_up;
  return out;
}

std::string LuaWrite::describe() const {
  std::string s = object + ":" + method + "(";
  for (size_t i = 0; i < args.size(); ++i) {
    if (i) s += ", ";
    std::visit(
        [&](const auto& v) {
          using T = std::decay_t<decltype(v)>;
          if constexpr (std::is_same_v<T, std::monostate>) {
            s += "nil";
          } else if constexpr (std::is_same_v<T, bool>) {
            s += v ? "true" : "false";
          } else if constexpr (std::is_same_v<T, double>) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.14g", v);
            s += buf;
          } else if constexpr (std::is_same_v<T, std::string>) {
            s += "\"" + v + "\"";
          } else {
            s += lua::point_string(v);
          }
        },
        args[i]);
  }
  s += ")";
  if (pawn >= 0) s += " [pawn " + std::to_string(pawn) + "]";
  return s;
}

// ---- Impl ---------------------------------------------------------------------------

struct LuaHost::Impl {
  std::unique_ptr<detail::LuaEnv> env;
  lua_State* L = nullptr;
  HostContext ctx;
  std::unordered_map<std::string, std::string> passive_of;  // weapon -> Lua Passive
  bool count_calls = false;
  std::unordered_map<std::string, uint64_t> call_counts;  // "table:method" -> calls
  // The Lua `Pawn` global is assigned lazily: set_selected records the pawn
  // (ctx.selected) and the SetPawn call runs right before the next Lua code
  // does (flush_selected). Nothing but Lua code reads the global, so the
  // deferral is invisible, and repeated selections cost one call.
  bool select_pending = false;
  // GetTwoClick per weapon table: the CreateClass getter of a static table
  // field, so one successful call answers for good.
  std::unordered_map<std::string, bool> two_click;
  // Pawn types whose GetDeathEffect is the default Pawn:GetDeathEffect (an
  // empty SkillEffect, no randomness): the call is skipped (death_effect_raw).
  int default_death_ref = LUA_NOREF;
  std::unordered_map<Symbol, bool> default_death;
  // sScript chunks, compiled once (registry refs): a chunk is a plain
  // function of the globals, so running the cached one is running the code.
  std::unordered_map<std::string, int> chunks;
  // C++ weapon ports (weapon_ports.hpp), used while native_weapons is on.
  bool native_weapons = true;
  lua::WeaponPortTable ports;
  uint64_t native_calls = 0;  // calls answered by a port
  // Temporary pawns Board:GetDeployLocScore created (a native Pawn is
  // constructed for each, which draws from the C library's rand()).
  uint64_t temp_pawns = 0;
};

namespace {

constexpr size_t kMaxConsoleLines = 256;
constexpr int kMaxDepth = 6;
// The collector's pause (LUA_GCSETPAUSE, percent of the live heap): LuaHost
// creation.
constexpr int kLuaGcPause = 1000;

// Points the bindings at a board and a call record for the duration of one
// native -> Lua call, and restores everything afterwards (calls nest).
class Scope {
 public:
  Scope(LuaHost::Impl& im, const Board* board, LuaCall* call)
      : im_(im), top_(lua_gettop(im.L)), board_(im.ctx.board), call_(im.ctx.call) {
    if (board) im.ctx.board = board;
    im.ctx.call = call;
    ++im.ctx.depth;
  }
  ~Scope() {
    lua_settop(im_.L, top_);
    im_.ctx.board = board_;
    im_.ctx.call = call_;
    --im_.ctx.depth;
  }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

 private:
  LuaHost::Impl& im_;
  int top_;
  const Board* board_;
  LuaCall* call_;
};

void fail(LuaCall& call, std::string_view obj, std::string_view func, const std::string& why) {
  call.ok = false;
  if (call.error.empty()) {
    call.error = "Something went wrong in " + std::string(obj) + "::" + std::string(func) + ": " + why;
  }
}

// Lua SetPawn(p): what selecting a pawn does natively. The Lua global is
// assigned before the next Lua code runs (flush_selected).
void set_selected(LuaHost::Impl& im, const Pawn* p) {
  im.ctx.selected = p ? p->uid : -1;
  im.select_pending = true;
}

void flush_selected(LuaHost::Impl& im) {
  if (!im.select_pending) return;
  im.select_pending = false;
  lua_State* L = im.L;
  lua_getglobal(L, "SetPawn");
  lua::push_pawn_uid(L, im.ctx.selected);
  if (lua_pcall(L, 1, 0, 0) != 0) lua_pop(L, 1);
}

// CallMethod(obj, func, args...) under pcall. On success the result is on
// top of the stack.
template <class PushArgs>
bool call_method(LuaHost::Impl& im, std::string_view obj, const char* func, LuaCall& call,
                 PushArgs push_args) {
  lua_State* L = im.L;
  if (im.ctx.depth > kMaxDepth) {
    fail(call, obj, func, "native -> Lua calls nested too deeply");
    return false;
  }
  flush_selected(im);
  if (im.count_calls) {
    std::string key(obj);
    key += ':';
    key += func;
    // The Lua function that runs, as source:line (inherited methods share it).
    const int top = lua_gettop(L);
    lua_getglobal(L, key.substr(0, obj.size()).c_str());
    if (lua_istable(L, -1)) {
      lua_getfield(L, -1, func);
      lua_Debug ar;
      if (lua_isfunction(L, -1) && lua_getinfo(L, ">S", &ar)) {
        key += " @";
        key += ar.short_src;
        key += ':';
        key += std::to_string(ar.linedefined);
      }
    }
    lua_settop(L, top);
    ++im.call_counts[key];
  }
  lua_getglobal(L, "CallMethod");
  lua_pushlstring(L, obj.data(), obj.size());
  lua_pushstring(L, func);
  const int nargs = push_args(L);
  if (lua_pcall(L, 2 + nargs, 1, 0) != 0) {
    fail(call, obj, func, lua::to_str(L, -1));
    lua_pop(L, 1);
    return false;
  }
  return true;
}

const char* lua_type_label(lua_State* L, int idx) {
  if (Instance* in = lua::to_instance(L, idx)) return lua::class_name(in->cls);
  return lua_typename(L, lua_type(L, idx));
}

// luabind's result conversion: the class instance or a cast failure.
bool take_point_list(lua_State* L, std::vector<Point>& out, LuaCall& call, std::string_view obj,
                     const char* func) {
  Instance* in = lua::to_instance(L, -1, Cls::PointList);
  if (!in) {
    fail(call, obj, func, std::string("cast_failed: returned ") + lua_type_label(L, -1) + ", expected PointList");
    return false;
  }
  try {
    out = *static_cast<std::vector<Point>*>(lua::resolve(in));
  } catch (const std::exception& e) {
    fail(call, obj, func, e.what());
    return false;
  }
  return true;
}

bool take_skill_effect(lua_State* L, LuaSkillEffect& out, LuaCall& call, std::string_view obj,
                       const char* func) {
  Instance* in = lua::to_instance(L, -1, Cls::SkillEffect);
  if (!in) {
    fail(call, obj, func, std::string("cast_failed: returned ") + lua_type_label(L, -1) + ", expected SkillEffect");
    return false;
  }
  try {
    out = *static_cast<LuaSkillEffect*>(lua::resolve(in));
  } catch (const std::exception& e) {
    fail(call, obj, func, e.what());
    return false;
  }
  return true;
}

// The weapon's C++ port for `method`, when one applies to this call: ports
// on, a board set, and the nesting the Lua call would accept.
const lua::WeaponPort* port_for(LuaHost::Impl& im, std::string_view weapon) {
  if (!im.native_weapons || !im.ctx.board || im.ctx.depth > kMaxDepth) return nullptr;
  return im.ports.find(im.L, weapon);
}

// Runs a port; on success its console output goes to `call` (as the Lua's
// would) and the call is counted as native.
template <class Run>
bool run_port(LuaHost::Impl& im, const lua::PortSpec& spec, std::string_view weapon, const char* func,
              LuaCall& call, Run&& run) {
  if (spec.runs_lua) flush_selected(im);
  const Board& b = *im.ctx.board;
  lua::PortContext pc{b, im.ctx.selected >= 0 ? b.find_pawn(im.ctx.selected) : nullptr, im.L, {}};
  if (!run(pc)) return false;
  ++im.native_calls;
  for (std::string& line : pc.console) {
    if (call.console.size() < kMaxConsoleLines) call.console.push_back(std::move(line));
  }
  if (im.count_calls) {
    std::string key(weapon);
    key += ':';
    key += func;
    key += " @native ";
    key += spec.file;
    key += ':';
    key += std::to_string(spec.line);
    ++im.call_counts[key];
  }
  return true;
}

std::vector<Point> target_area_impl(LuaHost::Impl& im, std::string_view weapon, Point origin,
                                    LuaCall& call) {
  std::vector<Point> area;
  // Skill::GetTargetArea only asks Lua for an origin on the board.
  if (!origin.valid()) return area;
  if (const lua::WeaponPort* port = port_for(im, weapon); port && port->area) {
    const lua::PortSpec& spec = *port->area;
    if (run_port(im, spec, weapon, "GetTargetArea", call, [&](lua::PortContext& pc) {
          return spec.area(pc, port->area_fields, origin, area);
        })) {
      std::erase_if(area, [](Point p) { return p.x < 0 || p.y < 0; });
      return area;
    }
    area.clear();
  }
  if (!call_method(im, weapon, "GetTargetArea", call, [&](lua_State* L) {
        lua::push_point(L, origin);
        return 1;
      })) {
    return area;
  }
  if (!take_point_list(im.L, area, call, weapon, "GetTargetArea")) return {};
  // Skill::GetTargetArea drops points with a negative coordinate.
  std::erase_if(area, [](Point p) { return p.x < 0 || p.y < 0; });
  return area;
}

LuaSkillEffect skill_effect_impl(LuaHost::Impl& im, std::string_view weapon, Point origin,
                                 Point target, LuaCall& call) {
  LuaSkillEffect se;
  if (const lua::WeaponPort* port = port_for(im, weapon); port && port->effect) {
    const lua::PortSpec& spec = *port->effect;
    if (run_port(im, spec, weapon, "GetSkillEffect", call, [&](lua::PortContext& pc) {
          return spec.effect(pc, port->effect_fields, origin, target, se);
        })) {
      return se;
    }
    se = LuaSkillEffect{};
  }
  if (!call_method(im, weapon, "GetSkillEffect", call, [&](lua_State* L) {
        lua::push_point(L, origin);
        lua::push_point(L, target);
        return 2;
      })) {
    return se;
  }
  if (!take_skill_effect(im.L, se, call, weapon, "GetSkillEffect")) return {};
  return se;
}

LuaHost::Impl& impl_of(lua_State* L) { return *lua::host_context(L).impl; }

// Whether CallMethod(type, "GetDeathEffect", tile) runs the default
// Pawn:GetDeathEffect (global.lua: `return SkillEffect()`), which has no
// other effect. CallMethod looks names containing "Mission" up elsewhere
// first, so those always go to Lua.
bool uses_default_death_effect(LuaHost::Impl& im, Symbol type) {
  if (im.default_death_ref == LUA_NOREF) return false;
  if (auto it = im.default_death.find(type); it != im.default_death.end()) return it->second;
  lua_State* L = im.L;
  const std::string name(symbol_name(type));
  bool is = false;
  if (!name.empty() && name.find("Mission") == std::string::npos) {
    const int top = lua_gettop(L);
    lua_getglobal(L, name.c_str());
    if (lua_istable(L, -1)) {
      lua_getfield(L, -1, "GetDeathEffect");
      lua_rawgeti(L, LUA_REGISTRYINDEX, im.default_death_ref);
      is = lua_rawequal(L, -1, -2) != 0;
    }
    lua_settop(L, top);
  }
  im.default_death.emplace(type, is);
  return is;
}

// Finds the default Pawn:GetDeathEffect after loading (before anything
// assigns the `Pawn` global) and checks that it returns an empty
// SkillEffect without drawing numbers; otherwise the shortcut stays off.
void find_default_death_effect(LuaHost::Impl& im) {
  lua_State* L = im.L;
  const int top = lua_gettop(L);
  lua_getglobal(L, "Pawn");
  if (lua_istable(L, -1)) {
    lua_getfield(L, -1, "GetDeathEffect");
    if (lua_isfunction(L, -1)) {
      lua_pushvalue(L, -1);
      const uint64_t draws = lua::rng(L).draws();
      if (lua_pcall(L, 0, 1, 0) == 0 && lua::rng(L).draws() == draws) {
        Instance* in = lua::to_instance(L, -1, Cls::SkillEffect);
        if (in && *static_cast<LuaSkillEffect*>(lua::resolve(in)) == LuaSkillEffect{}) {
          lua_pop(L, 1);
          im.default_death_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        }
      }
    }
  }
  lua_settop(L, top);
}

bool lua_starts_with(const std::string& s, const std::string& prefix) {
  return s.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace

// ---- re-entry points used by the bindings --------------------------------------------

namespace lua {

// Board::IsTargeted: every pawn but the selected one recomputes its queued
// attack (SkillManager::BoardUpdate) and Skill::IsTargeted scans the queued
// list for a dangerous hit on p or a push into p.
bool host_is_targeted(lua_State* L, Point p) {
  LuaHost::Impl& im = impl_of(L);
  const Board& b = *im.ctx.board;
  std::vector<std::pair<std::string, const Pawn*>> shooters;
  for (const Pawn& pawn : b.pawns()) {
    if (pawn.uid == im.ctx.selected || !pawn.queued.active()) continue;
    const int slot = pawn.queued.weapon;
    if (slot < 0 || slot >= kMaxWeapons || pawn.weapons[static_cast<size_t>(slot)] == kNoSymbol) continue;
    shooters.emplace_back(std::string(symbol_name(pawn.weapons[static_cast<size_t>(slot)])), &pawn);
  }
  for (const auto& [weapon, pawn] : shooters) {
    LuaCall nested;
    Scope scope(im, nullptr, &nested);
    const std::vector<Point> area = target_area_impl(im, weapon, pawn->pos, nested);
    if (std::find(area.begin(), area.end(), pawn->queued.target) == area.end()) continue;
    const LuaSkillEffect se = skill_effect_impl(im, weapon, pawn->pos, pawn->queued.target, nested);
    for (const LuaSpaceDamage& sd : se.q_effect) {
      const bool dangerous = (sd.iDamage > 0 && sd.iDamage != kDamageZero) || sd.iPush > 0;
      if (sd.loc == p && dangerous) return true;
      if (sd.iPush >= 0 && sd.iPush < 4 && p == sd.loc + kDirVectors[static_cast<size_t>(sd.iPush)] &&
          sd.loc.valid() && has_pawn(b, sd.loc)) {
        return true;
      }
    }
  }
  return false;
}

// Board::GetDeployLocScore for an empty tile: ScorePositioning(p, wall) with a
// temporary team-6 "Wall" pawn standing on p.
int host_deploy_score(lua_State* L, Point p) {
  LuaHost::Impl& im = impl_of(L);
  ++im.temp_pawns;
  Board scratch = *im.ctx.board;
  Pawn wall;
  wall.type = intern("Wall");
  wall.team = Team::Enemy;
  int32_t uid = 0;
  for (const Pawn& q : scratch.pawns()) uid = std::max(uid, q.uid + 1);
  wall.uid = uid;
  wall.pos = p;
  wall.hp = wall.max_hp = 1;
  wall.pushable = false;
  scratch.add_pawn(wall);
  LuaCall nested;
  Scope scope(im, &scratch, im.ctx.call ? im.ctx.call : &nested);
  flush_selected(im);
  lua_getglobal(L, "ScorePositioning");
  push_point(L, p);
  push_pawn(L, scratch.find_pawn(uid));
  if (lua_pcall(L, 2, 1, 0) != 0) throw LuaError(to_str(L, -1));
  if (lua_type(L, -1) != LUA_TNUMBER) throw LuaError("cast_failed: ScorePositioning did not return a number");
  return to_int(L, -1);
}

int host_position_score(lua_State* L, const Pawn& pawn, Point p) {
  LuaHost::Impl& im = impl_of(L);
  LuaCall nested;
  Scope scope(im, nullptr, im.ctx.call ? im.ctx.call : &nested);
  const std::string type(symbol_name(pawn.type));
  if (!call_method(im, type, "GetPositionScore", nested, [&](lua_State* S) {
        push_point(S, p);
        return 1;
      })) {
    throw LuaError(nested.error);
  }
  if (lua_type(L, -1) != LUA_TNUMBER) throw LuaError("cast_failed: GetPositionScore did not return a number");
  return to_int(L, -1);
}

// SquadControl::IsPassive: a prefix match over the Passive strings of the
// squad mechs' (powered) skills.
bool host_is_passive(lua_State* L, const std::string& name) {
  LuaHost::Impl& im = impl_of(L);
  if (im.ctx.options.passives) {
    return std::any_of(im.ctx.options.passives->begin(), im.ctx.options.passives->end(),
                       [&](const std::string& p) { return lua_starts_with(p, name); });
  }
  if (!im.ctx.board) return false;
  for (const Pawn& pawn : im.ctx.board->pawns()) {
    if (!pawn.mech) continue;
    for (Symbol w : pawn.weapons) {
      if (w == kNoSymbol) continue;
      const std::string weapon(symbol_name(w));
      auto it = im.passive_of.find(weapon);
      if (it == im.passive_of.end()) {
        std::string passive;
        lua_getglobal(L, weapon.c_str());
        if (lua_istable(L, -1)) {
          lua_getfield(L, -1, "Passive");
          if (lua_type(L, -1) == LUA_TSTRING) passive = to_str(L, -1);
          lua_pop(L, 1);
        }
        lua_pop(L, 1);
        it = im.passive_of.emplace(weapon, passive).first;
      }
      if (!it->second.empty() && lua_starts_with(it->second, name)) return true;
    }
  }
  return false;
}

void host_console(lua_State* L, const std::string& line) {
  HostContext& ctx = host_context(L);
  if (ctx.call && ctx.call->console.size() < kMaxConsoleLines) ctx.call->console.push_back(line);
}

}  // namespace lua

// ---- LuaHost -----------------------------------------------------------------------

LuaHost::LuaHost() = default;
LuaHost::~LuaHost() = default;

std::unique_ptr<LuaHost> LuaHost::create(const std::filesystem::path& game_root,
                                         const LuaHostOptions& options, LuaHostReport* report) {
  if (!std::filesystem::exists(game_root / "scripts" / "scripts.lua")) {
    throw std::runtime_error("no scripts/scripts.lua under " + game_root.string() +
                             " (set ITB_GAME_DIR to the game's root directory)");
  }
  std::unique_ptr<LuaHost> host(new LuaHost());
  host->impl_ = std::make_unique<Impl>();
  Impl& im = *host->impl_;
  im.ctx.options = options;
  im.ctx.impl = &im;
  im.native_weapons = options.native_weapons;
  if (const char* lua_only = std::getenv("ITB_LUA_WEAPONS"); lua_only && *lua_only && *lua_only != '0') {
    im.native_weapons = false;
  }
  detail::ScriptRun run;
  im.env = detail::LuaEnv::load_game_scripts(
      game_root, &run, [&im](lua_State* L) { lua::install_host_bindings(L, &im.ctx); });
  im.L = im.env->L();
  lua_State* L = im.L;
  find_default_death_effect(im);
  lua::rng(L).srand(options.seed);

  // In combat a game is running: CallMethod looks names containing
  // "Mission" up in GAME.Missions before _G. An empty game object keeps that
  // lookup from failing (it finds nothing and falls through to _G).
  if (luaL_dostring(L, "if GAME == nil and GameObject ~= nil then GAME = GameObject:new{} end") != 0) {
    lua_pop(L, 1);
  }
  // What Board::StartBattle and GameMap::GameMap do: SetBoard / SetGame.
  lua_getglobal(L, "SetBoard");
  lua::host_push_board(L);
  if (lua_pcall(L, 1, 0, 0) != 0) lua_pop(L, 1);
  lua_getglobal(L, "SetGame");
  lua::host_push_game(L);
  if (lua_pcall(L, 1, 0, 0) != 0) lua_pop(L, 1);
  // Garbage collection. The loaded scripts keep about 4 MB alive, and every
  // engine run makes short-lived garbage (SpaceDamage / Point userdata, effect
  // tables): with Lua's default pause (a new cycle once the heap doubles)
  // collection was 8-13% of a search's time, mostly marking the scripts again
  // and again. A cycle now starts at 10x the live heap (about 40 MB per
  // engine). No shipped script reads the collector (collectgarbage, weak
  // tables), and the userdata finalizers only free their C++ values.
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSETPAUSE, kLuaGcPause);

  if (report) {
    report->files_ok = run.files_ok;
    report->passes = run.passes;
    report->errors = run.errors;
    report->stubbed_globals = run.stubbed_globals;
    report->overwritten_stubs = run.overwritten_stubs;
  }
  return host;
}

std::string LuaHost::weapon_name(std::string_view base, bool upgrade_a, bool upgrade_b) {
  std::string name(base);
  if (upgrade_a || upgrade_b) {
    name += "_";
    if (upgrade_a) name += "A";
    if (upgrade_b) name += "B";
  }
  return name;
}

std::vector<Point> LuaHost::target_area(const Board& board, const Pawn& shooter,
                                        std::string_view weapon, Point origin, LuaCall* call) {
  LuaCall local;
  LuaCall& c = call ? *call : local;
  Scope scope(*impl_, &board, &c);
  set_selected(*impl_, &shooter);
  return target_area_impl(*impl_, weapon, origin, c);
}

LuaSkillEffect LuaHost::skill_effect_raw(const Board& board, const Pawn& shooter,
                                         std::string_view weapon, Point origin, Point target,
                                         LuaCall* call) {
  LuaCall local;
  LuaCall& c = call ? *call : local;
  Scope scope(*impl_, &board, &c);
  set_selected(*impl_, &shooter);
  return skill_effect_impl(*impl_, weapon, origin, target, c);
}

SkillEffect LuaHost::skill_effect(const Board& board, const Pawn& shooter, std::string_view weapon,
                                  Point origin, Point target, LuaCall* call) {
  return to_engine(skill_effect_raw(board, shooter, weapon, origin, target, call));
}

LuaSkillEffect LuaHost::queued_effect_raw(const Board& board, const Pawn& shooter,
                                          std::string_view weapon, Point target, LuaCall* call) {
  LuaCall local;
  LuaCall& c = call ? *call : local;
  Scope scope(*impl_, &board, &c);
  set_selected(*impl_, &shooter);
  const std::vector<Point> area = target_area_impl(*impl_, weapon, shooter.pos, c);
  if (std::find(area.begin(), area.end(), target) == area.end()) return {};
  LuaSkillEffect se = skill_effect_impl(*impl_, weapon, shooter.pos, target, c);
  se.effect = std::move(se.q_effect);
  se.q_effect.clear();
  return se;
}

SkillEffect LuaHost::queued_effect(const Board& board, const Pawn& shooter, std::string_view weapon,
                                   Point target, LuaCall* call) {
  return to_engine(queued_effect_raw(board, shooter, weapon, target, call));
}

int LuaHost::target_score(const Board& board, const Pawn& shooter, std::string_view weapon, Point origin,
                          Point target, LuaCall* call) {
  LuaCall local;
  LuaCall& c = call ? *call : local;
  Scope scope(*impl_, &board, &c);
  set_selected(*impl_, &shooter);
  if (!call_method(*impl_, weapon, "GetTargetScore", c, [&](lua_State* L) {
        lua::push_point(L, origin);
        lua::push_point(L, target);
        return 2;
      })) {
    return 0;
  }
  if (lua_type(impl_->L, -1) != LUA_TNUMBER) {
    fail(c, weapon, "GetTargetScore", "cast_failed: did not return a number");
    return 0;
  }
  return lua::to_int(impl_->L, -1);
}

int LuaHost::score_positioning(const Board& board, const Pawn& pawn, Point point, LuaCall* call) {
  LuaCall local;
  LuaCall& c = call ? *call : local;
  Scope scope(*impl_, &board, &c);
  set_selected(*impl_, &pawn);
  flush_selected(*impl_);
  lua_State* L = impl_->L;
  lua_getglobal(L, "ScorePositioning");
  lua::push_point(L, point);
  lua::push_pawn(L, &pawn);
  if (lua_pcall(L, 2, 1, 0) != 0) {
    fail(c, "global", "ScorePositioning", lua::to_str(L, -1));
    return 0;
  }
  if (lua_type(L, -1) != LUA_TNUMBER) {
    fail(c, "global", "ScorePositioning", "cast_failed: did not return a number");
    return 0;
  }
  return lua::to_int(L, -1);
}

bool LuaHost::is_two_click(std::string_view weapon, LuaCall* call) {
  const std::string name(weapon);
  if (auto it = impl_->two_click.find(name); it != impl_->two_click.end()) return it->second;
  LuaCall local;
  LuaCall& c = call ? *call : local;
  LuaCall probe;
  Scope scope(*impl_, nullptr, &probe);
  // LuaData::GetBool("TwoClick"): false unless the table and getter exist.
  lua_State* L = impl_->L;
  flush_selected(*impl_);
  lua_getglobal(L, name.c_str());
  if (!lua_istable(L, -1)) return false;
  lua_getfield(L, -1, "GetTwoClick");
  if (!lua_toboolean(L, -1)) return false;
  if (!call_method(*impl_, weapon, "GetTwoClick", probe, [](lua_State*) { return 0; })) {
    c.ok = false;
    if (c.error.empty()) c.error = probe.error;
    return false;
  }
  const bool two = lua_toboolean(L, -1) != 0;
  // A clean call (no writes, no output) of a table field getter: cache it.
  if (probe.ok && probe.writes.empty() && probe.console.empty()) {
    impl_->two_click.emplace(name, two);
  } else {
    c.writes.insert(c.writes.end(), probe.writes.begin(), probe.writes.end());
    c.console.insert(c.console.end(), probe.console.begin(), probe.console.end());
  }
  return two;
}

std::vector<Point> LuaHost::second_target_area(const Board& board, const Pawn& shooter,
                                               std::string_view weapon, Point origin, Point first,
                                               LuaCall* call) {
  LuaCall local;
  LuaCall& c = call ? *call : local;
  Scope scope(*impl_, &board, &c);
  set_selected(*impl_, &shooter);
  std::vector<Point> area;
  if (!call_method(*impl_, weapon, "GetSecondTargetArea", c, [&](lua_State* L) {
        lua::push_point(L, origin);
        lua::push_point(L, first);
        return 2;
      })) {
    return area;
  }
  if (!take_point_list(impl_->L, area, c, weapon, "GetSecondTargetArea")) return {};
  std::erase_if(area, [](Point p) { return p.x < 0 || p.y < 0; });
  return area;
}

bool LuaHost::two_click_exception(const Board& board, const Pawn& shooter, std::string_view weapon,
                                  Point origin, Point target, LuaCall* call) {
  LuaCall local;
  LuaCall& c = call ? *call : local;
  Scope scope(*impl_, &board, &c);
  set_selected(*impl_, &shooter);
  if (!call_method(*impl_, weapon, "IsTwoClickException", c, [&](lua_State* L) {
        lua::push_point(L, origin);
        lua::push_point(L, target);
        return 2;
      })) {
    return false;
  }
  lua_State* L = impl_->L;
  if (lua_type(L, -1) != LUA_TBOOLEAN) {
    fail(c, weapon, "IsTwoClickException", std::string("cast_failed: returned ") + lua_type_label(L, -1));
    return false;
  }
  return lua_toboolean(L, -1) != 0;
}

Point LuaHost::translate_first_click(const Board& board, const Pawn& shooter, std::string_view weapon,
                                     Point origin, Point target, LuaCall* call) {
  LuaCall local;
  LuaCall& c = call ? *call : local;
  Scope scope(*impl_, &board, &c);
  set_selected(*impl_, &shooter);
  if (!call_method(*impl_, weapon, "TranslateFirstClick", c, [&](lua_State* L) {
        lua::push_point(L, origin);
        lua::push_point(L, target);
        return 2;
      })) {
    return target;
  }
  lua::Instance* in = lua::to_instance(impl_->L, -1, Cls::Point);
  if (!in) {
    fail(c, weapon, "TranslateFirstClick",
         std::string("cast_failed: returned ") + lua_type_label(impl_->L, -1) + ", expected Point");
    return target;
  }
  return *static_cast<Point*>(lua::resolve(in));
}

LuaSkillEffect LuaHost::final_effect_raw(const Board& board, const Pawn& shooter,
                                         std::string_view weapon, Point origin, Point first,
                                         Point target, LuaCall* call) {
  LuaCall local;
  LuaCall& c = call ? *call : local;
  Scope scope(*impl_, &board, &c);
  set_selected(*impl_, &shooter);
  LuaSkillEffect se;
  // Three points travel as one PointList {origin, first, target}.
  if (!call_method(*impl_, weapon, "GetFinalEffect_Helper", c, [&](lua_State* L) {
        lua::push_point_list(L, {origin, first, target});
        return 1;
      })) {
    return se;
  }
  if (!take_skill_effect(impl_->L, se, c, weapon, "GetFinalEffect_Helper")) return {};
  return se;
}

LuaSkillEffect LuaHost::death_effect_raw(const Board& board, const Pawn& dying,
                                         std::optional<uint32_t> seed, const Pawn* selected,
                                         LuaCall* call) {
  LuaCall local;
  LuaCall& c = call ? *call : local;
  Scope scope(*impl_, &board, &c);
  set_selected(*impl_, selected);
  if (seed) lua::rng(impl_->L).srand(*seed);
  // The default Pawn:GetDeathEffect returns an empty SkillEffect.
  if (uses_default_death_effect(*impl_, dying.type)) return {};
  const std::string type(symbol_name(dying.type));
  LuaSkillEffect se;
  if (!call_method(*impl_, type, "GetDeathEffect", c, [&](lua_State* L) {
        lua::push_point(L, dying.pos);
        return 1;
      })) {
    return se;
  }
  if (!take_skill_effect(impl_->L, se, c, type, "GetDeathEffect")) return {};
  return se;
}

SkillEffect LuaHost::death_effect(const Board& board, const Pawn& dying, std::optional<uint32_t> seed,
                                  const Pawn* selected, LuaCall* call) {
  return to_engine(death_effect_raw(board, dying, seed, selected, call));
}

LuaCall LuaHost::run_script(const Board& board, std::string_view code, const Pawn* selected) {
  LuaCall c;
  Scope scope(*impl_, &board, &c);
  if (selected) set_selected(*impl_, selected);
  lua_State* L = impl_->L;
  flush_selected(*impl_);
  // LuaEnv::DirectLua: chunk name "line", errors returned (and ignored in game).
  const std::string key(code);
  if (auto it = impl_->chunks.find(key); it != impl_->chunks.end()) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, it->second);
  } else if (luaL_loadbuffer(L, code.data(), code.size(), "line") != 0) {
    c.ok = false;
    c.error = lua::to_str(L, -1);
    return c;
  } else {
    lua_pushvalue(L, -1);
    impl_->chunks.emplace(key, luaL_ref(L, LUA_REGISTRYINDEX));
  }
  if (lua_pcall(L, 0, 0, 0) != 0) {
    c.ok = false;
    c.error = lua::to_str(L, -1);
  }
  return c;
}

void LuaHost::select(const Pawn* pawn) { set_selected(*impl_, pawn); }

void LuaHost::set_native_weapons(bool on) { impl_->native_weapons = on; }

bool LuaHost::native_weapons() const { return impl_->native_weapons; }

std::string LuaHost::native_port(std::string_view weapon, std::string_view method) {
  const lua::WeaponPort* port = impl_->ports.find(impl_->L, weapon);
  const lua::PortSpec* spec = !port ? nullptr : method == "GetTargetArea" ? port->area : method == "GetSkillEffect" ? port->effect : nullptr;
  return spec ? std::string(spec->file) + ":" + std::to_string(spec->line) : std::string();
}

uint64_t LuaHost::native_calls() const { return impl_->native_calls; }

uint64_t LuaHost::temp_pawns() const { return impl_->temp_pawns; }

std::string LuaHost::native_binding(std::string_view weapon) {
  const lua::WeaponPort* port = impl_->ports.find(impl_->L, weapon);
  if (!port) return {};
  std::string out;
  auto add = [&](const char* what, const lua::PortSpec* spec, const lua::Fields& fields) {
    if (!spec) return;
    out += std::string(what) + "=" + spec->file + ":" + std::to_string(spec->line) + "(";
    for (const lua::FieldValue& f : fields) {
      out += std::to_string(f.type) + ":" + (f.b ? "t" : "f") + ":" + lua::native::number_string(f.n) + ":" + f.s + ";";
    }
    out += ")";
  };
  add("area", port->area, port->area_fields);
  add("effect", port->effect, port->effect_fields);
  return out;
}

std::string LuaHost::native_port_status(std::string_view weapon, std::string_view method) {
  const std::string m(method);
  const lua::LuaFunctionId id = impl_->ports.function_of(impl_->L, weapon, m.c_str());
  const std::string why = impl_->ports.why_not(impl_->L, weapon, m.c_str());
  char hash[32];
  std::snprintf(hash, sizeof hash, "%016llx", static_cast<unsigned long long>(id.hash));
  return (id.file.empty() ? std::string("(not a script function)") : id.key() + "-" + std::to_string(id.last_line) + " " + hash) +
         (why.empty() ? std::string(" ported") : " lua: " + why);
}

void LuaHost::set_call_counting(bool on) { impl_->count_calls = on; }

std::vector<std::pair<std::string, uint64_t>> LuaHost::call_counts() const {
  std::vector<std::pair<std::string, uint64_t>> out(impl_->call_counts.begin(), impl_->call_counts.end());
  std::sort(out.begin(), out.end());
  return out;
}

void LuaHost::seed(uint32_t s) { lua::rng(impl_->L).srand(s); }

uint64_t LuaHost::rand_draws() const { return lua::rng(impl_->L).draws(); }

int LuaHost::rand() { return lua::rng(impl_->L).rand(); }

std::vector<std::string> LuaHost::weapon_ids() const {
  lua_State* L = impl_->L;
  const int top = lua_gettop(L);
  std::vector<std::string> out;
  lua_getglobal(L, "Skill");
  const int base = lua_gettop(L);
  lua_pushnil(L);
  while (lua_next(L, LUA_GLOBALSINDEX) != 0) {
    if (lua_type(L, -2) == LUA_TSTRING && lua_istable(L, -1) && !lua_rawequal(L, -1, base)) {
      lua_pushvalue(L, -1);
      for (int depth = 0; depth < 32 && lua_getmetatable(L, -1); ++depth) {
        lua_remove(L, -2);
        if (lua_rawequal(L, -1, base)) {
          out.push_back(lua::to_str(L, -3));
          break;
        }
      }
      lua_pop(L, 1);
    }
    lua_pop(L, 1);
  }
  lua_settop(L, top);
  std::sort(out.begin(), out.end());
  return out;
}

namespace {

// Pushes table[field] (through __index) or returns false.
bool push_field(LuaHost::Impl& im, std::string_view table, std::string_view field) {
  lua_State* L = im.L;
  flush_selected(im);
  const std::string t(table), f(field);
  lua_getglobal(L, t.c_str());
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    return false;
  }
  lua_getfield(L, -1, f.c_str());
  lua_remove(L, -2);
  return true;
}

}  // namespace

std::optional<std::string> LuaHost::lua_string(std::string_view table, std::string_view field) const {
  lua_State* L = impl_->L;
  if (!push_field(*impl_, table, field)) return std::nullopt;
  std::optional<std::string> v;
  if (lua_type(L, -1) == LUA_TSTRING) v = lua::to_str(L, -1);
  lua_pop(L, 1);
  return v;
}

std::optional<double> LuaHost::lua_number(std::string_view table, std::string_view field) const {
  lua_State* L = impl_->L;
  if (!push_field(*impl_, table, field)) return std::nullopt;
  std::optional<double> v;
  if (lua_type(L, -1) == LUA_TNUMBER) v = lua_tonumber(L, -1);
  lua_pop(L, 1);
  return v;
}

std::optional<bool> LuaHost::lua_bool(std::string_view table, std::string_view field) const {
  lua_State* L = impl_->L;
  if (!push_field(*impl_, table, field)) return std::nullopt;
  std::optional<bool> v;
  if (lua_type(L, -1) == LUA_TBOOLEAN) v = lua_toboolean(L, -1) != 0;
  lua_pop(L, 1);
  return v;
}

}  // namespace itb
