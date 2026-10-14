#include "itb/game_data.hpp"

#include <algorithm>
#include <cstdlib>
#include <stdexcept>

#include "lua_env.hpp"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

namespace itb {
namespace {

// Reads fields of the table on top of the stack. Lookups go through
// __index, so inherited Pawn defaults apply exactly as in game.
class FieldReader {
 public:
  FieldReader(lua_State* L, const std::string& owner, std::vector<std::string>* warnings)
      : L_(L), owner_(owner), warnings_(warnings) {}

  int integer(const char* key, int fallback) {
    lua_getfield(L_, -1, key);
    int v = fallback;
    if (lua_type(L_, -1) == LUA_TNUMBER) {
      v = static_cast<int>(lua_tonumber(L_, -1));
    } else if (!lua_isnil(L_, -1)) {
      warn(key);
    }
    lua_pop(L_, 1);
    return v;
  }

  bool boolean(const char* key, bool fallback) {
    lua_getfield(L_, -1, key);
    bool v = fallback;
    if (lua_type(L_, -1) == LUA_TBOOLEAN) {
      v = lua_toboolean(L_, -1) != 0;
    } else if (!lua_isnil(L_, -1)) {
      warn(key);
    }
    lua_pop(L_, 1);
    return v;
  }

  std::string string(const char* key) {
    lua_getfield(L_, -1, key);
    std::string v;
    if (lua_type(L_, -1) == LUA_TSTRING) {
      v = lua_tostring(L_, -1);
    } else if (!lua_isnil(L_, -1)) {
      warn(key);
    }
    lua_pop(L_, 1);
    return v;
  }

  std::vector<std::string> strings(const char* key) {
    std::vector<std::string> out;
    lua_getfield(L_, -1, key);
    if (lua_istable(L_, -1)) {
      const int n = static_cast<int>(lua_objlen(L_, -1));
      for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L_, -1, i);
        if (lua_type(L_, -1) == LUA_TSTRING) out.emplace_back(lua_tostring(L_, -1));
        lua_pop(L_, 1);
      }
    } else if (!lua_isnil(L_, -1)) {
      warn(key);
    }
    lua_pop(L_, 1);
    return out;
  }

  // A Lua array of numbers (indices 1..n).
  std::vector<float> numbers(const char* key) {
    std::vector<float> out;
    lua_getfield(L_, -1, key);
    if (lua_istable(L_, -1)) {
      const int n = static_cast<int>(lua_objlen(L_, -1));
      for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L_, -1, i);
        out.push_back(static_cast<float>(lua_tonumber(L_, -1)));
        lua_pop(L_, 1);
      }
    }
    lua_pop(L_, 1);
    return out;
  }

  // The length of a Lua array field (0 when absent).
  int length(const char* key) {
    lua_getfield(L_, -1, key);
    const int n = lua_istable(L_, -1) ? static_cast<int>(lua_objlen(L_, -1)) : 0;
    lua_pop(L_, 1);
    return n;
  }

  float number(const char* key, float fallback) {
    lua_getfield(L_, -1, key);
    float v = fallback;
    if (lua_type(L_, -1) == LUA_TNUMBER) v = static_cast<float>(lua_tonumber(L_, -1));
    lua_pop(L_, 1);
    return v;
  }

  std::vector<Point> points(const char* key) {
    std::vector<Point> out;
    lua_getfield(L_, -1, key);
    if (lua_istable(L_, -1)) {
      const int n = static_cast<int>(lua_objlen(L_, -1));
      for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L_, -1, i);
        if (lua_istable(L_, -1) || lua_isuserdata(L_, -1)) {  // native Point userdata
          lua_getfield(L_, -1, "x");
          lua_getfield(L_, -2, "y");
          out.emplace_back(static_cast<int>(lua_tonumber(L_, -2)),
                           static_cast<int>(lua_tonumber(L_, -1)));
          lua_pop(L_, 2);
        }
        lua_pop(L_, 1);
      }
    }
    lua_pop(L_, 1);
    return out;
  }

 private:
  void warn(const char* key) {
    if (warnings_) warnings_->push_back(owner_ + "." + key + " is not plain data");
  }

  lua_State* L_;
  const std::string& owner_;
  std::vector<std::string>* warnings_;
};

