// Board, BoardPawn, GameMap, ValueBar and PawnFactory bindings for the Lua
// host. Queries read the engine Board the host is currently pointed at;
// mutators are recorded as LuaWrites (see lua_host.hpp). Semantics follow
// notes/stage6_lua_api_spec.md §3B/§3P; tile and path predicates reuse the
// stage 2 and stage 5 rules.

#include <algorithm>
#include <climits>
#include <cmath>
#include <string>
#include <vector>

#include "itb/movement.hpp"
#include "itb/tile_rules.hpp"
#include "lua_host_internal.hpp"
#include "lua_native.hpp"

extern "C" {
#include "lauxlib.h"
}

namespace itb::lua {
namespace {

char kHostKey;

// ---- context -----------------------------------------------------------------

const Board& board_of(lua_State* L) {
  const Board* b = host_context(L).board;
  if (!b) throw LuaError("no board is set (call outside a host call)");
  return *b;
}

const Tile kDummyTile{};

// Getters on an off-board point read the dummy tile (a default ROAD tile).
const Tile& tile_at(const Board& b, Point p) { return p.valid() ? b.tile(p) : kDummyTile; }

// ---- pawns -----------------------------------------------------------------------

const Pawn& pawn_ref(lua_State* L, int idx) {
  Instance* in = to_instance(L, idx, Cls::BoardPawn);
  if (!in) throw LuaError("expected BoardPawn");
  const int32_t uid = static_cast<int32_t>(static_cast<uint32_t>(in->index));
  const Pawn* p = board_of(L).find_pawn(uid);
  if (!p) throw LuaError("pawn " + std::to_string(uid) + " is not on the current board");
  return *p;
}

const Pawn* pawn_ptr_arg(lua_State* L, int idx) {
  if (lua_isnil(L, idx)) return nullptr;
  return &pawn_ref(L, idx);
}

// The occupants of p (not fallen), in board-list order.
std::vector<const Pawn*> occupants_of(const Board& b, Point p) {
  std::vector<const Pawn*> out;
  if (!p.valid()) return out;
  for (const Pawn& pawn : b.pawns()) {
    if (pawn.occupies(p) && !pawn.fallen) out.push_back(&pawn);
  }
  return out;
}

// BoardSpace::IsPawnSpace(true): some occupant alive or a corpse.
bool occupied(const Board& b, Point p) { return p.valid() && has_pawn(b, p); }

// occ[0] of an occupied tile (GetPawn(Point)); null otherwise.
const Pawn* occ0(const Board& b, Point p) {
  if (!occupied(b, p)) return nullptr;
  const auto occ = occupants_of(b, p);
  return occ.empty() ? nullptr : occ.front();
}

bool lua_type_flag(lua_State* L, const Pawn& p, const char* field, bool fallback);
int lua_type_int(lua_State* L, const Pawn& p, const char* field, int fallback);

// Pawn::IsTeam (spec §3B.1.2).
bool is_team(lua_State* L, const Pawn& p, int t) {
  const int team = static_cast<int>(p.team);
  switch (t) {
    case 2:
      return true;
    case 8:
      return p.faction == Faction::Bots;
    case 7:
      return team == 6 && p.faction == Faction::Default;
    case 6:
      return team >= 6;
    case -1:
      if (team >= 6 && !p.minor && lua_type_flag(L, p, "SpawnLimit", true)) return true;
      return team == -1;
    case 4:
      return p.mech || team == 4;
    case 5:
      return (!p.neutral && team == 1) || team == 5;
    default:
      return team == t;
  }
}

bool is_boosted(const Board& b, const Pawn& p) {
  return p.boosted || mutation_affects(b, p, Leader::Boosted) ||
         (p.has_pilot(kPilotArrogantBoost) && p.hp >= p.max_hp);
}

Pathing raw_pathing(int pr) { return Pathing{static_cast<PathProfile>(pr & 15), pr >> 4}; }

// Lua pawn-type lookups (LuaData::GetBool/GetInt): CallMethod(type, "Get"..field).
bool lua_type_value(lua_State* L, const Pawn& p, const char* field) {
  const std::string type(symbol_name(p.type));
  if (type.empty()) return false;
  const std::string getter = std::string("Get") + field;
  lua_getglobal(L, type.c_str());
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    return false;
  }
  lua_getfield(L, -1, getter.c_str());
  const bool has = !lua_isnil(L, -1) && !(lua_isboolean(L, -1) && !lua_toboolean(L, -1));
  lua_pop(L, 2);
  if (!has) return false;
  lua_getglobal(L, "CallMethod");
  lua_pushlstring(L, type.data(), type.size());
  lua_pushlstring(L, getter.data(), getter.size());
  if (lua_pcall(L, 2, 1, 0) != 0) {
    lua_pop(L, 1);
    return false;
  }
  return true;  // value on the stack
}

bool lua_type_flag(lua_State* L, const Pawn& p, const char* field, bool fallback) {
  if (!lua_type_value(L, p, field)) return fallback;
  const bool v = lua_toboolean(L, -1) != 0;
  lua_pop(L, 1);
  return v;
}

int lua_type_int(lua_State* L, const Pawn& p, const char* field, int fallback) {
  if (!lua_type_value(L, p, field)) return fallback;
  const int v = lua_type(L, -1) == LUA_TNUMBER ? to_int(L, -1) : fallback;
  lua_pop(L, 1);
  return v;
}

// Pilot skill names the engine models (pilots.lua Skill values).
bool has_ability(const Pawn& p, const std::string& name) {
  static const std::pair<const char*, PilotAbility> kNames[] = {
      {"Armored", kPilotArmored},          {"Thick", kPilotThick},
      {"Rock_Skill", kPilotRockSkill},     {"Retaliation", kPilotRetaliation},
      {"Flying", kPilotFlying},            {"Disable_Immunity", kPilotDisableImmunity},
      {"Freeze_Walk", kPilotFreezeWalk},   {"Pain_Immunity", kPilotPainImmunity},
      {"Road_Runner", kPilotRoadRunner},   {"Shifty", kPilotShifty},
      {"Post_Move", kPilotPostMove},       {"Double_Shot", kPilotDoubleShot},
      {"Youth_Move", kPilotYouthMove},     {"Arrogant_Boost", kPilotArrogantBoost},
  };
  for (const auto& [n, bit] : kNames) {
    if (name == n) return p.has_pilot(bit);
  }
  return false;
}

// ---- write recording -----------------------------------------------------------------

void record_write(lua_State* L, const char* object, const char* method, int first_arg,
                  int32_t pawn = -1) {
  LuaWrite w;
  w.object = object;
  w.method = method;
  w.pawn = pawn;
  for (int i = first_arg; i <= lua_gettop(L); ++i) {
    switch (lua_type(L, i)) {
      case LUA_TBOOLEAN:
        w.args.emplace_back(lua_toboolean(L, i) != 0);
        break;
      case LUA_TNUMBER:
        w.args.emplace_back(static_cast<double>(lua_tonumber(L, i)));
        break;
      case LUA_TSTRING:
        w.args.emplace_back(to_str(L, i));
        break;
      default:
        if (Instance* in = to_instance(L, i)) {
          if (in->cls == Cls::Point) {
            w.args.emplace_back(*static_cast<Point*>(resolve(in)));
          } else if (in->cls == Cls::SkillEffect && !w.effect) {
            w.effect = *static_cast<LuaSkillEffect*>(resolve(in));
            w.args.emplace_back(std::string("<SkillEffect>"));
          } else if (in->cls == Cls::SpaceDamage && !w.effect) {
            w.effect.emplace();
            w.effect->effect.push_back(*static_cast<LuaSpaceDamage*>(resolve(in)));
            w.args.emplace_back(std::string("<SpaceDamage>"));
          } else if (in->cls == Cls::BoardPawn) {
            w.args.emplace_back(static_cast<double>(static_cast<int32_t>(in->index)));
          } else {
            w.args.emplace_back(std::string("<") + class_name(in->cls) + ">");
          }
        } else {
          w.args.emplace_back(std::monostate{});
        }
    }
  }
  HostContext& ctx = host_context(L);
  if (ctx.call) ctx.call->writes.push_back(std::move(w));
}

// A mutating binding: checked against its overloads, recorded, then the
// declared default result is pushed.
enum class Ret : uint8_t { Void, Point, Int, Bool, Pawn };

struct Mutator {
  const char* name;
  std::vector<std::vector<A>> sigs;  // without self
  Ret ret = Ret::Void;
  bool record = true;  // false: cosmetic/UI, nothing to report
};

bool match_tail(lua_State* L, A self, const std::vector<A>& sig) {
  if (lua_gettop(L) != static_cast<int>(sig.size()) + 1) return false;
  if (!match_one(L, 1, self)) return false;
  for (size_t i = 0; i < sig.size(); ++i) {
    if (!match_one(L, static_cast<int>(i) + 2, sig[i])) return false;
  }
  return true;
}

int run_mutator(lua_State* L, const char* object, A self, const Mutator& m) {
  const bool ok = std::any_of(m.sigs.begin(), m.sigs.end(),
                              [&](const std::vector<A>& s) { return match_tail(L, self, s); });
  if (!ok) no_overload((std::string(object) + ":" + m.name + "(...)").c_str());
  if (m.record) {
    int32_t uid = -1;
    if (self == A::Pawn) uid = pawn_ref(L, 1).uid;
    record_write(L, object, m.name, 2, uid);
  }
  switch (m.ret) {
    case Ret::Void: return 0;
    case Ret::Point: push_point(L, kInvalidPoint); return 1;
    case Ret::Int: lua_pushinteger(L, m.name == std::string("FireWeapon") ? 0 : -1); return 1;
    case Ret::Bool: lua_pushboolean(L, 0); return 1;
    case Ret::Pawn: lua_pushnil(L); return 1;
  }
  return 0;
}

// ---- Board queries -------------------------------------------------------------------


int b_is_valid(lua_State* L) {
  if (match(L, {A::Board, A::Point})) {
    lua_pushboolean(L, point_arg(L, 2).valid());
  } else if (match(L, {A::Board, A::Int, A::Int})) {
    lua_pushboolean(L, Point{to_int(L, 2), to_int(L, 3)}.valid());
  } else {
    no_overload("bool IsValid(Board&,Point)\nbool IsValid(Board&,int,int)");
  }
  return 1;
}

int b_get_size(lua_State* L) {
  if (!match(L, {A::Board})) no_overload("Point GetSize(Board&)");
  push_point(L, Point{kBoardSize, kBoardSize});
  return 1;
}

int b_is_blocked(lua_State* L) {
  if (!match(L, {A::Board, A::Point, A::Int})) no_overload("bool IsBlocked(Board&,Point,int)");
  lua_pushboolean(L, native::is_blocked(board_of(L), point_arg(L, 2), to_int(L, 3)));
  return 1;
}

int b_is_pawn_space(lua_State* L) {
  const Board& b = board_of(L);
  if (match(L, {A::Board, A::Point})) {
    lua_pushboolean(L, native::is_pawn_space(b, point_arg(L, 2)));
  } else if (match(L, {A::Board, A::Point, A::Bool})) {
    const Point p = point_arg(L, 2);
    lua_pushboolean(L, to_bool(L, 3) ? occupied(b, p) : !occupants_of(b, p).empty());
  } else {
    no_overload("bool IsPawnSpace(Board&,Point)\nbool IsPawnSpace(Board&,Point,bool)");
  }
  return 1;
}

int b_get_pawn(lua_State* L) {
  const Board& b = board_of(L);
  const Pawn* found = nullptr;
  if (match(L, {A::Board, A::Point})) {
    found = native::pawn_at(b, point_arg(L, 2));
  } else if (match(L, {A::Board, A::Point, A::Bool})) {
    const Point p = point_arg(L, 2);
    const bool live = to_bool(L, 3);
    const auto occ = occupants_of(b, p);
    if (live ? occupied(b, p) : !occ.empty()) {
      auto alive = std::find_if(occ.begin(), occ.end(), [](const Pawn* x) { return x->alive(); });
      found = alive != occ.end() ? *alive : occ.back();
    }
  } else if (match(L, {A::Board, A::Int})) {
    found = b.find_pawn(to_int(L, 2));
  } else {
    no_overload("Pawn* GetPawn(Board&,Point)\nPawn* GetPawn(Board&,Point,bool)\nPawn* GetPawn(Board&,int)");
  }
  push_pawn(L, found);
  return 1;
}

int b_is_pawn_alive(lua_State* L) {
  if (!match(L, {A::Board, A::Int})) no_overload("bool IsPawnAlive(Board&,int)");
  const Pawn* p = board_of(L).find_pawn(to_int(L, 2));
  lua_pushboolean(L, p && p->alive());
  return 1;
}

int b_get_pawn_space(lua_State* L) {
  if (!match(L, {A::Board, A::Int})) no_overload("Point GetPawnSpace(Board&,int)");
  const Pawn* p = board_of(L).find_pawn(to_int(L, 2));
  push_point(L, p ? p->pos : kInvalidPoint);
  return 1;
}

int b_get_pawn_team(lua_State* L) {
  Point p;
  if (match(L, {A::Board, A::Point})) {
    p = point_arg(L, 2);
  } else if (match(L, {A::Board, A::Int, A::Int})) {
    p = Point{to_int(L, 2), to_int(L, 3)};
  } else {
    no_overload("int GetPawnTeam(Board&,Point)\nint GetPawnTeam(Board&,int,int)");
  }
  lua_pushinteger(L, native::pawn_team(board_of(L), p));
  return 1;
}

int b_is_pawn_team(lua_State* L) {
  Point p;
  int t = 0;
  if (match(L, {A::Board, A::Point, A::Int})) {
    p = point_arg(L, 2);
    t = to_int(L, 3);
  } else if (match(L, {A::Board, A::Int, A::Int, A::Int})) {
    p = Point{to_int(L, 2), to_int(L, 3)};
    t = to_int(L, 4);
  } else {
    no_overload("bool IsPawnTeam(Board&,Point,int)\nbool IsPawnTeam(Board&,int,int,int)");
  }
  lua_pushboolean(L, native::is_pawn_team(L, board_of(L), p, t));
  return 1;
}

Point point_or_xy(lua_State* L, const char* name) {
  if (match(L, {A::Board, A::Point})) return point_arg(L, 2);
  if (match(L, {A::Board, A::Int, A::Int})) return Point{to_int(L, 2), to_int(L, 3)};
  no_overload((std::string(name) + "(Board&,Point)\n" + name + "(Board&,int,int)").c_str());
}

int b_get_terrain(lua_State* L) {
  const Point p = point_or_xy(L, "int GetTerrain");
  lua_pushinteger(L, native::terrain(board_of(L), p));
  return 1;
}

int b_is_building(lua_State* L) {
  const Point p = point_or_xy(L, "bool IsBuilding");
  lua_pushboolean(L, native::is_building(board_of(L), p));
  return 1;
}

int b_is_terrain(lua_State* L) {
  if (!match(L, {A::Board, A::Point, A::Int})) no_overload("bool IsTerrain(Board&,Point,int)");
  const Tile& t = tile_at(board_of(L), point_arg(L, 2));
  const int q = to_int(L, 3);
  bool r = false;
  switch (q) {
    case 14: r = t.terrain == Terrain::Water && t.lava; break;
    case 16: r = t.cracked; break;
    case 18: r = t.terrain == Terrain::Ice && t.hp == 1; break;
    default: r = static_cast<int>(t.terrain) == q; break;
  }
  lua_pushboolean(L, r);
  return 1;
}

// One-Point tile predicates.
template <bool (*F)(const Board&, Point)>
int tile_query(lua_State* L) {
  if (!match(L, {A::Board, A::Point})) no_overload("bool <query>(Board&,Point)");
  lua_pushboolean(L, F(board_of(L), point_arg(L, 2)));
  return 1;
}

bool q_powered(const Board& b, Point p) { return p.valid() && is_populated(b.tile(p)); }
bool q_fire(const Board& b, Point p) { return tile_at(b, p).on_fire(); }
bool q_smoke(const Board& b, Point p) { return tile_at(b, p).smoke; }
bool q_acid(const Board& b, Point p) { return tile_at(b, p).acid; }
bool q_frozen(const Board& b, Point p) { return p.valid() && tile_frozen(b, p); }
bool q_vines(const Board& b, Point p) { return tile_at(b, p).vines; }
bool q_pod(const Board& b, Point p) { return tile_at(b, p).pod == PodState::Present; }
bool q_item(const Board& b, Point p) { return tile_at(b, p).item != kNoSymbol; }
bool q_crackable(const Board& b, Point p) { return p.valid() && is_crackable(b, p); }
bool q_cracked(const Board& b, Point p) { return tile_at(b, p).cracked; }
bool q_damaged(const Board& b, Point p) {
  const Tile& t = tile_at(b, p);
  return t.hp < t.max_hp;
}
bool q_edge(const Board&, Point p) {
  return p.x == 0 || p.y == 0 || p.x == kBoardSize - 1 || p.y == kBoardSize - 1;
}
bool q_unique_building(const Board& b, Point p) { return tile_at(b, p).unique_building != kNoSymbol; }
bool q_spawning(const Board& b, Point p) {
  return std::find(b.spawn_points.begin(), b.spawn_points.end(), p) != b.spawn_points.end();
}
// Spikes and teleporter pads; SetDangerous marks are not part of the Board.
bool q_dangerous(const Board& b, Point p) {
  const Tile& t = tile_at(b, p);
  return t.spikes || t.teleporter;
}
bool q_env_danger(const Board&, Point) { return false; }

int b_get_item(lua_State* L) {
  if (!match(L, {A::Board, A::Point})) no_overload("std::string GetItem(Board&,Point)");
  const std::string_view s = symbol_name(tile_at(board_of(L), point_arg(L, 2)).item);
  lua_pushlstring(L, s.data(), s.size());
  return 1;
}

int b_get_custom_tile(lua_State* L) {
  if (!match(L, {A::Board, A::Point})) no_overload("std::string GetCustomTile(Board&,Point)");
  const std::string_view s = symbol_name(tile_at(board_of(L), point_arg(L, 2)).custom_tile);
  lua_pushlstring(L, s.data(), s.size());
  return 1;
}

int b_get_health(lua_State* L) {
  if (!match(L, {A::Board, A::Point})) no_overload("int GetHealth(Board&,Point)");
  lua_pushinteger(L, tile_at(board_of(L), point_arg(L, 2)).hp);
  return 1;
}

int b_is_wall(lua_State* L) {
  if (!match(L, {A::Board, A::Point, A::Int, A::Int})) no_overload("bool IsWall(Board&,Point,int,int)");
  lua_pushboolean(L, wall_blocks(board_of(L), point_arg(L, 2), static_cast<Dir>(to_int(L, 3)),
                                 raw_pathing(to_int(L, 4))));
  return 1;
}

int b_is_busy(lua_State* L) {
  if (!match(L, {A::Board})) no_overload("bool IsBusy(Board&)");
  lua_pushboolean(L, 0);  // the host resolves instantly
  return 1;
}

int b_get_busy_state(lua_State* L) {
  if (!match(L, {A::Board})) no_overload("int GetBusyState(Board&)");
  lua_pushinteger(L, 0);
  return 1;
}

int b_get_turn(lua_State* L) {
  if (!match(L, {A::Board})) no_overload("int GetTurn(Board&)");
  lua_pushinteger(L, board_of(L).turn);
  return 1;
}

// SpaceDamage::Boost.
void boost(LuaSpaceDamage& sd) {
  if (sd.iDamage > 0 && sd.iDamage != kDamageZero && sd.iDamage != kDamageDeath) {
    ++sd.iDamage;
  } else if (sd.iDamage < 0 && sd.iDamage > -10) {
    --sd.iDamage;
  }
  sd.boosted = true;
}

// Board::IsSkillDeadly (spec §3B.1.5): would this hit kill the pawn on its tile?
bool is_deadly(const Board& b, LuaSpaceDamage sd, const Pawn* attacker) {
  if (attacker && is_boosted(b, *attacker)) boost(sd);
  const Pawn* target = occ0(b, sd.loc);
  if (!target || !target->alive()) return false;
  int dmg = sd.iDamage == kDamageZero ? 0 : sd.iDamage;
  const bool shielded = target->shield || is_turn_shielded(b, *target);
  if (dmg > 0 && (shielded || target->frozen) && dmg != kDamageDeath) dmg = 0;
  if (dmg <= 0) return false;
  const Tile& t = b.tile(sd.loc);
  const bool flying = is_flying(*target);
  if (t.terrain == Terrain::Ice && t.hp == 1 && !flying && !is_massive(b, *target)) return true;
  if (t.cracked && !flying) return true;
  if (dmg != kDamageDeath) {
    if (is_armored(b, *target)) dmg = std::max(dmg - 1, 0);
    if (target->acid) dmg *= 2;
  }
  return target->hp - dmg < 1;
}

int b_is_deadly(lua_State* L) {
  if (!match(L, {A::Board, A::SD, A::PawnPtr})) no_overload("bool IsDeadly(Board&,SpaceDamage,Pawn*)");
  lua_pushboolean(L, is_deadly(board_of(L), sd_arg(L, 2), pawn_ptr_arg(L, 3)));
  return 1;
}

int b_get_path(lua_State* L) {
  if (!match(L, {A::Board, A::Point, A::Point, A::Int})) no_overload("PointList GetPath(Board&,Point,Point,int)");
  push_point_list(L, native::path(board_of(L), point_arg(L, 2), point_arg(L, 3), to_int(L, 4)));
  return 1;
}

// GridSearchable::GetSingleCornerPath with the projectile profile (spec §3B.1.4,
// S5 §4.4): for each first direction (DIR order) that closes in on the goal,
// walk straight while the next tile is closer and passable (or the goal), then
// turn toward the goal and walk the same way; the first attempt that ends on
// the goal wins.
std::vector<Point> simple_path(const Board& b, Point from, Point to) {
  if (from == to || !to.valid()) return {};
  const Pathing pr = Pathing::lua(PathProfile::Projectile);
  auto dist = [&](Point p) { return std::abs(to.x - p.x) + std::abs(to.y - p.y); };
  auto walk = [&](std::vector<Point>& path, Point& cur, Point v) {
    while (true) {
      const Point next = cur + v;
      if (dist(next) >= dist(cur)) return;
      if (!can_pass(b, next, pr) && next != to) return;
      cur = next;
      path.push_back(cur);
    }
  };
  for (Point first : kDirVectors) {
    std::vector<Point> path{from};
    Point cur = from;
    if (dist(cur + first) >= dist(cur)) continue;
    walk(path, cur, first);
    const Point rest = to - cur;
    if (!(rest.x == 0 && rest.y == 0)) {
      const int d = std::abs(rest.x) > std::abs(rest.y) ? (rest.x > 0 ? 1 : 3) : (rest.y > 0 ? 2 : 0);
      walk(path, cur, kDirVectors[d]);
    }
    if (cur == to) return path;
  }
  return {};
}

int b_get_simple_path(lua_State* L) {
  if (!match(L, {A::Board, A::Point, A::Point})) no_overload("PointList GetSimplePath(Board&,Point,Point)");
  push_point_list(L, native::simple_path(board_of(L), point_arg(L, 2), point_arg(L, 3)));
  return 1;
}

// GridSearchable::GetOrthDestArea with the projectile profile: for each
// direction, the passable tiles in a straight line up to `len` (each optionally
// with orthogonal legs), then the first non-passable tile at the line's end.
std::vector<Point> orth_dest_area(const Board& b, Point start, int len, bool corners) {
  const Pathing pr = Pathing::lua(PathProfile::Projectile);
  std::vector<Point> out;
  auto add_unique = [&](Point p) {
    if (std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
  };
  for (int d = 0; d < 4; ++d) {
    const Point v = kDirVectors[d];
    Point cur = start;
    int steps = 0;
    while (true) {
      const Point next = cur + v;
      if (!can_pass(b, next, pr) || !next.valid() || steps >= len) break;
      cur = next;
      add_unique(cur);
      ++steps;
      if (!corners) continue;
      // Orthogonal legs: DOWN then UP for a horizontal line, RIGHT then LEFT
      // for a vertical one (Globals::GetOrthogonalDirs).
      const int legs[2] = {d % 2 == 1 ? 2 : 1, d % 2 == 1 ? 0 : 3};
      for (int leg : legs) {
        const Point w = kDirVectors[leg];
        Point q = cur;
        int leg_steps = steps;
        while (true) {
          const Point nq = q + w;
          if (!can_pass(b, nq, pr) || !nq.valid() || leg_steps >= len) {
            if (nq.valid()) out.push_back(nq);  // no duplicate check here (as in game)
            break;
          }
          q = nq;
          add_unique(q);
          ++leg_steps;
        }
      }
    }
    const Point end = cur + v;
    if (end.valid() && steps < len && std::find(out.begin(), out.end(), end) == out.end()) {
      out.push_back(end);
    }
  }
  return out;
}

int b_get_simple_reachable(lua_State* L) {
  if (!match(L, {A::Board, A::Point, A::Int, A::Bool})) {
    no_overload("PointList GetSimpleReachable(Board&,Point,int,bool)");
  }
  push_point_list(L, native::simple_reachable(board_of(L), point_arg(L, 2), to_int(L, 3), to_bool(L, 4)));
  return 1;
}

int b_get_reachable(lua_State* L) {
  if (!match(L, {A::Board, A::Point, A::Int, A::Int})) no_overload("PointList GetReachable(Board&,Point,int,int)");
  push_point_list(L, native::reachable(board_of(L), point_arg(L, 2), to_int(L, 3), to_int(L, 4)));
  return 1;
}

int b_get_distance(lua_State* L) {
  if (!match(L, {A::Board, A::Point, A::Point, A::Int})) no_overload("int GetDistance(Board&,Point,Point,int)");
  const Point from = point_arg(L, 2), to = point_arg(L, 3);
  if (!to.valid()) {
    lua_pushinteger(L, 0);
    return 1;
  }
  const DistanceMap d = distance_map(board_of(L), from, raw_pathing(to_int(L, 4)), 5);
  lua_pushinteger(L, d[to.index()] <= 5 ? d[to.index()] : INT_MAX);
  return 1;
}

std::vector<int> pawn_ids(lua_State* L, const Board& b, int team) {
  std::vector<int> ids;
  for (const Pawn& p : b.pawns()) {
    if ((p.alive() || is_corpse(b, p) || p.mech) && is_team(L, p, team)) ids.push_back(p.uid);
  }
  return ids;
}

int b_get_pawns(lua_State* L) {
  if (!match(L, {A::Board, A::Int})) no_overload("IntList GetPawns(Board&,int)");
  push_int_list(L, native::pawn_ids(L, board_of(L), to_int(L, 2)));
  return 1;
}

int b_get_pawn_count(lua_State* L) {
  const Board& b = board_of(L);
  if (match(L, {A::Board, A::Int})) {
    const int team = to_int(L, 2);
    int alive = 0, frozen = 0;
    for (const Pawn& p : b.pawns()) {
      if (!is_team(L, p, team) || !p.alive() || symbol_name(p.type) == "BonusDebris") continue;
      ++alive;
      if (p.frozen) ++frozen;
    }
    if (team == -1) alive += static_cast<int>(b.spawn_points.size());
    const int n = alive == 0 ? 0 : std::max(1, static_cast<int>(alive - frozen / 3.0));
    lua_pushinteger(L, n);
  } else if (match(L, {A::Board, A::Str})) {
    const std::string family = to_str(L, 2);
    int n = 0;
    for (const Pawn& p : b.pawns()) {
      const std::string_view type = symbol_name(p.type);
      if (!type.empty() && type.substr(0, type.size() - 1) == family) ++n;
    }
    lua_pushinteger(L, n);
  } else {
    no_overload("int GetPawnCount(Board&,std::string)\nint GetPawnCount(Board&,int)");
  }
  return 1;
}

int b_get_enemy_count(lua_State* L) {
  if (!match(L, {A::Board})) no_overload("int GetEnemyCount(Board&)");
  lua_settop(L, 1);
  lua_pushinteger(L, 6);
  return b_get_pawn_count(L);
}

int b_get_mech_damage(lua_State* L) {
  if (!match(L, {A::Board})) no_overload("int GetMechDamage(Board&)");
  int sum = 0;
  for (const Pawn& p : board_of(L).pawns()) {
    if (p.mech) sum += p.max_hp - p.hp;
  }
  lua_pushinteger(L, sum);
  return 1;
}

int b_get_infected_count(lua_State* L) {
  if (!match(L, {A::Board})) no_overload("int GetInfectedCount(Board&)");
  const Board& b = board_of(L);
  const std::vector<int> mechs = pawn_ids(L, b, 4);
  int n = 0;
  for (int id : mechs) {
    if (b.find_pawn(id)->infected) ++n;
  }
  lua_pushinteger(L, mechs.empty() ? 3 : n);
  return 1;
}

std::vector<Point> building_tiles(const Board& b) {
  std::vector<Point> out;
  for (int i = 0; i < kTileCount; ++i) {
    const Point p = Point::from_index(i);
    if (b.tile(p).is_building()) out.push_back(p);
  }
  return out;
}

int b_get_buildings(lua_State* L) {
  if (!match(L, {A::Board})) no_overload("PointList GetBuildings(Board&)");
  push_point_list(L, building_tiles(board_of(L)));
  return 1;
}

int b_get_distance_to_building(lua_State* L) {
  if (!match(L, {A::Board, A::Point})) no_overload("int GetDistanceToBuilding(Board&,Point)");
  const Point from = point_arg(L, 2);
  int best = INT_MAX;
  for (Point p : building_tiles(board_of(L))) {
    best = std::min(best, std::abs(p.x - from.x) + std::abs(p.y - from.y));
  }
  lua_pushinteger(L, best);
  return 1;
}

int b_get_distance_to_pawn(lua_State* L) {
  if (!match(L, {A::Board, A::Point, A::Int})) no_overload("int GetDistanceToPawn(Board&,Point,int)");
  const Board& b = board_of(L);
  const int team = to_int(L, 3);
  const DistanceMap d = distance_map(b, point_arg(L, 2), raw_pathing(team), 6);
  int best = INT_MAX;
  for (const Pawn& p : b.pawns()) {
    if (!is_team(L, p, team)) continue;
    const int dist = p.pos.valid() ? d[p.pos.index()] : 0;
    if (dist <= 6) best = std::min(best, dist);
  }
  lua_pushinteger(L, best);
  return 1;
}

int b_get_zone(lua_State* L) {
  if (!match(L, {A::Board, A::Str})) no_overload("PointList GetZone(Board&,std::string)");
  push_point_list(L, {});  // mission zones are not part of the engine Board
  return 1;
}

// Skill::IsTargeted over every other pawn's queued attack (Board::IsTargeted).
int b_is_targeted(lua_State* L) {
  if (!match(L, {A::Board, A::Point})) no_overload("bool IsTargeted(Board&,Point)");
  lua_pushboolean(L, host_is_targeted(L, point_arg(L, 2)));
  return 1;
}

int b_is_safe(lua_State* L) {
  if (!match(L, {A::Board, A::Point})) no_overload("bool IsSafe(Board&,Point)");
  const Board& b = board_of(L);
  const Point p = point_arg(L, 2);
  const Tile& t = tile_at(b, p);
  const bool safe = !q_pod(b, p) && !t.acid && !t.on_fire() && !host_is_targeted(L, p) &&
                    t.terrain != Terrain::Water && !q_spawning(b, p) && !t.smoke &&
                    !is_blocked(b, p, Pathing::lua(PathProfile::Ground));
  lua_pushboolean(L, safe);
  return 1;
}

int b_get_deploy_loc_score(lua_State* L) {
  if (!match(L, {A::Board, A::Point})) no_overload("int GetDeployLocScore(Board&,Point)");
  const Board& b = board_of(L);
  const Point p = point_arg(L, 2);
  if (occupied(b, p)) {
    lua_pushinteger(L, -10);
    return 1;
  }
  lua_pushinteger(L, host_deploy_score(L, p));
  return 1;
}

// ---- Board mutators (recorded) -----------------------------------------------------------

const std::vector<Mutator>& board_mutators() {
  using V = std::vector<A>;
  static const std::vector<Mutator> m = {
      {"SetCracked", {V{A::Point, A::Bool}}},
      {"SetDangerous", {V{A::Point}}},
      {"SetItem", {V{A::Point, A::Str}}},
      {"RemoveShield", {V{A::Point}}},
      {"AddShield", {V{A::Point}}},
      {"StartMechTravel", {V{}}},
      {"AddTeleport", {V{A::Point, A::Point}}},
      {"SetTerrain", {V{A::Point, A::Int}}},
      {"SetLava", {V{A::Point, A::Bool}}},
      {"SetSmoke", {V{A::Point, A::Bool, A::Bool}}},
      {"SetFrozen", {V{A::Point, A::Bool}}},
      {"SetAcid", {V{A::Point, A::Bool}}},
      {"SetWall", {V{A::Bool, A::Point, A::Int}}},
      {"SetPopulated", {V{A::Bool, A::Point}}},
      {"SetHealth", {V{A::Point, A::Int, A::Int}}},
      {"SetCustomTile", {V{A::Point, A::Str}}},
      {"BlockSpawn", {V{A::Point, A::Int}}},
      {"ClearBlockSpawns", {V{}}},
      {"ClearSpace", {V{A::Point}}},
      {"SpawnQueued", {V{}}},
      {"RandomizeTerrain", {V{A::Int, A::Int}}},
      {"AddUniqueBuilding", {V{A::Str}}, Ret::Point},
      {"Crack", {V{A::Int}}},
      {"RemovePawn", {V{A::Point}, V{A::PawnPtr}}},
      {"AddPawn",
       {V{A::PawnPtr, A::Point}, V{A::PawnPtr}, V{A::PawnPtr, A::Str}, V{A::Str}, V{A::Str, A::Point},
        V{A::Str, A::Str}},
       Ret::Point},
      {"SpawnPawn",
       {V{A::PawnPtr, A::Point}, V{A::PawnPtr, A::Str}, V{A::PawnPtr}, V{A::Str, A::Point},
        V{A::Str, A::Str}, V{A::Str}},
       Ret::Int},
      {"AddEffect", {V{A::SE}, V{A::SD}}},
      {"DamageSpace", {V{A::SD}, V{A::Point, A::Int}}},
      // Not only cosmetic: a bouncing tile is busy for 0.5 s (Resolver::bounce).
      {"Bounce", {V{A::Point, A::Int}}},
      // Cosmetic / UI: checked but not reported.
      {"MarkSpaceImage", {V{A::Point, A::Str, A::Color}}, Ret::Void, false},
      {"MarkSpaceSimpleColor", {V{A::Point, A::Color}}, Ret::Void, false},
      {"MarkSpaceColor", {V{A::Point, A::Color}}, Ret::Void, false},
      {"MarkSpaceDamage", {V{A::SD}}, Ret::Void, false},
      {"MarkSpaceDesc", {V{A::Point, A::Str}, V{A::Point, A::Str, A::Bool}}, Ret::Void, false},
      {"MarkFlashing", {V{A::Point, A::Bool}}, Ret::Void, false},
      {"SetTerrainIcon", {V{A::Point, A::Str}}, Ret::Void, false},
      {"AddAnimation", {V{A::Point, A::Str, A::Int}}, Ret::Void, false},
      {"AddAlert", {V{A::Point, A::Str}}, Ret::Void, false},
      {"AddBurst", {V{A::Point, A::Str, A::Int}}, Ret::Void, false},
      {"Ping", {V{A::Point, A::Color}}, Ret::Void, false},
      {"StartShake", {V{A::Num}}, Ret::Void, false},
      {"StartPopEvent", {V{A::Str}}, Ret::Void, false},
      {"SetWeather", {V{A::Int, A::Int, A::Point, A::Point, A::Num}}, Ret::Void, false},
      {"StopWeather", {V{}}, Ret::Void, false},
      {"Slide", {V{A::Int}}, Ret::Void, false},
      {"Fade", {V{A::Int}}, Ret::Void, false},
      {"LockBomb", {V{A::Point}}, Ret::Void, false},
  };
  return m;
}

template <size_t I>
int board_mutator(lua_State* L) {
  return run_mutator(L, "Board", A::Board, board_mutators()[I]);
}

// ---- BoardPawn ------------------------------------------------------------------------

template <bool (*F)(lua_State*, const Board&, const Pawn&)>
int pawn_bool(lua_State* L) {
  if (!match(L, {A::Pawn})) no_overload("bool <query>(Pawn&)");
  lua_pushboolean(L, F(L, board_of(L), pawn_ref(L, 1)));
  return 1;
}

template <int (*F)(lua_State*, const Board&, const Pawn&)>
int pawn_int(lua_State* L) {
  if (!match(L, {A::Pawn})) no_overload("int <query>(Pawn&)");
  lua_pushinteger(L, F(L, board_of(L), pawn_ref(L, 1)));
  return 1;
}

bool pq_powered(lua_State*, const Board&, const Pawn& p) { return p.movement.powered; }
bool pq_corpse(lua_State*, const Board& b, const Pawn& p) { return is_corpse(b, p); }
bool pq_temp_unit(lua_State* L, const Board&, const Pawn& p) { return lua_type_flag(L, p, "TempUnit", false); }
bool pq_burrower(lua_State*, const Board&, const Pawn& p) { return p.burrows; }
bool pq_non_grid(lua_State*, const Board&, const Pawn& p) { return p.non_grid; }
bool pq_damaged(lua_State*, const Board&, const Pawn& p) { return p.hp != p.max_hp; }
bool pq_avoiding_mines(lua_State* L, const Board&, const Pawn& p) {
  return lua_type_flag(L, p, "AvoidingMines", false);
}
bool pq_shield(lua_State*, const Board& b, const Pawn& p) { return p.shield || is_turn_shielded(b, p); }
bool pq_weapon_armed(lua_State*, const Board&, const Pawn&) { return false; }
bool pq_dead(lua_State*, const Board&, const Pawn& p) { return !p.alive(); }
bool pq_selected(lua_State* L, const Board&, const Pawn& p) { return host_context(L).selected == p.uid; }
bool pq_mech(lua_State*, const Board&, const Pawn& p) { return p.mech; }
bool pq_busy(lua_State*, const Board&, const Pawn&) { return false; }
bool pq_grappled(lua_State*, const Board&, const Pawn& p) { return p.webbed && p.alive(); }
bool pq_guarding(lua_State*, const Board&, const Pawn& p) { return native::is_guarding(p); }
bool pq_undo_possible(lua_State*, const Board&, const Pawn& p) { return p.movement.undo_ready; }
bool pq_fire(lua_State*, const Board&, const Pawn& p) { return native::is_fire(p); }
bool pq_frozen(lua_State*, const Board&, const Pawn& p) { return p.frozen; }
bool pq_acid(lua_State*, const Board&, const Pawn& p) { return p.acid; }
bool pq_teleporter(lua_State*, const Board&, const Pawn& p) { return native::is_teleporter(p); }
bool pq_jumper(lua_State*, const Board&, const Pawn& p) { return native::is_jumper(p); }
bool pq_flying(lua_State*, const Board&, const Pawn& p) { return is_flying(p); }
bool pq_ranged(lua_State* L, const Board&, const Pawn& p) { return lua_type_int(L, p, "Ranged", 0) == 1; }
bool pq_infected(lua_State*, const Board&, const Pawn& p) { return p.infected; }
bool pq_boosted(lua_State*, const Board& b, const Pawn& p) { return is_boosted(b, p); }
bool pq_player(lua_State*, const Board&, const Pawn& p) { return p.team == Team::Player; }
bool pq_enemy(lua_State*, const Board&, const Pawn& p) { return p.team == Team::Enemy; }
bool pq_active(lua_State*, const Board&, const Pawn& p) {
  return p.active && p.alive() && p.movement.powered;
}

int pi_shots(lua_State*, const Board&, const Pawn& p) {
  return static_cast<int>(std::count_if(p.weapons.begin(), p.weapons.end(),
                                        [](Symbol w) { return w != kNoSymbol; }));
}
int pi_danger(lua_State* L, const Board&, const Pawn& p) { return lua_type_int(L, p, "ScoreDanger", -10); }
int pi_armed(lua_State*, const Board&, const Pawn&) { return -1; }
int pi_id(lua_State*, const Board&, const Pawn& p) { return p.uid; }
int pi_team(lua_State*, const Board&, const Pawn& p) { return static_cast<int>(p.team); }
int pi_path_prof(lua_State*, const Board& b, const Pawn& p) { return native::path_prof(b, p); }
int pi_move_speed(lua_State*, const Board& b, const Pawn& p) { return native::move_speed(b, p); }
int pi_base_move(lua_State*, const Board& b, const Pawn& p) { return base_move(b, p); }
int pi_health(lua_State*, const Board&, const Pawn& p) { return p.hp; }
int pi_turn_count(lua_State*, const Board&, const Pawn& p) { return p.movement.turn_count; }
int pi_selected_weapon(lua_State*, const Board&, const Pawn&) { return 0; }

int p_get_type(lua_State* L) {
  if (!match(L, {A::Pawn})) no_overload("std::string GetType(Pawn&)");
  const std::string_view s = symbol_name(pawn_ref(L, 1).type);
  lua_pushlstring(L, s.data(), s.size());
  return 1;
}

int p_get_space(lua_State* L) {
  if (!match(L, {A::Pawn})) no_overload("Point GetSpace(Pawn&)");
  push_point(L, pawn_ref(L, 1).pos);
  return 1;
}

int p_get_target(lua_State* L) {
  if (!match(L, {A::Pawn})) no_overload("Point GetTarget(Pawn&)");
  push_point(L, kInvalidPoint);
  return 1;
}

int p_is_ability(lua_State* L) {
  if (!match(L, {A::Pawn, A::Str})) no_overload("bool IsAbility(Pawn&,std::string)");
  lua_pushboolean(L, native::is_ability(pawn_ref(L, 1), to_str(L, 2)));
  return 1;
}

int p_custom_position_score(lua_State* L) {
  if (!match(L, {A::Pawn, A::Point})) no_overload("int GetCustomPositionScore(Pawn&,Point)");
  lua_pushinteger(L, host_position_score(L, pawn_ref(L, 1), point_arg(L, 2)));
  return 1;
}

// Pawn::IsDeadly: the same test for a hit on the pawn's own tile.
int p_is_deadly(lua_State* L) {
  if (!match(L, {A::Pawn, A::SD})) no_overload("bool IsDeadly(Pawn&,SpaceDamage)");
  const Pawn& p = pawn_ref(L, 1);
  LuaSpaceDamage sd = sd_arg(L, 2);
  sd.loc = p.pos;
  lua_pushboolean(L, p.pos.valid() && is_deadly(board_of(L), std::move(sd), nullptr));
  return 1;
}

int p_name_string(lua_State* L) {
  if (lua_gettop(L) < 1 || !to_instance(L, 1, Cls::BoardPawn)) no_overload("std::string <name>(Pawn&,...)");
  lua_pushstring(L, "");
  return 1;
}

const std::vector<Mutator>& pawn_mutators() {
  using V = std::vector<A>;
  static const std::vector<Mutator> m = {
      {"SetInfected", {V{A::Bool}}},
      {"SpawnAnimation", {V{}}, Ret::Void, false},
      {"SetMech", {V{}}},
      {"SetNeutral", {V{A::Bool}}},
      {"SetMissionCritical", {V{A::Bool}}},
      {"SetShield", {V{A::Bool}}},
      {"SetHealth", {V{A::Int}}},
      {"AddMoveBonus", {V{A::Int}}},
      {"SetPowered", {V{A::Bool}}},
      {"SetSpace", {V{A::Point}}},
      {"Kill", {V{A::Bool}}},
      {"SetActive", {V{A::Bool}}},
      {"SetAcid", {V{A::Bool}}},
      {"SetMoveSpeed", {V{A::Int}}},
      {"SetPriorityTarget", {V{A::Point}}},
      {"SetTeam", {V{A::Int}}},
      {"FlyAway", {V{}}},
      {"SetCustomAnim", {V{A::Str}}, Ret::Void, false},
      {"Retreat", {V{}}},
      {"FireWeapon", {V{A::Point, A::Int}}, Ret::Int},
      {"SetInvisible", {V{A::Bool}}, Ret::Void, false},
      {"SetFrozen", {V{A::Bool}}},
      {"SetBoosted", {V{A::Bool}}},
      {"ModifyHealth", {V{A::Int, A::Bool, A::Int}}},
      {"GuardPawn", {V{A::Bool}}},
      {"ResetUses", {V{}}},
      {"AddWeapon", {V{A::Str}}},
      {"ClearQueued", {V{}}},
      {"Fall", {V{A::Int}}},
      {"SetMutation", {V{A::Int}}},
      {"Move", {V{A::Point}}, Ret::Bool},
  };
  return m;
}

template <size_t I>
int pawn_mutator(lua_State* L) {
  return run_mutator(L, "Pawn", A::Pawn, pawn_mutators()[I]);
}

// ---- GameMap / ValueBar / PawnFactory -------------------------------------------------------

struct ValueBarData {
  int value = 0;
  int max = 0;
};

int g_get_sector(lua_State* L) {
  if (!match(L, {A::Game})) no_overload("int GetSector(GameMap&)");
  lua_pushinteger(L, host_context(L).options.sector);
  return 1;
}

int g_get_team_turn(lua_State* L) {
  if (!match(L, {A::Game})) no_overload("int GetTeamTurn(GameMap&)");
  lua_pushinteger(L, board_of(L).player_phase ? 1 : 6);
  return 1;
}

int g_get_turn_count(lua_State* L) {
  if (!match(L, {A::Game})) no_overload("int GetTurnCount(GameMap&)");
  lua_pushinteger(L, board_of(L).turn);
  return 1;
}

int g_get_power(lua_State* L) {
  if (!match(L, {A::Game})) no_overload("ValueBar GetPower(GameMap&)");
  const Board& b = board_of(L);
  push_owned(L, Cls::ValueBar, ValueBarData{b.grid_power, b.grid_power_max});
  return 1;
}

int g_get_pawn(lua_State* L) {
  if (!match(L, {A::Game, A::Int})) no_overload("Pawn* GetPawn(GameMap&,int)");
  push_pawn(L, board_of(L).find_pawn(to_int(L, 2)));
  return 1;
}

int g_false(lua_State* L) {
  if (lua_gettop(L) < 1 || !to_instance(L, 1, Cls::GameMap)) no_overload("bool <query>(GameMap&,...)");
  lua_pushboolean(L, 0);
  return 1;
}

int g_zero(lua_State* L) {
  if (lua_gettop(L) < 1 || !to_instance(L, 1, Cls::GameMap)) no_overload("int <query>(GameMap&,...)");
  lua_pushinteger(L, 0);
  return 1;
}

int g_empty_string(lua_State* L) {
  if (lua_gettop(L) < 1 || !to_instance(L, 1, Cls::GameMap)) no_overload("std::string <query>(GameMap&)");
  lua_pushstring(L, "");
  return 1;
}

int g_nil(lua_State* L) {
  if (lua_gettop(L) < 1 || !to_instance(L, 1, Cls::GameMap)) no_overload("<query>(GameMap&)");
  lua_pushnil(L);
  return 1;
}

int g_ui(lua_State* L) {
  if (lua_gettop(L) < 1 || !to_instance(L, 1, Cls::GameMap)) no_overload("void <ui>(GameMap&,...)");
  return 0;
}

int g_modify_power_grid(lua_State* L) {
  if (!match(L, {A::Game, A::Int})) no_overload("void ModifyPowerGrid(GameMap&,int)");
  record_write(L, "Game", "ModifyPowerGrid", 2);
  return 0;
}

int vb_get_value(lua_State* L) {
  if (!match(L, {A::ValueBar})) no_overload("int GetValue(ValueBar&)");
  lua_pushinteger(L, get<ValueBarData>(L, 1, Cls::ValueBar).value);
  return 1;
}

int vb_get_max(lua_State* L) {
  if (!match(L, {A::ValueBar})) no_overload("int GetMax(ValueBar&)");
  lua_pushinteger(L, get<ValueBarData>(L, 1, Cls::ValueBar).max);
  return 1;
}

int vb_add_event(lua_State* L) {
  if (!match(L, {A::ValueBar, A::Str})) no_overload("void AddEvent(ValueBar&,std::string)");
  return 0;
}

int f_create_pawn(lua_State* L) {
  if (!match(L, {A::Factory, A::Str}) && !match(L, {A::Factory, A::Str, A::Int})) {
    no_overload("Pawn* CreatePawn(PawnFactory&,std::string)\nPawn* CreatePawn(PawnFactory&,std::string,int)");
  }
  // A new pawn is not on any board; the host cannot create one on a
  // read-only board, so the request is reported and nil returned.
  record_write(L, "PAWN_FACTORY", "CreatePawn", 2);
  lua_pushnil(L);
  return 1;
}

// ---- global functions ------------------------------------------------------------------------

int fn_get_difficulty(lua_State* L) {
  if (!match(L, {})) no_overload("int GetDifficulty()");
  lua_pushinteger(L, host_context(L).options.difficulty);
  return 1;
}

int fn_advanced(lua_State* L) {
  if (!match(L, {})) no_overload("bool IsNew...()");
  lua_pushboolean(L, host_context(L).options.advanced_content);
  return 1;
}

int fn_false(lua_State* L) {
  lua_pushboolean(L, 0);
  return 1;
}

int fn_true(lua_State* L) {
  lua_pushboolean(L, 1);
  return 1;
}

int fn_is_mutation(lua_State* L) {
  if (!match(L, {A::Int})) no_overload("bool IsMutation(int)");
  lua_pushboolean(L, 0);
  return 1;
}

int fn_is_passive_skill(lua_State* L) {
  if (!match(L, {A::Str})) no_overload("bool IsPassiveSkill(std::string)");
  lua_pushboolean(L, native::is_passive_skill(L, to_str(L, 1)));
  return 1;
}

int fn_console_print(lua_State* L) {
  if (!match(L, {A::Str})) no_overload("void ConsolePrint(std::string)");
  host_console(L, to_str(L, 1));
  return 0;
}

// print(...) as in the stock base library, but captured.
int fn_print(lua_State* L) {
  std::string line;
  const int n = lua_gettop(L);
  lua_getglobal(L, "tostring");
  for (int i = 1; i <= n; ++i) {
    lua_pushvalue(L, -1);
    lua_pushvalue(L, i);
    if (lua_pcall(L, 1, 1, 0) != 0) throw LuaError(to_str(L, -1));  // e.g. tostring(Point)
    if (lua_type(L, -1) != LUA_TSTRING && lua_type(L, -1) != LUA_TNUMBER) {
      throw LuaError("'tostring' must return a string to 'print'");
    }
    if (i > 1) line += '\t';
    line += to_str(L, -1);
    lua_pop(L, 1);
  }
  host_console(L, line);
  return 0;
}

}  // namespace

// ---- registration ----------------------------------------------------------------------

HostContext& host_context(lua_State* L) {
  lua_pushlightuserdata(L, &kHostKey);
  lua_rawget(L, LUA_REGISTRYINDEX);
  auto* ctx = static_cast<HostContext*>(lua_touserdata(L, -1));
  lua_pop(L, 1);
  if (!ctx) throw LuaError("no Lua host context");
  return *ctx;
}

void push_pawn(lua_State* L, const Pawn* p) { push_pawn_uid(L, p ? p->uid : -1); }

void push_pawn_uid(lua_State* L, int32_t uid) {
  if (uid < 0) {
    lua_pushnil(L);
    return;
  }
  auto* in = static_cast<Instance*>(lua_newuserdata(L, sizeof(Instance)));
  *in = Instance{Cls::BoardPawn, Kind::Pawn, nullptr, nullptr,
                 static_cast<uintptr_t>(static_cast<uint32_t>(uid)), nullptr, nullptr};
  finish_instance(L);
}

namespace {

template <size_t... I>
void add_board_mutators(std::vector<Method>& out, std::index_sequence<I...>) {
  (out.push_back({board_mutators()[I].name, ITB_G(board_mutator<I>)}), ...);
}

template <size_t... I>
void add_pawn_mutators(std::vector<Method>& out, std::index_sequence<I...>) {
  (out.push_back({pawn_mutators()[I].name, ITB_G(pawn_mutator<I>)}), ...);
}

constexpr size_t kBoardMutatorCount = 47;
constexpr size_t kPawnMutatorCount = 31;

void register_methods(lua_State* L, Cls cls, const std::vector<Method>& methods) {
  register_class(L, cls, nullptr, nullptr, {});
  add_methods(L, cls, methods);
}

}  // namespace

void install_host_bindings(lua_State* L, HostContext* ctx) {
  if (board_mutators().size() != kBoardMutatorCount || pawn_mutators().size() != kPawnMutatorCount) {
    throw std::logic_error("mutator table size mismatch");
  }
  lua_pushlightuserdata(L, &kHostKey);
  lua_pushlightuserdata(L, ctx);
  lua_rawset(L, LUA_REGISTRYINDEX);

  std::vector<Method> board = {
      {"IsValid", ITB_G(b_is_valid)},
      {"GetSize", ITB_G(b_get_size)},
      {"IsBlocked", ITB_G(b_is_blocked)},
      {"IsPawnSpace", ITB_G(b_is_pawn_space)},
      {"GetPawn", ITB_G(b_get_pawn)},
      {"IsPawnAlive", ITB_G(b_is_pawn_alive)},
      {"GetPawnSpace", ITB_G(b_get_pawn_space)},
      {"GetPawnTeam", ITB_G(b_get_pawn_team)},
      {"IsPawnTeam", ITB_G(b_is_pawn_team)},
      {"GetTerrain", ITB_G(b_get_terrain)},
      {"IsBuilding", ITB_G(b_is_building)},
      {"IsTerrain", ITB_G(b_is_terrain)},
      {"IsPowered", ITB_G(tile_query<q_powered>)},
      {"IsFire", ITB_G(tile_query<q_fire>)},
      {"IsSmoke", ITB_G(tile_query<q_smoke>)},
      {"IsAcid", ITB_G(tile_query<q_acid>)},
      {"IsFrozen", ITB_G(tile_query<q_frozen>)},
      {"IsVines", ITB_G(tile_query<q_vines>)},
      {"IsPod", ITB_G(tile_query<q_pod>)},
      {"IsItem", ITB_G(tile_query<q_item>)},
      {"IsDangerousItem", ITB_G(tile_query<q_item>)},
      {"IsCrackable", ITB_G(tile_query<q_crackable>)},
      {"IsCracked", ITB_G(tile_query<q_cracked>)},
      {"IsDamaged", ITB_G(tile_query<q_damaged>)},
      {"IsEdge", ITB_G(tile_query<q_edge>)},
      {"IsUniqueBuilding", ITB_G(tile_query<q_unique_building>)},
      {"IsSpawning", ITB_G(tile_query<q_spawning>)},
      {"IsDangerous", ITB_G(tile_query<q_dangerous>)},
      {"IsEnvironmentDanger", ITB_G(tile_query<q_env_danger>)},
      {"GetItem", ITB_G(b_get_item)},
      {"GetCustomTile", ITB_G(b_get_custom_tile)},
      {"GetHealth", ITB_G(b_get_health)},
      {"IsWall", ITB_G(b_is_wall)},
      {"IsBusy", ITB_G(b_is_busy)},
      {"GetBusyState", ITB_G(b_get_busy_state)},
      {"GetTurn", ITB_G(b_get_turn)},
      {"IsDeadly", ITB_G(b_is_deadly)},
      {"GetPath", ITB_G(b_get_path)},
      {"GetSimplePath", ITB_G(b_get_simple_path)},
      {"GetSimpleReachable", ITB_G(b_get_simple_reachable)},
      {"GetReachable", ITB_G(b_get_reachable)},
      {"GetDistance", ITB_G(b_get_distance)},
      {"GetPawns", ITB_G(b_get_pawns)},
      {"GetPawnCount", ITB_G(b_get_pawn_count)},
      {"GetEnemyCount", ITB_G(b_get_enemy_count)},
      {"GetMechDamage", ITB_G(b_get_mech_damage)},
      {"GetInfectedCount", ITB_G(b_get_infected_count)},
      {"GetBuildings", ITB_G(b_get_buildings)},
      {"GetDistanceToBuilding", ITB_G(b_get_distance_to_building)},
      {"GetDistanceToPawn", ITB_G(b_get_distance_to_pawn)},
      {"GetZone", ITB_G(b_get_zone)},
      {"IsTargeted", ITB_G(b_is_targeted)},
      {"IsSafe", ITB_G(b_is_safe)},
      {"GetDeployLocScore", ITB_G(b_get_deploy_loc_score)},
  };
  add_board_mutators(board, std::make_index_sequence<kBoardMutatorCount>{});
  register_methods(L, Cls::Board, board);

  std::vector<Method> pawn = {
      {"IsPowered", ITB_G(pawn_bool<pq_powered>)},
      {"IsCorpse", ITB_G(pawn_bool<pq_corpse>)},
      {"IsTempUnit", ITB_G(pawn_bool<pq_temp_unit>)},
      {"IsBurrower", ITB_G(pawn_bool<pq_burrower>)},
      {"IsNonGridStructure", ITB_G(pawn_bool<pq_non_grid>)},
      {"IsDamaged", ITB_G(pawn_bool<pq_damaged>)},
      {"IsAvoidingMines", ITB_G(pawn_bool<pq_avoiding_mines>)},
      {"IsShield", ITB_G(pawn_bool<pq_shield>)},
      {"IsWeaponArmed", ITB_G(pawn_bool<pq_weapon_armed>)},
      {"IsDead", ITB_G(pawn_bool<pq_dead>)},
      {"IsSelected", ITB_G(pawn_bool<pq_selected>)},
      {"IsMech", ITB_G(pawn_bool<pq_mech>)},
      {"IsBusy", ITB_G(pawn_bool<pq_busy>)},
      {"IsGrappled", ITB_G(pawn_bool<pq_grappled>)},
      {"IsGuarding", ITB_G(pawn_bool<pq_guarding>)},
      {"IsUndoPossible", ITB_G(pawn_bool<pq_undo_possible>)},
      {"IsFire", ITB_G(pawn_bool<pq_fire>)},
      {"IsFrozen", ITB_G(pawn_bool<pq_frozen>)},
      {"IsAcid", ITB_G(pawn_bool<pq_acid>)},
      {"IsTeleporter", ITB_G(pawn_bool<pq_teleporter>)},
      {"IsJumper", ITB_G(pawn_bool<pq_jumper>)},
      {"IsFlying", ITB_G(pawn_bool<pq_flying>)},
      {"IsRanged", ITB_G(pawn_bool<pq_ranged>)},
      {"IsInfected", ITB_G(pawn_bool<pq_infected>)},
      {"IsBoosted", ITB_G(pawn_bool<pq_boosted>)},
      {"IsPlayer", ITB_G(pawn_bool<pq_player>)},
      {"IsEnemy", ITB_G(pawn_bool<pq_enemy>)},
      {"IsActive", ITB_G(pawn_bool<pq_active>)},
      {"GetShotsRemaining", ITB_G(pawn_int<pi_shots>)},
      {"GetDangerScore", ITB_G(pawn_int<pi_danger>)},
      {"GetArmedWeaponId", ITB_G(pawn_int<pi_armed>)},
      {"GetId", ITB_G(pawn_int<pi_id>)},
      {"GetTeam", ITB_G(pawn_int<pi_team>)},
      {"GetPathProf", ITB_G(pawn_int<pi_path_prof>)},
      {"GetMoveSpeed", ITB_G(pawn_int<pi_move_speed>)},
      {"GetBaseMove", ITB_G(pawn_int<pi_base_move>)},
      {"GetHealth", ITB_G(pawn_int<pi_health>)},
      {"GetTurnCount", ITB_G(pawn_int<pi_turn_count>)},
      {"GetSelectedWeapon", ITB_G(pawn_int<pi_selected_weapon>)},
      {"GetType", ITB_G(p_get_type)},
      {"GetSpace", ITB_G(p_get_space)},
      {"GetTarget", ITB_G(p_get_target)},
      {"GetFirstClick", ITB_G(p_get_target)},
      {"IsAbility", ITB_G(p_is_ability)},
      {"GetCustomPositionScore", ITB_G(p_custom_position_score)},
      {"IsDeadly", ITB_G(p_is_deadly)},
      {"GetMechName", ITB_G(p_name_string)},
      {"GetPilotName", ITB_G(p_name_string)},
      {"GetPersonality", ITB_G(p_name_string)},
      {"GetPilotLanguage", ITB_G(p_name_string)},
  };
  add_pawn_mutators(pawn, std::make_index_sequence<kPawnMutatorCount>{});
  register_methods(L, Cls::BoardPawn, pawn);

  register_methods(L, Cls::GameMap,
                   {
                       {"GetSector", ITB_G(g_get_sector)},
                       {"GetTeamTurn", ITB_G(g_get_team_turn)},
                       {"GetTurnCount", ITB_G(g_get_turn_count)},
                       {"GetPower", ITB_G(g_get_power)},
                       {"GetPawn", ITB_G(g_get_pawn)},
                       {"ModifyPowerGrid", ITB_G(g_modify_power_grid)},
                       {"IsIslandUnlocked", ITB_G(g_false)},
                       {"IsTip", ITB_G(g_false)},
                       {"IsVoicePopup", ITB_G(g_false)},
                       {"IsEvent", ITB_G(g_false)},
                       {"IsMechClass", ITB_G(g_false)},
                       {"GetEventCount", ITB_G(g_zero)},
                       {"GetAnotherVoice", ITB_G(g_zero)},
                       {"GetSquad", ITB_G(g_empty_string)},
                       {"GetSavedCorp", ITB_G(g_empty_string)},
                       {"GetItems", ITB_G(g_empty_string)},
                       {"GetRandomVoice", ITB_G(g_nil)},
                       {"GetCorp", ITB_G(g_nil)},
                       {"AddObjective", ITB_G(g_ui)},
                       {"AddTip", ITB_G(g_ui)},
                       {"AddTutorial", ITB_G(g_ui)},
                       {"AddPermaTip", ITB_G(g_ui)},
                       {"AddVerticalInterfaceTip", ITB_G(g_ui)},
                       {"AddInterfaceTip", ITB_G(g_ui)},
                       {"ClearTips", ITB_G(g_ui)},
                       {"BlockNextTurn", ITB_G(g_ui)},
                       {"TriggerSound", ITB_G(g_ui)},
                       {"AddNote", ITB_G(g_ui)},
                       {"AddEvent", ITB_G(g_ui)},
                       {"AddVoicePopup", ITB_G(g_ui)},
                       {"RemoveItem", ITB_G(g_ui)},
                       {"AddPilot", ITB_G(g_ui)},
                       {"AddWeapon", ITB_G(g_ui)},
                   });
  register_methods(L, Cls::ValueBar,
                   {{"GetValue", ITB_G(vb_get_value)},
                    {"GetMax", ITB_G(vb_get_max)},
                    {"AddEvent", ITB_G(vb_add_event)}});
  register_methods(L, Cls::PawnFactory, {{"CreatePawn", ITB_G(f_create_pawn)}});

  set_global_function(L, "GetDifficulty", ITB_G(fn_get_difficulty));
  set_global_function(L, "IsNewEnemies", ITB_G(fn_advanced));
  set_global_function(L, "IsNewMissions", ITB_G(fn_advanced));
  set_global_function(L, "IsNewEquipment", ITB_G(fn_advanced));
  set_global_function(L, "IsMutation", ITB_G(fn_is_mutation));
  set_global_function(L, "IsPassiveSkill", ITB_G(fn_is_passive_skill));
  set_global_function(L, "ConsolePrint", ITB_G(fn_console_print));
  set_global_function(L, "IsRelease", fn_true);
  set_global_function(L, "IsGamepad", fn_false);
  set_global_function(L, "IsLargeFont", fn_false);
  set_global_function(L, "IsPhoneUI", fn_false);
  set_global_function(L, "IsTouch", fn_false);
  set_global_function(L, "print", ITB_G(fn_print));

  push_ref(L, Cls::PawnFactory, ctx);
  lua_setglobal(L, "PAWN_FACTORY");
}

void host_push_board(lua_State* L) { push_ref(L, Cls::Board, &host_context(L)); }
void host_push_game(lua_State* L) { push_ref(L, Cls::GameMap, &host_context(L)); }

// ---- native equivalents (lua_native.hpp) ------------------------------------------------

namespace native {

bool is_blocked(const Board& b, Point p, int prof) { return itb::is_blocked(b, p, raw_pathing(prof)); }

bool is_pawn_space(const Board& b, Point p) { return occupied(b, p); }

const Pawn* pawn_at(const Board& b, Point p) { return occ0(b, p); }

int pawn_team(const Board& b, Point p) {
  const Pawn* pawn = occ0(b, p);
  return pawn ? static_cast<int>(pawn->team) : 2;
}

bool is_pawn_team(lua_State* L, const Board& b, Point p, int team) {
  const Pawn* pawn = occ0(b, p);
  return pawn ? is_team(L, *pawn, team) : team == 2;
}

bool is_building(const Board& b, Point p) { return tile_at(b, p).is_building(); }

bool is_pod(const Board& b, Point p) { return q_pod(b, p); }

int terrain(const Board& b, Point p) { return static_cast<int>(tile_at(b, p).terrain); }

std::vector<Point> simple_path(const Board& b, Point from, Point to) { return lua::simple_path(b, from, to); }

std::vector<Point> simple_reachable(const Board& b, Point p, int len, bool corners) {
  return orth_dest_area(b, p, len, corners);
}

std::vector<Point> reachable(const Board& b, Point p, int move, int prof) {
  return mask_points(reachable_list(b, p, move, raw_pathing(prof)));
}

std::vector<Point> path(const Board& b, Point from, Point to, int prof) {
  return find_path(b, from, to, raw_pathing(prof));
}

std::vector<int> pawn_ids(lua_State* L, const Board& b, int team) { return lua::pawn_ids(L, b, team); }

int path_prof(const Board& b, const Pawn& p) { return path_profile(b, p).raw(); }

int move_speed(const Board& b, const Pawn& p) { return itb::move_speed(b, p); }

bool is_ability(const Pawn& p, std::string_view name) { return has_ability(p, std::string(name)); }

bool is_jumper(const Pawn& p) { return p.jumper && p.alive(); }

bool is_teleporter(const Pawn& p) { return p.teleporter && p.alive(); }

bool is_guarding(const Pawn& p) { return !p.pushable; }

bool is_fire(const Pawn& p) { return p.fire; }

bool is_passive_skill(lua_State* L, const std::string& name) { return host_is_passive(L, name); }

}  // namespace native

}  // namespace itb::lua
