#include "itb/recording.hpp"

#include <algorithm>
#include <array>
#include <fstream>

#include <nlohmann/json.hpp>

#include "itb/game_data.hpp"
#include "itb/movement.hpp"
#include "itb/pilot_xp.hpp"
#include "itb/tile_rules.hpp"

namespace itb {
namespace {

using nlohmann::json;

template <typename T>
T get_or(const json& j, const char* key, T fallback) {
  auto it = j.find(key);
  if (it == j.end() || it->is_null()) return fallback;
  try {
    return it->get<T>();
  } catch (const json::exception&) {
    return fallback;
  }
}

Point get_point(const json& j, const char* key) {
  auto it = j.find(key);
  if (it == j.end() || !it->is_array() || it->size() < 2) return kInvalidPoint;
  return {(*it)[0].get<int>(), (*it)[1].get<int>()};
}

Terrain terrain_from(const json& t, Tile& tile, std::vector<std::string>& warnings) {
  const std::string name = get_or<std::string>(t, "terrain", "");
  const int id = get_or<int>(t, "terrain_id", -1);
  if (name == "ground") return Terrain::Road;
  if (name == "building") return Terrain::Building;
  if (name == "rubble") return Terrain::Rubble;
  if (name == "water") return Terrain::Water;
  if (name == "mountain") return Terrain::Mountain;
  if (name == "ice") return Terrain::Ice;
  if (name == "forest") return Terrain::Forest;
  if (name == "sand") return Terrain::Sand;
  if (name == "chasm") return Terrain::Hole;
  if (name == "lava") {
    // Bridges before 2026-05-04 named terrain id 5 (ice) "lava" by mistake.
    // Real lava is water (id 3) with the lava flag (IsTerrain(TERRAIN_LAVA)).
    if (id == static_cast<int>(Terrain::Ice) && !get_or<bool>(t, "lava", false)) {
      warnings.push_back(kIceNamedLava);
      return Terrain::Ice;
    }
    tile.lava = true;
    return Terrain::Water;
  }
  warnings.push_back("unknown terrain '" + name + "'");
  return static_cast<Terrain>(get_or<int>(t, "terrain_id", 0));
}

void load_tile(const json& t, Board& board, std::vector<std::string>& warnings) {
  const Point p{get_or<int>(t, "x", -1), get_or<int>(t, "y", -1)};
  if (!p.valid()) {
    warnings.push_back("tile with invalid position");
    return;
  }
  Tile& tile = board.tile(p);
  tile = Tile{};
  tile.terrain = terrain_from(t, tile, warnings);
  if (tile.is_building() || tile.is_mountain()) {
    tile.hp = static_cast<int8_t>(get_or<int>(t, "building_hp", tile.is_mountain() ? 2 : 1));
    tile.max_hp = static_cast<int8_t>(tile.is_mountain() ? 2 : std::max<int>(tile.hp, 1));
    // The bridge reports "frozen" for anything frozen on the tile; only
    // structures carry their own frozen state.
    tile.frozen = get_or<bool>(t, "frozen", false);
    // A structure's shield (Science_Shield, Passive_AutoShields): the bridge
    // reports it only where the game exposes it (a Board:IsShield API), and
    // live_play adds the ones the engine's own predictions placed (vanilla
    // has no such API; a shielded building read as bare took the hit in the
    // end-of-turn prediction). A pawn's shield is its own "shield".
    tile.shield = get_or<bool>(t, "shield", false);
  }
  // The bridge only reports population on mountains, where it is
  // meaningless; recorded buildings are populated unless proven otherwise.
  if (tile.is_building()) tile.populated = true;
  if (tile.terrain == Terrain::Ice) tile.hp = tile.max_hp = 2;
  if (get_or<bool>(t, "fire", false)) tile.fire = FireState::Burning;
  tile.smoke = get_or<bool>(t, "smoke", false);
  tile.acid = get_or<bool>(t, "acid", false);
  tile.cracked = get_or<bool>(t, "cracked", false);
  if (get_or<bool>(t, "pod", false)) tile.pod = PodState::Present;
  tile.conveyor = static_cast<int8_t>(get_or<int>(t, "conveyor", -1));
  if (auto item = get_or<std::string>(t, "item", ""); !item.empty()) tile.item = intern(item);
  if (auto obj = get_or<std::string>(t, "objective_name", ""); !obj.empty()) {
    tile.unique_building = intern(obj);
  } else if (get_or<bool>(t, "unique_building", false)) {
    tile.unique_building = intern("unique_building");
  }
  // Bridge extension: live structure state the old fields leave implicit.
  if (tile.terrain == Terrain::Ice) {
    if (auto it = t.find("ice_hp"); it != t.end() && it->is_number_integer()) {
      tile.hp = static_cast<int8_t>(std::clamp(it->get<int>(), 1, 2));
    }
  }
  if (tile.is_building()) tile.populated = get_or<bool>(t, "populated", tile.populated);
  if (auto custom = get_or<std::string>(t, "custom_tile", ""); !custom.empty()) tile.custom_tile = intern(custom);
}

Pawn load_unit(const json& u, const GameData* data, std::vector<std::string>& warnings) {
  const std::string type = get_or<std::string>(u, "type", "");
  const int32_t uid = get_or<int>(u, "uid", -1);
  const Point pos{get_or<int>(u, "x", -1), get_or<int>(u, "y", -1)};

  Pawn p;
  const PawnDef* def = data ? data->pawn(type) : nullptr;
  if (def) {
    p = data->make_pawn(*def, uid, pos);
  } else {
    if (data) warnings.push_back("unknown pawn type '" + type + "'");
    p.uid = uid;
    p.type = intern(type);
    p.pos = pos;
  }
  // Recorded values win over definition defaults (pilots, upgrades, statuses).
  p.hp = static_cast<int8_t>(get_or<int>(u, "hp", p.hp));
  p.max_hp = static_cast<int8_t>(get_or<int>(u, "max_hp", p.max_hp));
  const int recorded_max = p.max_hp;
  if (p.hp > p.max_hp) {
    // Older bridges reported the type's base Health as max_hp, without the
    // pilot and upgrade bonuses that the live HP includes.
    warnings.push_back("hp above the recorded max_hp (max_hp raised)");
    p.max_hp = p.hp;
  }
  // The bridge reports the current effective speed (move, Pawn:GetMoveSpeed)
  // and a base: the Lua MoveSpeed in bridges before 2026-08, since then
  // Pawn:GetBaseMove() (every pilot and upgrade bonus, not the web). A base
  // other than the definition's MoveSpeed is the latter. Pilots and upgrades
  // aren't recorded separately, so the difference is kept as a standing
  // bonus; load_recording takes out the part base_move computes live
  // (live_move_modifiers) once the board's turn is known. Until then
  // pilot_bonus holds the raw difference.
  const int effective_move = get_or<int>(u, "move", p.move);
  const int recorded_base = get_or<int>(u, "base_move", effective_move);
  int full_move = effective_move;
  if (def && recorded_base != def->move_speed) {
    full_move = recorded_base;  // Pawn:GetBaseMove()
    p.move = static_cast<int8_t>(def->move_speed);
  } else {
    p.move = static_cast<int8_t>(recorded_base);
  }
  // A speed below the base is Pawn:GetMoveSpeed() reading 0 for a webbed (or
  // otherwise held) pawn at that moment, not a lasting penalty.
  if (effective_move < recorded_base) warnings.push_back("effective move below base move (ignored)");
  p.movement.pilot_bonus = static_cast<int8_t>(std::clamp(full_move - p.move, -100, 100));
  p.team = static_cast<Team>(get_or<int>(u, "team", static_cast<int>(p.team)));
  p.mech = get_or<bool>(u, "mech", p.mech);
  p.flying = get_or<bool>(u, "flying", p.flying);
  p.massive = get_or<bool>(u, "massive", p.massive);
  p.pushable = get_or<bool>(u, "pushable", p.pushable);
  // ExtraSpaces as the bridge reads them from the type (Lua _G[type]); the
  // definition's when not exported.
  if (auto it = u.find("extra_spaces"); it != u.end() && it->is_array()) {
    Point offset{0, 0};
    if (!it->empty() && (*it)[0].is_array() && (*it)[0].size() >= 2) {
      offset = Point{(*it)[0][0].get<int>(), (*it)[0][1].get<int>()};
    }
    if (it->size() > 1) warnings.push_back("pawn '" + type + "' has more than one extra space (first kept)");
    if (def && (offset.x != p.extra_dx || offset.y != p.extra_dy)) {
      warnings.push_back("pawn '" + type + "' extra_spaces differ from its definition (recorded kept)");
    }
    p.extra_dx = static_cast<int8_t>(offset.x);
    p.extra_dy = static_cast<int8_t>(offset.y);
  }
  p.armor = get_or<bool>(u, "armor", p.armor);
  p.minor = get_or<bool>(u, "minor", p.minor);
  p.active = get_or<bool>(u, "active", false);
  // A unit that already used its move this turn (not exported by older bridges).
  p.moved = get_or<bool>(u, "moved", false);
  // A pending bonus move (the save's iBonusShift): a Shifty (1) or Post_Move
  // (the base move) pilot's move after attacking. The bridge cannot read it;
  // the live driver sets it for a unit it saw attack this turn.
  p.movement.bonus_shift = static_cast<int8_t>(std::clamp(get_or<int>(u, "bonus_shift", 0), 0, 100));
  p.fire = get_or<bool>(u, "fire", false);
  p.frozen = get_or<bool>(u, "frozen", false);
  p.acid = get_or<bool>(u, "acid", false);
  p.shield = get_or<bool>(u, "shield", false);
  p.boosted = get_or<bool>(u, "boosted", false);
  // Webbed: the bridge's "grappled" is the game's own flag (Pawn:IsGrappled,
  // @0086e5d0: Pawn+0xFB1 on a living pawn). Its "web" passes through a
  // legacy guess that clears the flag when no known webbing Vek is alive and
  // misses spider eggs (live 2026-10-10, Mission_AcidStorm: mechs next to a
  // WebbEgg1 came out unwebbed), so the probe wins when present.
  if (auto g = u.find("grappled"); g != u.end() && g->is_boolean()) {
    p.webbed = g->get<bool>();
  } else {
    p.webbed = get_or<bool>(u, "web", false);
  }
  p.infected = get_or<bool>(u, "infected", false);
  // AE Injured has no Lua getter; live tooling sets it (engine_overrides).
  p.injured = get_or<bool>(u, "injured", false);

  // The exact weapon tables (upgrade suffix included) when the bridge
  // exports them (weapons_exact), else the type's SkillList as recorded;
  // a unit without either keeps its definition's (hand-written boards).
  auto weapons = u.find("weapons_exact");
  if (weapons == u.end() || !weapons->is_array() || weapons->empty()) weapons = u.find("weapons");
  if (weapons != u.end() && weapons->is_array()) {
    p.weapons = {};
    size_t i = 0;
    for (const json& w : *weapons) {
      if (i >= p.weapons.size()) {
        warnings.push_back(type + " has more than " + std::to_string(kMaxWeapons) + " weapons");
        break;
      }
      if (w.is_string()) p.weapons[i++] = intern(w.get<std::string>());
    }
  }
  // Uses left of limited weapons, by position in weapons_exact.
  if (auto it = u.find("weapon_slots"); it != u.end() && it->is_array()) {
    for (size_t i = 0; i < it->size() && i < p.uses.size(); ++i) {
      const json& ws = (*it)[i];
      if (!ws.is_object()) continue;
      // A weapon without its reactor cores cannot fire (and an unpowered
      // passive does nothing): no uses.
      if (!get_or<bool>(ws, "powered", true)) {
        p.uses[i] = 0;
        continue;
      }
      if (get_or<int>(ws, "limited", 0) <= 0) continue;
      int uses = get_or<int>(ws, "uses", -1);
      if (uses < 0) uses = get_or<int>(ws, "uses_saved", -1);
      if (uses >= 0) p.uses[i] = static_cast<int8_t>(std::min(uses, 100));
    }
  }
  // Lua traits for types the game data does not know.
  if (!def) {
    if (auto it = u.find("traits"); it != u.end() && it->is_object()) {
      const json& t = *it;
      p.leader = static_cast<Leader>(get_or<int>(t, "leader", 0));
      p.minor = get_or<bool>(t, "minor", p.minor);
      p.explodes = get_or<bool>(t, "explodes", p.explodes);
      p.ignore_smoke = get_or<bool>(t, "ignore_smoke", p.ignore_smoke);
      p.ignore_fire = get_or<bool>(t, "ignore_fire", p.ignore_fire);
      p.burns = get_or<bool>(t, "burns", p.burns);
      p.corpse = get_or<bool>(t, "corpse", p.corpse);
      p.jumper = get_or<bool>(t, "jumper", p.jumper);
      p.teleporter = get_or<bool>(t, "teleporter", p.teleporter);
      p.burrows = get_or<bool>(t, "burrows", p.burrows);
      p.neutral = get_or<bool>(t, "neutral", p.neutral);
      p.ignore_flip = get_or<bool>(t, "ignore_flip", p.ignore_flip);
      p.non_grid = get_or<bool>(t, "non_grid", p.non_grid);
    }
  }

  // AE pilot level-up skills (Pilot::GetAllPilotSkills order: Health, Move,
  // Grid, Reactor, Opener, Closer, Popular, Thick, Skilled, Invulnerable,
  // Adrenaline, Pain, Regen, Conservative), active from levels 1 and 2. Only
  // the ones that change rules map to abilities: Thick Skin (no fire, no
  // ACID) and Technician (Regen).
  // The ids and level are also kept on the pawn (Pawn::pilot_level...) for
  // level-ups during the turn (itb/pilot_xp.hpp).
  auto level_skill = [&](int slot, int level, int id) {
    (slot == 1 ? p.pilot_skill1 : p.pilot_skill2) = static_cast<int8_t>(std::clamp(id, -1, 127));
    if (level < slot) return;
    if (id == 7) p.pilot_abilities |= kPilotThick;
    if (id == 12) p.pilot_abilities |= kPilotRegen;
  };
  if (auto it = u.find("pilot_skills"); it != u.end() && it->is_array()) {
    const int level = get_or<int>(u, "pilot_level", 0);
    p.pilot_level = static_cast<int8_t>(std::clamp(level, 0, kPilotMaxLevel));
    for (const json& sk : *it) {
      if (!sk.is_string()) continue;
      const std::string text = sk.get<std::string>();
      const size_t eq = text.find('=');
      if (eq == std::string::npos) continue;
      const int slot = text.rfind("skill2", 0) == 0 ? 2 : 1;
      int id = -1;
      try {
        id = std::stoi(text.substr(eq + 1));
      } catch (const std::exception&) {
        continue;
      }
      level_skill(slot, level, id);
    }
  }
  // The bridge extension's pilot record also carries skill id 0 and the
  // save's XP ("exp": progress toward the next level).
  if (auto it = u.find("pilot"); it != u.end() && it->is_object()) {
    const int level = get_or<int>(*it, "level", 0);
    p.pilot_level = static_cast<int8_t>(std::clamp(level, 0, kPilotMaxLevel));
    level_skill(1, level, get_or<int>(*it, "skill1", -1));
    level_skill(2, level, get_or<int>(*it, "skill2", -1));
    const int xp = get_or<int>(*it, "xp", -1);
    p.pilot_xp = static_cast<int16_t>(xp < 0 ? -1 : std::min(xp, 1000));
  }
  // Level, XP and max HP come from the save, written at the start of the
  // turn; HP is live. A mech above its saved max HP leveled up since (only a
  // Health or Skilled level-up raises a mech's maximum during a turn): apply
  // that level-up, its XP starting again at 0.
  if (p.mech && p.hp > recorded_max && tracks_pilot_xp(p)) {
    const int next = p.pilot_level == 0 ? p.pilot_skill1 : p.pilot_skill2;
    if (next == static_cast<int>(LevelSkill::Health) || next == static_cast<int>(LevelSkill::Skilled)) {
      ++p.pilot_level;
      p.pilot_xp = 0;
      p.max_hp = static_cast<int8_t>(std::max<int>(p.hp, recorded_max + 2));
      warnings.push_back("saved pilot level behind the live HP (level-up this turn applied)");
    }
  }

  // A burrower underground (dove after a hit: Pawn::Burrow(-1, -1)). The
  // bridge omits these; live tooling carries them over from the engine's
  // prediction with x, y = the tile it dove from (Pawn::GetMoveOrigin).
  if (get_or<bool>(u, "underground", false)) {
    p.movement.prev_pos = p.pos;
    p.pos = kInvalidPoint;
    p.queued = QueuedShot{};
    return p;
  }

  if (get_or<bool>(u, "has_queued_attack", false)) {
    // The bridge does not report which weapon is queued; Vek carry one.
    p.queued.weapon = 0;
    p.queued.origin = get_point(u, "queued_origin");
    p.queued.target = get_point(u, "queued_target");
  } else if (auto it = u.find("queued_any"); it != u.end() && it->is_object()) {
    // Queued shots of non-enemy units (trains, rockets): the skill index
    // counts Move as 0.
    const int skill = get_or<int>(*it, "skill", -1);
    const Point target = get_point(*it, "target");
    if (skill >= 1 && skill <= kMaxWeapons && target.valid()) {
      p.queued.weapon = static_cast<int8_t>(skill - 1);
      p.queued.origin = get_point(*it, "origin");
      p.queued.target = target;
    }
  }
  return p;
}

// Squad passives (board.hpp Passive) by Lua weapon name; upgraded variants
// carry a suffix. Order matters: longer names first.
uint32_t passive_of(std::string_view weapon) {
  static const std::pair<const char*, uint32_t> kNames[] = {
      {"Passive_FriendlyFire_AB", kPassiveFriendlyFireAB | kPassiveFriendlyFire},
      {"Passive_FriendlyFire_A", kPassiveFriendlyFireA | kPassiveFriendlyFire},
      {"Passive_FriendlyFire_B", kPassiveFriendlyFireB | kPassiveFriendlyFire},
      {"Passive_FriendlyFire", kPassiveFriendlyFire},
      {"Passive_Boosters_A", kPassiveKickoff | kPassiveKickoffUpgraded},
      {"Passive_Boosters_AB", kPassiveKickoff | kPassiveKickoffUpgraded},
      {"Passive_Boosters", kPassiveKickoff},
      {"Passive_ForceAmp", kPassiveForceAmp},
      {"Passive_AutoShields", kPassiveAutoShield},
      {"Passive_FlameImmune", kPassiveFlameImmune},
      {"Passive_FireBoost", kPassiveFireBoost},
      {"Passive_HealingSmoke", kPassiveHealingSmoke},
      {"Passive_PlayerTurnShield", kPassivePlayerTurnShield},
      {"Passive_Psions", kPassivePsionLeech},
      {"Passive_Leech_A", kPassiveLeechKill | kPassiveLeechKillA},
      {"Passive_Leech", kPassiveLeechKill},
      {"Passive_Electric_A", kPassiveElectricSmoke | kPassiveElectricSmokeA},
      {"Passive_Electric", kPassiveElectricSmoke},
      {"Passive_Burrows", kPassiveBurrows},
      {"Passive_FastDecay", kPassiveFastDecay},
      {"Passive_VoidShock", kPassiveVoidShock},
      {"Passive_CritDefense", kPassiveCritDefense},
  };
  for (const auto& [name, bits] : kNames) {
    if (weapon.starts_with(name)) return bits;
  }
  return kPassiveNone;
}

std::vector<Point> point_list(const json& j) {
  std::vector<Point> out;
  if (!j.is_array()) return out;
  for (const json& p : j) {
    if (p.is_array() && p.size() >= 2 && p[0].is_number() && p[1].is_number()) {
      out.emplace_back(p[0].get<int>(), p[1].get<int>());
    }
  }
  return out;
}

std::optional<FinalEnvState> final_env(const json& s, const char* key) {
  auto it = s.find(key);
  if (it == s.end() || !it->is_object()) return std::nullopt;
  FinalEnvState f;
  f.complete = get_or<bool>(*it, "complete", false);
  f.mode = get_or<int>(*it, "mode", 0);
  f.phase = get_or<int>(*it, "phase", 0);
  f.instant = get_or<bool>(*it, "instant", false);
  if (auto l = it->find("locations"); l != it->end()) f.locations = point_list(*l);
  return f;
}

void load_objectives(const json& s, Recording& rec);
void load_mission_ext(const json& s, Recording& rec);

// MissionData::grid_defense, the game's Grid Defense chance (PowerBar::
// GetDefense 0x008ed5c0: base + overpower bonus + pilot bonus). An explicit
// "grid_defense" (percent) wins; else it is built from "grid_overflow", the
// save's GameData.overflow (the overpower bonus PowerBar keeps, at most 25),
// plus 3 for each squad pilot whose learned level-up skills include Grid
// (Pilot::GetDefBonus; SquadControl::GetDefBonus sums the squad), plus the
// base. Neither: -1 (unknown).
//
// The base is 15, or 0 when Settings::GetDifficulty() is Unfair at the
// moment PowerBar's constructor (0x008ed180) runs; nothing saves or reloads
// it. The power bar is built with each GameMap: by Game::StartGame (a new
// run, the difficulty just chosen) and by Game::LoadGame (Continue, Reset
// Turn), which builds the map before GameMap::LoadGame sets the save's
// difficulty. So a run continued right after the game starts gets the base
// of the settings file's difficulty, not the save's: runs 21-23 (Unfair)
// showed exactly overflow on the HUD, but run 22 after a restart (settings
// difficulty Hard) showed overflow + 15, up to 40%. The bridge exports the
// base it saw as "grid_defense_base"; without it the save's difficulty
// decides (right for a new run and after Reset Turn).
void load_grid_defense(const json& s, Recording& rec) {
  MissionData& m = rec.mission;
  if (auto it = s.find("grid_defense"); it != s.end() && it->is_number()) {
    m.grid_defense = std::clamp(it->get<int>(), 0, 100);
    return;
  }
  auto ov = s.find("grid_overflow");
  if (ov == s.end() || !ov->is_number()) return;
  int base = m.difficulty == 3 ? 0 : 15;
  if (auto b = s.find("grid_defense_base"); b != s.end() && b->is_number()) base = std::max(0, b->get<int>());
  int pct = base + std::max(0, ov->get<int>());
  constexpr int kGrid = static_cast<int>(LevelSkill::Grid);
  for (const Pawn& p : rec.board.pawns()) {
    if (!p.mech || p.team != Team::Player) continue;
    if ((p.pilot_level >= 1 && p.pilot_skill1 == kGrid) || (p.pilot_level >= 2 && p.pilot_skill2 == kGrid)) pct += 3;
  }
  m.grid_defense = std::clamp(pct, 0, 100);
}

// Stage 7 mission data: environment marks and the env/mission fields newer
// bridges export (stage 7 spec section 2.4).
void load_mission(const json& s, Recording& rec) {
  MissionData& m = rec.mission;
  m.mission_id = rec.mission_id;
  m.env_type = get_or<std::string>(s, "env_type", "");
  m.difficulty = get_or<int>(s, "difficulty", -1);
  if (auto it = s.find("environment_danger_v2"); it != s.end() && it->is_array()) {
    for (const json& d : *it) {
      if (!d.is_array() || d.size() < 2 || !d[0].is_number() || !d[1].is_number()) continue;
      DangerTile t;
      t.p = {d[0].get<int>(), d[1].get<int>()};
      if (d.size() > 2 && d[2].is_number()) t.damage = d[2].get<int>();
      if (d.size() > 3 && d[3].is_number()) t.kill = d[3].get<int>() != 0;
      if (d.size() > 4 && d[4].is_number()) t.flying_immune = d[4].get<int>();
      m.danger.push_back(t);
    }
  } else if (auto v1 = s.find("environment_danger"); v1 != s.end()) {
    for (Point p : point_list(*v1)) m.danger.push_back(DangerTile{p, 1, true, -1});
  }
  if (auto it = s.find("environment_freeze"); it != s.end()) m.freeze = point_list(*it);
  if (auto it = s.find("environment_tides_index"); it != s.end() && it->is_number()) m.tides_index = it->get<int>();
  if (auto it = s.find("environment_wind_dir"); it != s.end() && it->is_number()) {
    const int d = it->get<int>();
    if (d >= 0 && d < 4) m.wind_dir = static_cast<Dir>(d);
  }
  m.volcano = final_env(s, "mission_final_volcano");
  m.final_cave = final_env(s, "mission_final_cave");
  m.hacking_bot = get_or<int>(s, "mission_hacking_bot_id", -1);
  m.hacking_building = get_or<int>(s, "mission_hacking_hack_id", -1);
  if (auto it = s.find("is_infinite_spawn"); it != s.end() && it->is_boolean()) m.infinite_spawn = it->get<bool>();
  // Mission_Reactivation: the units the mission may still thaw, as a player
  // can know them (live_play.py builds the list from the units frozen at the
  // mission's start and the thaws it has seen; environments.cpp
  // ReactivationMission). Members are marked on the board's pawns. The
  // mission's own list (mission_state.instance.Enemies) is hidden state and
  // never read. Absent (old recordings): every frozen enemy is a candidate.
  if (auto it = s.find("reactivation_visible_list"); it != s.end() && it->is_array() &&
      m.mission_id == "Mission_Reactivation") {
    m.reactivation_known = true;
    for (Pawn& p : rec.board.pawns()) p.reactivation_listed = false;
    for (const json& uid : *it) {
      if (!uid.is_number()) continue;
      if (Pawn* p = rec.board.find_pawn(static_cast<int32_t>(uid.get<double>()))) p->reactivation_listed = true;
    }
  }
  for (const json& u : s["units"]) {
    if (get_or<bool>(u, "queued_launch", false) && !get_or<bool>(u, "is_extra_tile", false)) {
      m.launching.push_back(get_or<int>(u, "uid", -1));
    }
  }
  load_objectives(s, rec);
  load_mission_ext(s, rec);
}

std::vector<Point> xy_points(const json& j) {
  std::vector<Point> out;
  if (!j.is_array()) return out;
  for (const json& p : j) {
    if (p.is_object() && p.contains("x") && p.contains("y") && p["x"].is_number() && p["y"].is_number()) {
      out.emplace_back(p["x"].get<int>(), p["y"].get<int>());
    }
  }
  return out;
}

std::vector<std::string> strings(const json& j) {
  std::vector<std::string> out;
  if (!j.is_array()) return out;
  for (const json& v : j) {
    if (v.is_string()) out.push_back(v.get<std::string>());
  }
  return out;
}

// Bridge extension (stage 7 spec section 4): mission identity, instance
// dumps, zones, the environment's ordered locations and strike log, and
// whether every queued shot was exported.
void load_mission_ext(const json& s, Recording& rec) {
  MissionData& m = rec.mission;
  rec.bridge_ext_version = get_or<int>(s, "bridge_ext_version", 0);
  if (auto it = s.find("bridge_errors"); it != s.end() && it->is_array()) {
    for (const json& e : *it) {
      if (e.is_object()) {
        rec.bridge_errors.push_back(get_or<std::string>(e, "where", "?") + ": " + get_or<std::string>(e, "error", ""));
      }
    }
  }
  if (auto it = s.find("mission_state"); it != s.end() && it->is_object()) {
    const json& ms = *it;
    m.mission_key = get_or<int>(ms, "key", -1);
    m.turn_limit = get_or<int>(ms, "turn_limit", -1);
    if (auto c = ms.find("class_chain"); c != ms.end()) m.mission_classes = strings(*c);
    if (auto c = ms.find("env_class_chain"); c != ms.end()) m.env_classes = strings(*c);
    if (auto i = ms.find("instance"); i != ms.end()) m.mission_instance_json = i->dump();
    if (auto e = ms.find("env_instance"); e != ms.end() && e->is_object()) {
      m.env_instance_json = e->dump();
      // Env_Attack's planned Locations, in the order Ordered environments
      // strike them (Env_Seismic).
      if (auto l = e->find("Locations"); l != e->end() && m.ordered_locations.empty()) {
        m.ordered_locations = xy_points(*l);
      }
      // Env_Belt's Belts and BeltsDir, in list order (the push order).
      auto belts = e->find("Belts");
      auto dirs = e->find("BeltsDir");
      // An empty Lua table dumps as {}: no belts left (CheckBelts dropped them).
      auto empty = [](const json& j) { return j.is_object() && j.empty(); };
      if (belts != e->end() && dirs != e->end() && empty(*belts) && empty(*dirs)) {
        m.belts.clear();
        m.belts_known = true;
      } else if (belts != e->end() && dirs != e->end() && belts->is_array() && dirs->is_array() &&
                 belts->size() == dirs->size()) {
        const std::vector<Point> pts = xy_points(*belts);
        bool ok = pts.size() == belts->size();
        std::vector<std::pair<Point, Dir>> list;
        for (size_t i = 0; ok && i < pts.size(); ++i) {
          const json& d = (*dirs)[i];
          ok = pts[i].valid() && d.is_number_integer() && d.get<int>() >= 0 && d.get<int>() < 4;
          if (ok) list.emplace_back(pts[i], static_cast<Dir>(d.get<int>()));
        }
        if (ok) {
          m.belts = std::move(list);
          m.belts_known = true;
        } else {
          rec.warnings.push_back("env_instance Belts/BeltsDir unreadable (conveyors from the tiles)");
        }
      }
    }
  }
  if (auto it = s.find("zones"); it != s.end() && it->is_object()) {
    for (const auto& [name, pts] : it->items()) m.zones[name] = point_list(pts);
  }
  if (auto it = s.find("attack_order_all"); it != s.end() && it->is_array()) {
    m.all_queued_known = true;
    for (const json& uid : *it) {
      if (uid.is_number_integer()) rec.attack_order_all.push_back(uid.get<int32_t>());
    }
  }
  if (auto it = s.find("env_strike_log"); it != s.end() && it->is_array()) {
    for (const json& e : *it) {
      if (!e.is_object()) continue;
      Recording::EnvStrike strike;
      strike.turn = get_or<int>(e, "turn", -1);
      if (auto a = e.find("current_attack"); a != e.end()) {
        strike.tiles = a->is_object() ? xy_points(json::array({*a})) : xy_points(*a);
      }
      rec.env_strikes.push_back(std::move(strike));
    }
  }
}

// The tiles' conveyor directions. The bridge's `conveyor` field comes from a
// scan of the whole save file, which holds every region's map: on an island
// with another belt map it reports phantom belts, on belt missions and others
// alike, and wrong directions (live m34, Mission_BeltRandom: the B4 and C4
// belts as pushing left, not down). The live sources win: the Env_Belt
// instance's Belts/BeltsDir, else the "conveyor<dir>.png" custom tiles
// Env_Belt:MarkBoard draws (CheckBelts clears a broken belt's tile), which
// the bridge extension exports for every tile (custom_tile).
void reconcile_conveyors(Recording& rec) {
  Board& b = rec.board;
  const MissionData& m = rec.mission;
  std::array<int8_t, kTileCount> dir;
  dir.fill(-1);
  bool live = rec.bridge_ext_version >= 1;
  if (m.belts_known) {
    live = true;
    for (const auto& [p, d] : m.belts) dir[p.index()] = static_cast<int8_t>(d);
  } else {
    for (int i = 0; i < kTileCount; ++i) {
      const Tile& t = b.tile(Point::from_index(i));
      if (t.custom_tile == kNoSymbol) continue;
      const std::string_view name = symbol_name(t.custom_tile);
      if (name.size() == 13 && name.starts_with("conveyor") && name.ends_with(".png") && name[8] >= '0' &&
          name[8] <= '3') {
        dir[i] = static_cast<int8_t>(name[8] - '0');
        live = true;  // older bridges with custom_tile: the sprites are live too
      }
    }
  }
  if (!live) return;  // old bridges: the conveyor field is all there is
  int fixed = 0;
  for (int i = 0; i < kTileCount; ++i) {
    Tile& t = b.tile(Point::from_index(i));
    if (t.conveyor != dir[i]) {
      t.conveyor = dir[i];
      ++fixed;
    }
  }
  if (fixed > 0) {
    rec.warnings.push_back("conveyor field disagrees with the live belts on " + std::to_string(fixed) +
                           " tile(s) (save scan); using the live belts");
  }
}

// The queued spawns' types and queue order (bridge spawn_queue). When the
// queue matches the markers, Board::spawn_points take the queue order;
// otherwise each marker gets the type queued on its tile, if unambiguous.
void load_spawn_queue(const json& s, Recording& rec) {
  Board& b = rec.board;
  auto it = s.find("spawn_queue");
  if (it == s.end() || !it->is_array()) return;
  std::vector<std::pair<Point, std::string>> queue;
  for (const json& e : *it) {
    if (!e.is_object()) continue;
    queue.emplace_back(Point{get_or<int>(e, "x", -1), get_or<int>(e, "y", -1)}, get_or<std::string>(e, "type", ""));
  }
  if (get_or<bool>(s, "spawn_queue_matches_markers", false)) {
    b.spawn_points.clear();
    rec.spawn_types.clear();
    for (const auto& [p, type] : queue) {
      b.spawn_points.push_back(p);
      rec.spawn_types.push_back(type);
    }
    rec.spawn_order_known = true;
    return;
  }
  rec.warnings.push_back("spawn_queue does not match the spawn markers (types by tile only)");
  rec.spawn_types.assign(b.spawn_points.size(), "");
  for (size_t i = 0; i < b.spawn_points.size(); ++i) {
    std::string type;
    bool unique = true;
    for (const auto& [p, t] : queue) {
      if (p != b.spawn_points[i]) continue;
      if (!type.empty() && type != t) unique = false;
      type = t;
    }
    if (unique) rec.spawn_types[i] = type;
  }
}

// Stage 8: the mission's objective bookkeeping (environment.hpp ObjectiveData).
void load_objectives(const json& s, Recording& rec) {
  ObjectiveData& o = rec.mission.objectives;
  if (auto it = s.find("bonus_objective_ids"); it != s.end() && it->is_array()) {
    o.bonus_known = true;
    for (const json& b : *it) {
      if (b.is_number_integer()) o.bonus.push_back(b.get<int>());
    }
  }
  o.kills_done = get_or<int>(s, "mission_kills_done", -1);
  o.kill_target = get_or<int>(s, "mission_kill_target", -1);
  o.kill_limit = get_or<int>(s, "mission_kill_limit", -1);
  o.blocked_spawns = get_or<int>(s, "mission_blocked_spawns", -1);
  o.power_start = get_or<int>(s, "mission_power_start", -1);
  // PowerStart is the grid at deployment; nothing can damage the grid before
  // the first player turn.
  if (o.power_start < 0 && rec.board.turn == 1 && s.contains("grid_power")) o.power_start = rec.board.grid_power;
  o.repair_target = get_or<int>(s, "repair_platform_target", -1);
  o.repairs_done = get_or<int>(s, "repair_platforms_used", -1);
  o.mountain_target = get_or<int>(s, "mission_mountain_target", -1);
  o.mountains_done = get_or<int>(s, "mission_mountains_destroyed", -1);
  o.freeze_target = get_or<int>(s, "freeze_building_target", -1);
  if (auto it = s.find("freeze_building_tiles"); it != s.end()) o.freeze_buildings = point_list(*it);
  if (auto it = s.find("terraform_grass_tiles"); it != s.end() && it->is_array()) {
    o.grass = point_list(*it);
    o.grass_known = true;
  }
}

// Weapons whose queued attack webs its target tile from the attacker's:
// ScorpionAtk1:GetSkillEffect adds AddGrapple(p1, p2) when Web == 1
// (weapons_enemy.lua: ScorpionAtk1/2, ScorpionAtk_Acid and LeaperAtk1/2
// inherit it), MosquitoAtk1 when Webbing (MosquitoAtkB).
bool webs_its_target(std::string_view w) {
  return w == "ScorpionAtk1" || w == "ScorpionAtk2" || w == "ScorpionAtk_Acid" || w == "LeaperAtk1" ||
         w == "LeaperAtk2" || w == "MosquitoAtkB";
}

// Whether the pawn on `from` webs tile `to` (a neighbour) by what the board
// shows: a queued webbing attack on `to`, the Scorpion Leader's queued
// ScorpionAtkB (webs every neighbour), or a spider egg (SpiderAtk webs the
// egg tile's neighbours when it lands).
bool shows_web(const Pawn& src, Point from, Point to) {
  if (src.team != Team::Enemy) return false;  // the web check breaks it at once
  if (symbol_name(src.type).find("WebbEgg") != std::string_view::npos) return true;
  if (!src.queued.active() || src.queued.weapon >= static_cast<int>(src.weapons.size())) return false;
  if (src.queued.origin.valid() && src.queued.origin != from) return false;
  const std::string_view w = symbol_name(src.weapons[static_cast<size_t>(src.queued.weapon)]);
  if (w == "ScorpionAtkB") return true;
  return webs_its_target(w) && src.queued.target == to;
}

// Webs are tile state (Tile::web_out / web_in), made when the webber acts:
// a Vek's queued attack runs its AddGrapple at once. The bridge reports only
// whether a pawn is webbed (IsGrappled) and at most one source, which it
// guesses (the nearest webber). Every neighbour of a webbed pawn's tiles
// (its extra tile too) whose pawn shows a web onto it is a source; when none
// does, the reported source if it stands next to the pawn; else the pawn
// keeps the flag alone (no tile to release it).
void rebuild_webs(Board& b, const std::vector<std::pair<int32_t, int32_t>>& reported,
                  std::vector<std::string>& warnings) {
  auto add = [&](Point from, Point to) {
    const Point d = to - from;
    const int dir = d.y < 0 ? 0 : d.x > 0 ? 1 : d.y > 0 ? 2 : 3;
    Tile& t = b.tile(from);
    if (t.web_out >> dir & 1) return;
    t.web_out = static_cast<uint8_t>(t.web_out | (1u << dir));
    ++b.tile(to).web_in;
  };
  for (Pawn& w : b.pawns()) {
    if (!w.webbed || !w.pos.valid()) continue;
    std::vector<Point> tiles{w.pos};
    if (const Point e = w.extra_tile(); e.valid()) tiles.push_back(e);
    bool found = false;
    for (Point to : tiles) {
      for (int d = 0; d < 4; ++d) {
        const Point from = step(to, static_cast<Dir>(d));
        if (!from.valid() || w.occupies(from) || !has_pawn(b, from)) continue;
        if (shows_web(*first_occupant(b, from), from, to)) {
          add(from, to);
          found = true;
        }
      }
    }
    if (found) continue;
    int32_t hint = -1;
    for (const auto& [uid, src] : reported) {
      if (uid == w.uid) hint = src;
    }
    const Pawn* src = hint >= 0 ? b.find_pawn(hint) : nullptr;
    for (Point to : tiles) {
      if (!src || found) break;
      for (int d = 0; d < 4 && !found; ++d) {
        const Point from = step(to, static_cast<Dir>(d));
        if (from.valid() && !w.occupies(from) && src->occupies(from)) {
          add(from, to);
          found = true;
        }
      }
    }
    if (!found) {
      warnings.push_back("webbed " + std::string(symbol_name(w.type)) + "#" + std::to_string(w.uid) +
                         " has no web source next to it (flag only)");
    }
  }
}

// Whether a bridge "pilot" record shows the pilot's own Skill off. In the
// game (Pilot::IsAbility 0x00859010) a Skill whose pilot has a PowerCost
// (Silica's Double_Shot 2; Gana's Deploy_Anywhere, Archimedes' Post_Move,
// Prospero's Flying 1) only counts while the pilot's PowerState is powered
// (0x0051e290: non-empty, first cell set); the mech bay sets it. The live
// "skill_active" (Pawn:IsAbility(Skill)) wins; else the save's "power"
// cells, e.g. {0, 0} for an unpowered Silica. Unknown: on. A Skill without
// a PowerCost is always on.
bool pilot_skill_unpowered(const json& pilot, const std::string& id, const GameData* data) {
  if (!data || data->pilot_power_cost(id) <= 0) return false;
  if (auto it = pilot.find("skill_active"); it != pilot.end() && it->is_boolean()) return !it->get<bool>();
  auto it = pilot.find("power");
  if (it == pilot.end()) return false;
  // An empty Lua table may come out as {} as well as [].
  if (it->is_object()) return it->empty();
  if (!it->is_array()) return false;
  return it->empty() || !(*it)[0].is_number() || (*it)[0].get<double>() == 0;
}

}  // namespace

std::optional<Recording> load_recording(const std::filesystem::path& path,
                                        const GameData* data, std::string* error) {
  auto fail = [&](const std::string& msg) -> std::optional<Recording> {
    if (error) *error = path.string() + ": " + msg;
    return std::nullopt;
  };

  std::ifstream in(path);
  if (!in) return fail("cannot open");
  json root;
  try {
    in >> root;
  } catch (const json::exception& e) {
    return fail(std::string("bad JSON: ") + e.what());
  }

  Recording rec;
  const json* state = &root;
  if (root.contains("data")) {
    rec.run_id = get_or<std::string>(root, "run_id", "");
    rec.mission_index = get_or<int>(root, "mission_index", 0);
    rec.turn = get_or<int>(root, "turn", 0);
    rec.label = get_or<std::string>(root, "label", "");
    const json& d = root["data"];
    if (!d.is_object() || !d.contains("bridge_state")) return fail("no data.bridge_state");
    state = &d["bridge_state"];
  }
  const json& s = *state;
  if (!s.contains("tiles") || !s.contains("units")) return fail("not a bridge board");

  rec.mission_id = get_or<std::string>(s, "mission_id", "");
  rec.phase = get_or<std::string>(s, "phase", "");
  // A dump taken while effects were still resolving (bridge `stable`
  // false): pushes in flight have not moved their pawns yet, deaths may
  // be pending. Loaded as is, flagged for the caller.
  rec.board_busy = get_or<bool>(s, "board_busy", false) || get_or<bool>(s, "command_waiting", false);
  if (rec.board_busy) {
    rec.warnings.push_back("state dumped while the board was busy (busy_state " +
                           std::to_string(get_or<int>(s, "busy_state", -1)) +
                           "): positions and HP may be mid-animation");
  }
  Board& b = rec.board;
  b.grid_power = get_or<int>(s, "grid_power", b.grid_power);
  b.grid_power_max = get_or<int>(s, "grid_power_max", b.grid_power_max);
  b.turn = get_or<int>(s, "turn", b.turn);
  b.total_turns = get_or<int>(s, "total_turns", b.total_turns);

  for (const json& t : s["tiles"]) load_tile(t, b, rec.warnings);
  std::vector<std::pair<int32_t, int>> mutations;  // uid -> recorded mutation
  std::vector<std::pair<int32_t, Point>> extra_entries;  // uid -> reported extra tile
  std::vector<std::pair<int32_t, int32_t>> web_reported;  // webbed uid -> reported source uid
  for (const json& u : s["units"]) {
    // Multi-tile pawns are reported once more per extra tile (same uid,
    // "is_extra_tile"); the main entry carries the pawn, the extra tile
    // follows from its ExtraSpaces (checked below).
    if (get_or<bool>(u, "is_extra_tile", false)) {
      extra_entries.emplace_back(get_or<int>(u, "uid", -1), Point{get_or<int>(u, "x", -1), get_or<int>(u, "y", -1)});
      continue;
    }
    Pawn& p = b.add_pawn(load_unit(u, data, rec.warnings));
    if (p.webbed) web_reported.emplace_back(p.uid, get_or<int>(u, "web_source_uid", -1));
    std::string pilot = get_or<std::string>(u, "pilot_id", "");
    bool skill_off = false;
    if (auto it = u.find("pilot"); it != u.end() && it->is_object()) {
      Recording::PilotInfo info;
      info.uid = p.uid;
      info.id = get_or<std::string>(*it, "id", "");
      info.level = get_or<int>(*it, "level", -1);
      info.xp = get_or<int>(*it, "xp", -1);
      info.skill1 = get_or<int>(*it, "skill1", -1);
      info.skill2 = get_or<int>(*it, "skill2", -1);
      if (pilot.empty()) pilot = info.id;
      info.skill_unpowered = pilot_skill_unpowered(*it, pilot, data);
      skill_off = info.skill_unpowered;
      rec.pilot_info.push_back(std::move(info));
    }
    if (!pilot.empty() && !skill_off) {
      rec.pilots.emplace_back(p.uid, pilot);
      // The pilot's own ability (its Lua Skill) now, before the standing move
      // bonus below takes out what base_move computes live: Youth_Move and
      // Arrogant_Boost are part of that, and the recorded speed includes them.
      if (data) p.pilot_abilities |= data->pilot_ability(pilot);
    }
    if (auto it = u.find("mutation"); it != u.end() && it->is_number_integer()) {
      mutations.emplace_back(p.uid, it->get<int>());
    }
  }
  rebuild_webs(b, web_reported, rec.warnings);
  for (const Pawn& p : b.pawns()) {
    if (p.mech && p.team == Team::Player) {
      for (size_t i = 0; i < p.weapons.size(); ++i) {
        const Symbol w = p.weapons[i];
        // uses 0 here = recorded unpowered (a passive has no uses otherwise).
        if (w != kNoSymbol && p.uses[i] != 0) b.passives |= passive_of(symbol_name(w));
      }
    }
  }
  // Board::UpdateLeaders: the last living leader in list order.
  for (const Pawn& p : b.pawns()) {
    if (p.alive() && p.leader != Leader::None) b.psion = p.leader;
  }
  // Pawn turn count (Pawn::StartTurn, once per player turn): a mech has had
  // one per turn so far. Then the standing move bonus: the recorded speed
  // minus what base_move computes live.
  for (Pawn& p : b.pawns()) {
    if (p.mech && p.team == Team::Player) p.movement.turn_count = static_cast<int8_t>(std::clamp(b.turn, 0, 100));
    p.movement.pilot_bonus =
        static_cast<int8_t>(std::max(0, p.movement.pilot_bonus - live_move_modifiers(b, p)));
  }
  // Recorded HP already includes the Soldier psion's or the Abomination's +1.
  for (Pawn& p : b.pawns()) {
    if (p.alive() && mutation_adds_health(b.psion) && mutation_affects(b, p, b.psion)) p.health_bonus = true;
  }
  // The save's per-pawn mutation (turn start) settles it, stale ones included.
  // A psion carries its own mutation but never gets its effect.
  for (const auto& [uid, mutation] : mutations) {
    if (Pawn* p = b.find_pawn(uid)) {
      const Leader m = static_cast<Leader>(mutation);
      p->health_bonus = mutation_adds_health(m) && p->leader != m;
    }
  }
  // The bridge's `boosted` is Pawn:IsBoosted(), which includes the Boost
  // psion: only a boost the psion does not explain is the pawn's status.
  // The same holds for an Arrogant_Boost pilot at full health: IsBoosted is
  // true while the mech is undamaged and false again as soon as it is hit,
  // so that boost is not a status the pawn keeps (live fuzz 2026-10-16: a
  // Kickback after a friendly hit took 1 self damage, not 2). A Boosted
  // status on such a mech is indistinguishable here and is dropped too.
  for (Pawn& p : b.pawns()) {
    if (p.boosted && mutation_affects(b, p, Leader::Boosted)) p.boosted = false;
    if (p.boosted && p.has_pilot(kPilotArrogantBoost) && p.hp >= p.max_hp) p.boosted = false;
  }
  rec.difficulty = get_or<int>(s, "difficulty", -1);
  if (auto it = s.find("spawning_tiles"); it != s.end() && it->is_array()) {
    for (const json& p : *it) {
      if (p.is_array() && p.size() >= 2) b.spawn_points.emplace_back(p[0].get<int>(), p[1].get<int>());
    }
  }
  for (const auto& [uid, at] : extra_entries) {
    const Pawn* p = b.find_pawn(uid);
    if (!p) {
      rec.warnings.push_back("extra tile of unknown pawn " + std::to_string(uid));
    } else if (p->extra_tile() != at) {
      rec.warnings.push_back("extra tile of " + std::string(symbol_name(p->type)) + "#" + std::to_string(uid) +
                             " reported at (" + std::to_string(at.x) + "," + std::to_string(at.y) +
                             "), not at its ExtraSpaces offset");
    }
  }
  // Teleporter pads: [x1, y1, x2, y2] per pair. Whoever stands on a pad
  // now has already arrived there.
  if (auto it = s.find("teleporter_pairs"); it != s.end() && it->is_array()) {
    for (const json& pair : *it) {
      if (!pair.is_array() || pair.size() < 4) continue;
      for (int k = 0; k < 2; ++k) {
        const Point p{pair[2 * k].get<int>(), pair[2 * k + 1].get<int>()};
        if (!p.valid()) continue;
        b.teleporters.push_back(p);
        b.tile(p).teleporter = true;
        const Pawn* on = b.pawn_at(p);
        b.teleporter_occupants.push_back(on && on->alive() ? on->uid : -1);
      }
    }
  }
  load_mission(s, rec);
  load_grid_defense(s, rec);
  reconcile_conveyors(rec);
  load_spawn_queue(s, rec);
  if (auto it = s.find("attack_order"); it != s.end() && it->is_array()) {
    for (const json& uid : *it) {
      if (uid.is_number_integer()) rec.attack_order.push_back(uid.get<int32_t>());
    }
  }
  return rec;
}

TurnContext turn_context(const Recording& rec, Visibility visibility) {
  TurnContext ctx;
  ctx.mission = rec.mission;
  if (visibility == Visibility::Full) {
    ctx.spawn_types = rec.spawn_types;
    ctx.spawn_order_known = rec.spawn_order_known;
  }
  return ctx;
}

}  // namespace itb