PawnDef read_pawn(lua_State* L, const std::string& name, std::vector<std::string>* warnings) {
  FieldReader f(L, name, warnings);
  PawnDef d;
  d.name = name;
  d.symbol = intern(name);
  d.pawn_class = f.string("Class");
  d.image = f.string("Image");
  d.health = f.integer("Health", 3);
  d.move_speed = f.integer("MoveSpeed", 0);
  d.skills = f.strings("SkillList");
  d.default_team = static_cast<Team>(f.integer("DefaultTeam", static_cast<int>(Team::None)));
  d.default_faction = static_cast<Faction>(f.integer("DefaultFaction", 0));
  d.leader = static_cast<Leader>(f.integer("Leader", 0));
  d.tier = static_cast<Tier>(f.integer("Tier", 0));
  d.ranged = f.integer("Ranged", 0);
  d.massive = f.boolean("Massive", false);
  d.flying = f.boolean("Flying", false);
  d.pushable = f.boolean("Pushable", true);
  d.armor = f.boolean("Armor", false);
  d.ignore_smoke = f.boolean("IgnoreSmoke", false);
  d.ignore_fire = f.boolean("IgnoreFire", false);
  d.ignore_flip = f.boolean("IgnoreFlip", false);
  d.minor = f.boolean("Minor", false);
  d.corpse = f.boolean("Corpse", false);
  d.burrows = f.boolean("Burrows", false);
  d.jumper = f.boolean("Jumper", false);
  d.teleporter = f.boolean("Teleporter", false);
  d.explodes = f.boolean("Explodes", false);
  d.burns = f.boolean("Burns", false);
  d.neutral = f.boolean("Neutral", false);
  d.non_grid = f.boolean("NonGrid", false);
  d.large_shield = f.boolean("LargeShield", false);
  d.temp_unit = f.boolean("TempUnit", false);
  d.mission = f.boolean("Mission", false);
  d.spawn_limit = f.boolean("SpawnLimit", true);
  d.void_shock_immune = f.boolean("VoidShockImmune", false);
  d.avoiding_mines = f.boolean("AvoidingMines", false);
  d.is_death_effect = f.boolean("IsDeathEffect", false);
  d.extra_spaces = f.points("ExtraSpaces");
  for (const std::string& skill : d.skills) intern(skill);
  return d;
}

// Animation::GetAnimInfo reads NumFrames, Time, Loop and Lengths; a non-empty
// Frames list replaces the frame count.
AnimDef read_anim(lua_State* L, const std::string& name) {
  FieldReader f(L, name, nullptr);
  AnimDef a;
  a.name = name;
  a.symbol = intern(name);
  a.num_frames = f.integer("NumFrames", 1);
  if (const int frames = f.length("Frames"); frames > 0) a.num_frames = frames;
  a.time = f.number("Time", 1.0f);
  a.loop = f.boolean("Loop", false);
  a.lengths = f.numbers("Lengths");
  return a;
}

}  // namespace

std::filesystem::path GameData::default_game_root() {
  if (const char* env = std::getenv("ITB_GAME_DIR"); env && *env) return env;
#ifdef ITB_DEFAULT_GAME_DIR
  return ITB_DEFAULT_GAME_DIR;
#else
  return {};
#endif
}

GameData GameData::load(const std::filesystem::path& game_root, ScriptLoadReport* report) {
  if (!std::filesystem::exists(game_root / "scripts" / "scripts.lua")) {
    throw std::runtime_error("no scripts/scripts.lua under " + game_root.string() +
                             " (set ITB_GAME_DIR to the game's root directory)");
  }
  detail::ScriptRun run;
  auto env = detail::LuaEnv::load_game_scripts(game_root, &run);
  lua_State* L = env->L();

  GameData data;
  std::vector<std::string> warnings;
  lua_getglobal(L, "PawnList");
  if (!lua_istable(L, -1)) throw std::runtime_error("game scripts did not define PawnList");
  const int n = static_cast<int>(lua_objlen(L, -1));
  for (int i = 1; i <= n; ++i) {
    lua_rawgeti(L, -1, i);
    const char* raw = lua_tostring(L, -1);
    const std::string name = raw ? raw : "";
    lua_pop(L, 1);
    if (name.empty() || data.pawn(name)) continue;
    lua_getglobal(L, name.c_str());
    if (lua_istable(L, -1)) {
      data.by_symbol_[intern(name)] = data.pawns_.size();
      data.pawns_.push_back(read_pawn(L, name, &warnings));
    } else {
      warnings.push_back(name + " is in PawnList but not a table");
    }
    lua_pop(L, 1);
  }
  lua_pop(L, 1);

  // Many pawns are declared as `X = Pawn:new{...}` (or derived from another
  // pawn) without AddPawn, so also take every global table whose metatable
  // chain reaches the Pawn base class.
  std::vector<std::string> derived;
  lua_getglobal(L, "Pawn");
  const int base = lua_gettop(L);
  lua_pushnil(L);
  while (lua_next(L, LUA_GLOBALSINDEX) != 0) {
    if (lua_type(L, -2) == LUA_TSTRING && lua_istable(L, -1) && !lua_rawequal(L, -1, base)) {
      bool is_pawn = false;
      lua_pushvalue(L, -1);
      for (int depth = 0; depth < 32 && lua_getmetatable(L, -1); ++depth) {
        lua_remove(L, -2);
        if (lua_rawequal(L, -1, base)) {
          is_pawn = true;
          break;
        }
      }
      lua_pop(L, 1);
      if (is_pawn) derived.emplace_back(lua_tostring(L, -2));
    }
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
  std::sort(derived.begin(), derived.end());
  for (const std::string& name : derived) {
    if (data.pawn(name)) continue;
    lua_getglobal(L, name.c_str());
    data.by_symbol_[intern(name)] = data.pawns_.size();
    data.pawns_.push_back(read_pawn(L, name, &warnings));
    lua_pop(L, 1);
  }

  // Pilots: every global table whose metatable chain reaches the Pilot class
  // (CreatePilot: _G[Id] = Pilot:new(data)), with its Skill.
  lua_getglobal(L, "Pilot");
  if (lua_istable(L, -1)) {
    const int pilot_base = lua_gettop(L);
    lua_pushnil(L);
    while (lua_next(L, LUA_GLOBALSINDEX) != 0) {
      if (lua_type(L, -2) == LUA_TSTRING && lua_istable(L, -1) && !lua_rawequal(L, -1, pilot_base)) {
        bool is_pilot = false;
        lua_pushvalue(L, -1);
        for (int depth = 0; depth < 32 && lua_getmetatable(L, -1); ++depth) {
          lua_remove(L, -2);
          if (lua_rawequal(L, -1, pilot_base)) {
            is_pilot = true;
            break;
          }
        }
        lua_pop(L, 1);
        if (is_pilot) {
          lua_getfield(L, -1, "Skill");
          if (lua_type(L, -1) == LUA_TSTRING) data.pilot_skills_[lua_tostring(L, -3)] = lua_tostring(L, -1);
          lua_pop(L, 1);
        }
      }
      lua_pop(L, 1);
    }
  }
  lua_pop(L, 1);

  // Animations: every table in ANIMS (sorted by name for a stable order).
  std::vector<std::string> anim_names;
  lua_getglobal(L, "ANIMS");
  if (lua_istable(L, -1)) {
    lua_pushnil(L);
    while (lua_next(L, -2) != 0) {
      if (lua_type(L, -2) == LUA_TSTRING && lua_istable(L, -1)) {
        anim_names.emplace_back(lua_tostring(L, -2));
      }
      lua_pop(L, 1);
    }
    std::sort(anim_names.begin(), anim_names.end());
    for (const std::string& name : anim_names) {
      lua_getfield(L, -1, name.c_str());
      data.anim_by_symbol_[intern(name)] = data.anims_.size();
      data.anims_.push_back(read_anim(L, name));
      lua_pop(L, 1);
    }
  }
  lua_pop(L, 1);

  // Values (game.lua).
  lua_getglobal(L, "Values");
  if (lua_istable(L, -1)) {
    lua_pushnil(L);
    while (lua_next(L, -2) != 0) {
      if (lua_type(L, -2) == LUA_TSTRING && lua_type(L, -1) == LUA_TNUMBER) {
        data.values_[lua_tostring(L, -2)] = static_cast<float>(lua_tonumber(L, -1));
      }
      lua_pop(L, 1);
    }
  }
  lua_pop(L, 1);

  if (report) {
    report->files_ok = run.files_ok;
    report->files_failed = static_cast<int>(run.errors.size());
    report->passes = run.passes;
    report->errors = run.errors;
    report->stubbed_globals = run.stubbed_globals;
    report->overwritten_stubs = run.overwritten_stubs;
    report->field_warnings = warnings;
  }
  return data;
}

const PawnDef* GameData::pawn(std::string_view name) const {
  const Symbol s = find_symbol(name);
  return s == kNoSymbol ? nullptr : pawn(s);
}

const PawnDef* GameData::pawn(Symbol symbol) const {
  auto it = by_symbol_.find(symbol);
  return it == by_symbol_.end() ? nullptr : &pawns_[it->second];
}

const AnimDef* GameData::animation(std::string_view name) const {
  const Symbol s = find_symbol(name);
  return s == kNoSymbol ? nullptr : animation(s);
}

const AnimDef* GameData::animation(Symbol symbol) const {
  auto it = anim_by_symbol_.find(symbol);
  return it == anim_by_symbol_.end() ? nullptr : &anims_[it->second];
}

float GameData::value(std::string_view name, float fallback) const {
  auto it = values_.find(std::string(name));
  return it == values_.end() ? fallback : it->second;
}

std::string_view GameData::pilot_skill(std::string_view pilot_id) const {
  auto it = pilot_skills_.find(std::string(pilot_id));
  return it == pilot_skills_.end() ? std::string_view{} : std::string_view(it->second);
}

uint32_t GameData::pilot_ability(std::string_view pilot_id) const {
  return pilot_skill_ability(pilot_skill(pilot_id));
}

uint32_t pilot_skill_ability(std::string_view skill) {
  static const std::pair<const char*, PilotAbility> kNames[] = {
      {"Armored", kPilotArmored},          {"Thick", kPilotThick},
      {"Rock_Skill", kPilotRockSkill},     {"Retaliation", kPilotRetaliation},
      {"Flying", kPilotFlying},            {"Disable_Immunity", kPilotDisableImmunity},
      {"Freeze_Walk", kPilotFreezeWalk},   {"Pain_Immunity", kPilotPainImmunity},
      {"Road_Runner", kPilotRoadRunner},   {"Shifty", kPilotShifty},
      {"Post_Move", kPilotPostMove},       {"Double_Shot", kPilotDoubleShot},
      {"Youth_Move", kPilotYouthMove},     {"Arrogant_Boost", kPilotArrogantBoost},
      {"Regen", kPilotRegen},              {"Zoltan_Skill", kPilotZoltan},
      {"Extra_XP", kPilotExtraXp},         {"KO_Boost", kPilotKoBoost},
  };
  for (const auto& [name, bit] : kNames) {
    if (skill == name) return bit;
  }
  return kPilotNone;
}

Pawn GameData::make_pawn(const PawnDef& def, int32_t uid, Point pos) const {
  Pawn p;
  p.uid = uid;
  p.type = def.symbol;
  p.pos = pos;
  p.hp = static_cast<int8_t>(def.health);
  p.max_hp = static_cast<int8_t>(def.health);
  p.move = static_cast<int8_t>(def.move_speed);
  p.team = def.default_team;
  p.faction = def.default_faction;
  for (size_t i = 0; i < def.skills.size() && i < p.weapons.size(); ++i) {
    p.weapons[i] = intern(def.skills[i]);
  }
  // `mech` is not part of the definition: the game sets it (Pawn::SetMech) for
  // squad units, so callers that place a squad set it themselves.
  p.massive = def.massive;
  p.flying = def.flying;
  p.pushable = def.pushable;
  p.armor = def.armor;
  p.ignore_smoke = def.ignore_smoke;
  p.ignore_fire = def.ignore_fire;
  p.minor = def.minor;
  p.corpse = def.corpse;
  p.burrows = def.burrows;
  p.jumper = def.jumper;
  p.teleporter = def.teleporter;
  p.explodes = def.explodes;
  p.burns = def.burns;
  p.ignore_flip = def.ignore_flip;
  p.neutral = def.neutral;
  p.non_grid = def.non_grid;
  p.leader = def.leader;
  p.mission_critical = def.mission;
  // ExtraSpaces: every shipped definition has at most one (Dam_Pawn (1,0),
  // the trains (0,1)); a second one would not be modelled.
  if (!def.extra_spaces.empty()) {
    p.extra_dx = static_cast<int8_t>(def.extra_spaces.front().x);
    p.extra_dy = static_cast<int8_t>(def.extra_spaces.front().y);
  }
  p.active = true;
  return p;
}

}  // namespace itb
