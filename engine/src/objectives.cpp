// Stage 8: mission objectives and the position metric (objectives.hpp).
//
// Behaviour follows the shipped mission scripts: missions.lua (Mission base,
// BONUS_* and GetBonusStatus), missions/** and advanced/missions/** (each
// mission's GetCompletedObjectives / UpdateObjectives / counters). Which pawn
// deaths raise EVENT_ENEMY_KILLED / EVENT_ACID_DESTROYED follows
// Pawn::ProcessDeath; GetMechDamage and GetInfectedCount follow Board.
#include "itb/objectives.hpp"

#include <algorithm>
#include <string_view>

#include "itb/enemy_phase.hpp"
#include "itb/environment.hpp"
#include "itb/symbols.hpp"

namespace itb {
namespace {

std::string_view type_name(const Pawn& p) { return symbol_name(p.type); }

// Mission_Terraform grass: the grass sprite when the recording has sprites
// (any of the zone's tiles shows one), else not sand.
bool grass_sprites(const Board& b, const std::vector<Point>& zone) {
  for (Point g : zone) {
    if (b.tile(g).custom_tile != kNoSymbol && symbol_name(b.tile(g).custom_tile) == "ground_grass.png") return true;
  }
  return false;
}
bool is_grass(const Board& b, Point g, bool sprites) {
  const Tile& t = b.tile(g);
  if (!sprites) return t.terrain != Terrain::Sand;
  return t.custom_tile != kNoSymbol && symbol_name(t.custom_tile) == "ground_grass.png";
}
bool type_is(const Pawn& p, std::string_view t) { return type_name(p) == t; }

// Board:IsPawnAlive.
bool alive(const Pawn* p) { return p && p->alive() && !p->fallen; }

struct Turn {
  const Board& before;
  const Board& after;
  const PhaseResult* phase;
};

// A pawn on `after` that was not on `before` (uids can be reused).
bool is_new(const Pawn& n, const Turn& t) {
  const Pawn* b = t.before.find_pawn(n.uid);
  return !b || same_pawn(*b, t.after, t.phase) != &n;
}

enum class Fate : uint8_t { Alive, Died, Left };

// What became of `p` (alive at the start) by the end of the turn. Leaving
// without dying: a retreat (tile_rules retreat: the minor flag, HP 0), a
// satellite rocket that launched (FlyAway removes it), the Mission_Hacking
// bot swapped for its player-team copy on the same tile.
Fate fate(const Pawn& p, const Turn& t) {
  const Pawn* q = same_pawn(p, t.after, t.phase);
  if (alive(q)) return Fate::Alive;
  if (q && !p.minor && q->minor && !q->fallen && q->hp <= 0) return Fate::Left;
  if (!q && type_is(p, "SatelliteRocket")) return Fate::Left;
  if (!q && type_is(p, "Snowtank1")) {
    for (const Pawn& n : t.after.pawns()) {
      if (type_is(n, "Snowtank1_Player") && n.pos == p.pos && is_new(n, t)) return Fate::Left;
    }
  }
  return Fate::Died;
}

// Pawns alive at the start matching `pred` that died this turn.
template <typename Pred>
int died(const Turn& t, Pred pred) {
  int n = 0;
  for (const Pawn& p : t.before.pawns()) {
    if (alive(&p) && pred(p) && fate(p, t) == Fate::Died) ++n;
  }
  return n;
}

template <typename Pred>
int count_alive(const Board& b, Pred pred) {
  int n = 0;
  for (const Pawn& p : b.pawns()) n += alive(&p) && pred(p) ? 1 : 0;
  return n;
}

bool is_enemy(const Pawn& p) { return p.team == Team::Enemy; }

// Board:GetMechDamage: max HP minus HP over every mech, dead ones included.
int mech_damage(const Board& b) {
  int n = 0;
  for (const Pawn& p : b.pawns()) {
    if (p.mech) n += p.max_hp - std::max<int>(0, p.hp);
  }
  return n;
}

int infected(const Board& b) { return count_alive(b, [](const Pawn& p) { return p.infected; }); }

int mountains(const Board& b) {
  int n = 0;
  for (int i = 0; i < kTileCount; ++i) n += b.tile(Point::from_index(i)).is_mountain() ? 1 : 0;
  return n;
}

int fires(const Board& b) {
  int n = 0;
  for (int i = 0; i < kTileCount; ++i) n += b.tile(Point::from_index(i)).on_fire() ? 1 : 0;
  return n;
}

// Mission:GetKillBonus / GetPacifistCount (DIFF_EASY 0, DIFF_UNFAIR 3).
int kill_bonus(int difficulty) { return difficulty == 0 ? 5 : 7; }
int pacifist_count(int difficulty) { return difficulty == 3 ? 6 : difficulty == 0 ? 4 : 5; }

// Progress toward `target` of something counted from `done`, in kStar units
// for `stars` stars spread evenly over the target.
int toward(int done, int gained, int target, int stars = 1) {
  if (target <= 0) return 0;
  auto units = [&](int c) { return std::clamp(c, 0, target) * stars * kStar / target; };
  return units(done + gained) - units(done);
}

// A counted objective's progress (kills, blocks, mountains, repair
// platforms) scaled by its pace: the units still needed per turn left, this
// one included, up to kMaxPace. Two such objectives compete in one tier, and
// unscaled the one with the smaller target always won: run 7's Mission_Force
// took 1 of 2 mountains (420) over 2 of 7 kills (240) on turn 1, finished the
// mountains on turn 4 anyway and missed the kills; paced (mountains 1/turn
// needed of 4 -> x0.5, kills 7/4 -> x1.75) the replay took the kills.
constexpr int kMaxPace = 4;
int paced(int progress, int done, int target, const Board& before) {
  if (target <= 0) return progress;
  const int needed = std::max(1, target - std::max(0, done));
  const int turns = std::max(1, before.total_turns - before.turn + 1);
  return progress * std::min(needed, kMaxPace * turns) / turns;
}

const char* bonus_name(BonusId b) {
  switch (b) {
    case BonusId::Asset: return "BONUS_ASSET";
    case BonusId::Kill: return "BONUS_KILL";
    case BonusId::Grid: return "BONUS_GRID";
    case BonusId::Mechs: return "BONUS_MECHS";
    case BonusId::Block: return "BONUS_BLOCK";
    case BonusId::KillFive: return "BONUS_KILL_FIVE";
    case BonusId::Debris: return "BONUS_DEBRIS";
    case BonusId::SelfDamage: return "BONUS_SELFDAMAGE";
    case BonusId::Pacifist: return "BONUS_PACIFIST";
  }
  return "BONUS_?";
}

std::string num(int v) { return std::to_string(v); }

}  // namespace

const Pawn* same_pawn(const Pawn& p, const Board& after, const PhaseResult* phase) {
  const Pawn* q = after.find_pawn(p.uid);
  if (!q || q->type != p.type) return nullptr;
  if (phase) {
    for (const PhaseEvent& e : phase->events) {
      if (e.type == PhaseEventType::SpawnEmerged && e.uid == p.uid) return nullptr;
    }
  }
  return q;
}

int ObjectiveReport::failed() const {
  int n = 0;
  for (const ObjectiveLine& l : lines) n += l.failed;
  return n;
}

int ObjectiveReport::progress() const {
  int n = 0;
  for (const ObjectiveLine& l : lines) n += l.progress;
  return n;
}

bool ObjectiveReport::exact() const {
  return std::all_of(lines.begin(), lines.end(), [](const ObjectiveLine& l) { return l.exact; });
}

std::vector<BonusId> active_bonuses(const Board& board, const MissionData& m) {
  const ObjectiveData& od = m.objectives;
  std::vector<BonusId> out;
  if (od.bonus_known) {
    for (int id : od.bonus) {
      if (id >= 1 && id <= 9) out.push_back(static_cast<BonusId>(id));
    }
    return out;
  }
  // The bridge sets mission_kill_target for BONUS_KILL_FIVE, except on
  // Mission_AcidTank where it is the mission's own acid-kill goal.
  if (od.kill_target >= 0 && m.mission_id != "Mission_AcidTank") out.push_back(BonusId::KillFive);
  if (od.kill_limit >= 0) out.push_back(BonusId::Pacifist);
  for (int i = 0; i < kTileCount; ++i) {
    const Tile& t = board.tile(Point::from_index(i));
    if (t.unique_building != kNoSymbol && !symbol_name(t.unique_building).starts_with("Mission_")) {
      out.push_back(BonusId::Asset);
      break;
    }
  }
  bool debris = false, mites = false;
  for (const Pawn& p : board.pawns()) {
    debris = debris || type_is(p, "BonusDebris");
    mites = mites || (p.mech && p.infected);
  }
  if (debris) out.push_back(BonusId::Debris);
  if (mites) out.push_back(BonusId::SelfDamage);
  return out;
}

ObjectiveReport evaluate_objectives(const Board& before, const Board& after, const TurnContext* ctx,
                                    const PhaseResult* phase) {
  static const MissionData kNoMission;
  const MissionData& m = ctx ? ctx->mission : kNoMission;
  const ObjectiveData& od = m.objectives;
  const std::string& id = m.mission_id;
  const int difficulty = m.difficulty;
  ObjectiveReport r;
  r.mission_ends = phase ? phase->mission_ended : before.turn >= before.total_turns;
  const bool ends = r.mission_ends;
  const Turn turn{before, after, phase};

  // ---- Native event counters.
  for (const Pawn& p : before.pawns()) {
    if (!alive(&p) || p.team != Team::Enemy || p.mech || p.minor) continue;
    if (fate(p, turn) == Fate::Died) ++r.enemy_kills;
  }
  // Counted as deaths are processed (Simulation::process_death): the ACID
  // that matters is the one the pawn has as it dies, and the pawn is gone
  // from `after`. Pawns created during the turn count too.
  r.acid_kills = after.acid_destroyed - before.acid_destroyed;
  if (phase) r.spawns_blocked = phase->count(PhaseEventType::SpawnBlocked);
  for (int i = 0; i < kTileCount; ++i) {
    const Point pt = Point::from_index(i);
    const Tile& a = before.tile(pt);
    const Tile& b = after.tile(pt);
    if (a.is_mountain() && b.terrain == Terrain::Rubble) ++r.mountains_destroyed;
    if (a.item != kNoSymbol && symbol_name(a.item) == "Item_Repair_Mine" && b.item != a.item) {
      const Pawn* on = nullptr;
      for (const Pawn& p : after.pawns()) {
        if (p.occupies(pt) && alive(&p)) on = &p;
      }
      if (on && on->team == Team::Player) ++r.repairs_used;
    }
  }

  auto line = [&](std::string name, int failed, int progress, bool exact, std::string detail) {
    r.lines.push_back(ObjectiveLine{std::move(name), failed, progress, exact, std::move(detail)});
  };
  // A unit to protect: 1 star per unit lost.
  auto protect = [&](const char* name, auto pred) {
    if (count_alive(before, pred) == 0) return;
    const int lost = died(turn, pred);
    line(name, lost, 0, true, num(lost) + " lost");
  };
  // A target to destroy (1 star): progress when it dies, failed if it is
  // still alive when the mission ends. Short of a kill, the hits it still
  // takes to die (HP, plus one for a shield and one for ice, which each
  // absorb a hit) count toward up to half a star, in proportion to the hits
  // it took this turn: Mission_Hacking's tower has 1 HP behind a shield set
  // once at the start, so stripping the shield is half the work, but no
  // turn that only strips it would otherwise gain anything (live
  // 2026-10-12, run 8: the tower was never touched and the star was lost).
  auto destroy = [&](const std::string& name, auto pred) {
    if (count_alive(before, pred) == 0) return;
    const int killed = died(turn, pred);
    const int left = count_alive(after, pred);
    int progress = killed > 0 ? kStar : 0;
    if (killed == 0) {
      auto hits = [](const Pawn& p) { return std::max(0, static_cast<int>(p.hp)) + (p.shield ? 1 : 0) + (p.frozen ? 1 : 0); };
      for (const Pawn& p : before.pawns()) {
        if (!alive(&p) || !pred(p)) continue;
        const Pawn* q = same_pawn(p, after, phase);
        if (!alive(q) || hits(p) <= 0) continue;
        progress = std::max(progress, (hits(p) - hits(*q)) * kStar / (2 * hits(p)));
      }
    }
    line(name, ends && left > 0 ? 1 : 0, progress, true, num(killed) + " destroyed, " + num(left) + " left");
  };

  // ---- Time pods: not an objective in the game's list, but a reward that is
  // lost when the pod breaks and secured when a player unit picks it up
  // (an intact pod is recovered at mission end). Weighted as 1 star.
  {
    int had = 0, broken = 0, taken = 0;
    for (int i = 0; i < kTileCount; ++i) {
      const Point pt = Point::from_index(i);
      if (before.tile(pt).pod != PodState::Present) continue;
      ++had;
      const PodState s = after.tile(pt).pod;
      taken += s == PodState::Collected ? 1 : 0;
      broken += s != PodState::Present && s != PodState::Collected ? 1 : 0;
    }
    if (had > 0) line("pod", broken, taken * kStar, true, num(taken) + " collected, " + num(broken) + " destroyed");
  }

  // ---- Unique buildings: the asset (BONUS_ASSET, Mission.AssetLoc) and
  // Mission_Critical's two buildings (Solar/Wind/Power/Factory), 1 star each,
  // failed when damaged (Board:IsDamaged).
  for (int i = 0; i < kTileCount; ++i) {
    const Point pt = Point::from_index(i);
    const Tile& a = before.tile(pt);
    if (a.unique_building == kNoSymbol || !a.is_building() || a.hp <= 0) continue;
    const Tile& b = after.tile(pt);
    const bool damaged = !b.is_building() || b.hp < a.hp;
    const std::string name(symbol_name(a.unique_building));
    const bool critical = name.starts_with("Mission_");
    line(critical ? name : std::string("BONUS_ASSET ") + name, damaged ? 1 : 0, 0, true,
         to_visual(pt) + (damaged ? " damaged" : " intact"));
  }

  // ---- Bonus objectives (missions.lua GetBonusStatus).
  const std::vector<BonusId> bonuses = active_bonuses(before, m);
  const int kills = r.enemy_kills;
  const bool kills_known = od.kills_done >= 0 && id != "Mission_AcidTank";
  const int done = kills_known ? od.kills_done : 0;
  for (BonusId b : bonuses) {
    const std::string name = bonus_name(b);
    switch (b) {
      case BonusId::Asset:
        break;  // the unique building above
      case BonusId::Kill: {
        // Board:GetEnemyCount() <= 0 at the end.
        const int left = count_alive(after, [](const Pawn& p) { return is_enemy(p) && !type_is(p, "BonusDebris"); });
        line(name, ends && left > 0 ? 1 : 0, 0, true, num(left) + " enemies left");
        break;
      }
      case BonusId::Grid: {
        // Mission:GetDamage() = PowerStart - power < 3. Power does not come back
        // during a mission, so crossing 3 is final.
        const bool known = od.power_start >= 0;
        const int start = known ? od.power_start : before.grid_power;
        const int d0 = std::max(0, start - before.grid_power), d1 = std::max(0, start - after.grid_power);
        line(name, d0 < 3 && d1 >= 3 ? 1 : 0, 0, known,
             "grid damage " + num(d0) + " -> " + num(d1) + (known ? "" : " (PowerStart not recorded: this turn only)"));
        break;
      }
      case BonusId::Mechs: {
        // Board:GetMechDamage() < 4 when the mission ends; repairs lower it,
        // so only the end counts (tier 2 already ranks the HP itself).
        const int d = mech_damage(after);
        line(name, ends && d >= 4 ? 1 : 0, 0, true, "mech damage " + num(d));
        break;
      }
      case BonusId::Block: {
        // Mission:GetSpawnCount() queues nothing on the final turn
        // (IsFinalTurn: turn == TurnLimit - 1), so the spawns emerging in the
        // enemy phase before it are the last ones to block.
        const bool known = od.blocked_spawns >= 0;
        const int so_far = known ? od.blocked_spawns : 0;
        const bool last = ends || before.turn >= before.total_turns - 1;
        line(name, last && known && so_far + r.spawns_blocked < 3 ? 1 : 0, paced(toward(so_far, r.spawns_blocked, 3), so_far, 3, before),
             known, num(r.spawns_blocked) + " blocked" + (known ? "" : " (BlockedSpawns not recorded)"));
        break;
      }
      case BonusId::KillFive: {
        const int target = od.kill_target > 0 ? od.kill_target : kill_bonus(difficulty);
        line(name, ends && done + kills < target ? 1 : 0, paced(toward(done, kills, target), done, target, before), kills_known,
             num(done) + " + " + num(kills) + " of " + num(target) + " kills");
        break;
      }
      case BonusId::Pacifist: {
        const int limit = od.kill_limit >= 0 ? od.kill_limit : pacifist_count(difficulty);
        line(name, done <= limit && done + kills > limit ? 1 : 0, 0, kills_known,
             num(done) + " + " + num(kills) + " kills, limit " + num(limit));
        break;
      }
      case BonusId::Debris: {
        auto sack = [](const Pawn& p) { return type_is(p, "BonusDebris"); };
        const int killed = died(turn, sack);
        const int left = count_alive(after, sack);
        line(name, ends && left > 0 ? 1 : 0, killed * kStar / 2, true,
             num(killed) + " destroyed, " + num(left) + " left");
        break;
      }
      case BonusId::SelfDamage: {
        // Board:GetInfectedCount() == 0 at the end; three mechs start infected.
        const int i0 = infected(before), i1 = infected(after);
        line(name, ends && i1 > 0 ? 1 : 0, (i0 - i1) * kStar / 3, true, "mites " + num(i0) + " -> " + num(i1));
        break;
      }
    }
  }

  // ---- The mission's own objectives.
  auto mission_unit = [](std::string_view type) { return [type](const Pawn& p) { return type_is(p, type); }; };
  if (id == "Mission_Tanks") {
    protect("Mission_Tanks", mission_unit("Archive_Tank"));
  } else if (id == "Mission_Civilians") {
    protect("Mission_Civilians", mission_unit("VIP_Truck"));
  } else if (id == "Mission_Bomb") {
    protect("Mission_Bomb", mission_unit("ProtoBomb"));
  } else if (id == "Mission_BotDefense") {
    protect("Mission_BotDefense", [](const Pawn& p) { return type_is(p, "Snowmine1") && p.team == Team::Player; });
  } else if (id == "Mission_Artillery") {
    protect("Mission_Artillery", mission_unit("ArchiveArtillery"));
  } else if (id == "Mission_Filler") {
    protect("Mission_Filler", mission_unit("Filler_Pawn"));
  } else if (id == "Mission_Volatile") {
    // TargetDied: dead without having retreated.
    protect("Mission_Volatile", mission_unit("GlowingScorpion"));
  } else if (id == "Mission_Final_Cave") {
    // "Defend the Renfield Bomb": no reputation, but a lost bomb is dropped
    // again and the fight runs longer. Weighted as 1 star.
    protect("Mission_Final_Cave", [](const Pawn& p) { return type_is(p, "BigBomb") && p.team == Team::Player; });
  } else if (id == "Mission_Train" || id == "Mission_Armored_Train") {
    // 2 stars: 1 when the train is stopped (the moving train dies and is
    // replaced by its wreck), 1 more when the wreck dies.
    const bool armored = id == "Mission_Armored_Train";
    auto moving = mission_unit(armored ? "Train_Armored" : "Train_Pawn");
    auto wreck = mission_unit(armored ? "Train_Armored_Damaged" : "Train_Damaged");
    const bool moving0 = count_alive(before, moving) > 0, wreck0 = count_alive(before, wreck) > 0;
    if (moving0 || wreck0) {
      const bool moving1 = count_alive(after, moving) > 0;
      int lost = moving0 && !moving1 ? 1 : 0;
      // The wreck dies: one alive at the start that died, or one created
      // during the turn that is dead at the end.
      bool wreck_lost = died(turn, wreck) > 0;
      for (const Pawn& q : after.pawns()) {
        wreck_lost = wreck_lost || (wreck(q) && !alive(&q) && is_new(q, turn));
      }
      lost += wreck_lost ? 1 : 0;
      line(id, lost, 0, true, moving0 ? (moving1 ? "train moving" : "train stopped") : "wreck");
    }
  } else if (id == "Mission_Satellite") {
    int lost = 0, launched = 0, had = 0;
    for (const Pawn& p : before.pawns()) {
      if (!alive(&p) || !type_is(p, "SatelliteRocket")) continue;
      ++had;
      const Fate f = fate(p, turn);
      lost += f == Fate::Died ? 1 : 0;
      launched += f == Fate::Left ? 1 : 0;
    }
    if (had) line(id, lost, launched * kStar, true, num(launched) + " launched, " + num(lost) + " destroyed");
  } else if (id == "Mission_Disposal") {
    protect("Mission_Disposal unit", mission_unit("Disposal_Unit"));
    const int m0 = mountains(before), m1 = mountains(after);
    if (m0 > 0) {
      line("Mission_Disposal mountains", ends && m1 > 0 ? 1 : 0, (m0 - m1) * kStar / m0, true,
           "mountains " + num(m0) + " -> " + num(m1));
    }
  } else if (id == "Mission_Terraform") {
    protect("Mission_Terraform unit", mission_unit("Terraformer"));
    if (od.grass_known) {
      // A tile is grass while it shows the grass sprite (UpdateMission:
      // GetCustomTile == "ground_grass.png"); the Terraformer's script clears
      // it. Terrain is no guide: its sand turns to ground when hit (run 8
      // replay: two terraformed tiles hit later still counted as grass, so the
      // last shot that would finish the grass scored nothing). Recordings
      // without custom tiles fall back to sand.
      const bool sprites = grass_sprites(before, od.grass);
      int left = 0, turned = 0;
      for (Point g : od.grass) {
        const bool g0 = is_grass(before, g, sprites), g1 = is_grass(after, g, sprites);
        turned += g0 && !g1 ? 1 : 0;
        left += g1 ? 1 : 0;
      }
      // Only the Terraformer terraforms, so the turn it dies with grass left
      // fails this star too (GetCompletedObjectives: both fail).
      const bool orphaned = count_alive(before, mission_unit("Terraformer")) > 0 && died(turn, mission_unit("Terraformer")) > 0;
      const int total = static_cast<int>(od.grass.size());
      line("Mission_Terraform grass", (ends || orphaned) && left > 0 ? 1 : 0, total ? turned * kStar / total : 0, sprites,
           num(left) + " grass tiles left" + (sprites ? "" : " (no sprites recorded: approximated by sand)"));
    } else {
      line("Mission_Terraform grass", 0, 0, false, "grass tiles not recorded");
    }
  } else if (id == "Mission_Force") {
    const bool known = od.mountains_done >= 0;
    const int so_far = known ? od.mountains_done : 0;
    const int target = od.mountain_target > 0 ? od.mountain_target : 2;
    line(id, ends && so_far + r.mountains_destroyed < target ? 1 : 0, paced(toward(so_far, r.mountains_destroyed, target), so_far, target, before),
         known, num(so_far) + " + " + num(r.mountains_destroyed) + " of " + num(target) + " mountains");
  } else if (id == "Mission_AcidTank") {
    // 2 stars: the first acid kill earns 1, the fourth the second.
    const bool known = od.kills_done >= 0;
    const int so_far = known ? od.kills_done : 0, total = so_far + r.acid_kills;
    auto units = [](int c) { return c <= 0 ? 0 : kStar + (std::min(c, 4) - 1) * kStar / 3; };
    const int stars = total >= 4 ? 2 : total > 0 ? 1 : 0;
    line(id, ends ? 2 - stars : 0, units(total) - units(so_far), known,
         num(so_far) + " + " + num(r.acid_kills) + " acid kills of 4");
  } else if (id == "Mission_Barrels") {
    auto vat = [](const Pawn& p) { return type_name(p).starts_with("AcidVat"); };
    const int killed = died(turn, vat), left = count_alive(after, vat);
    if (killed || left) line(id, ends ? left : 0, killed * kStar, true, num(killed) + " destroyed, " + num(left) + " left");
  } else if (id == "Mission_BoomBots") {
    // 4 bots placed at the start; 2 destroyed earn 1 star, 4 earn 2.
    auto boom = [](const Pawn& p) { return type_name(p).ends_with("_Boom"); };
    const int alive0 = count_alive(before, boom);
    const int d0 = std::max(0, 4 - alive0), d1 = d0 + died(turn, boom);
    const int stars = d1 >= 4 ? 2 : d1 >= 2 ? 1 : 0;
    line(id, ends ? 2 - stars : 0, toward(d0, d1 - d0, 4, 2), alive0 <= 4,
         num(d0) + " -> " + num(d1) + " of 4 destroyed");
  } else if (id == "Mission_ForestFire") {
    // Fires on the board at the end: 8+ earn 2 stars, 4+ earn 1.
    const int f0 = fires(before), f1 = fires(after);
    const int stars = f1 >= 8 ? 2 : f1 >= 4 ? 1 : 0;
    line(id, ends ? 2 - stars : 0, (std::min(f1, 8) - std::min(f0, 8)) * kStar / 4, true,
         "fires " + num(f0) + " -> " + num(f1));
  } else if (id == "Mission_Repair") {
    const bool known = od.repairs_done >= 0;
    const int so_far = known ? od.repairs_done : 0;
    const int target = od.repair_target > 0 ? od.repair_target : 3;
    line(id, ends && so_far + r.repairs_used < target ? 1 : 0, paced(toward(so_far, r.repairs_used, target), so_far, target, before), known,
         num(so_far) + " + " + num(r.repairs_used) + " of " + num(target) + " platforms");
  } else if (id == "Mission_FreezeBldg") {
    const int target = od.freeze_target > 0 ? od.freeze_target : 5;
    std::vector<Point> tiles = od.freeze_buildings;
    const bool known = !tiles.empty();
    if (!known) {
      // Mission start froze every building; buildings destroyed earlier count
      // as thawed but are not known here.
      for (int i = 0; i < kTileCount; ++i) {
        if (before.tile(Point::from_index(i)).is_building()) tiles.push_back(Point::from_index(i));
      }
    }
    auto thawed = [&](const Board& b) {
      int n = 0;
      for (Point t : tiles) n += b.tile(t).is_building() && b.tile(t).frozen ? 0 : 1;
      return n;
    };
    const int t0 = thawed(before), t1 = thawed(after);
    line(id, ends && t1 < target ? 1 : 0, toward(t0, t1 - t0, target), known,
         num(t0) + " -> " + num(t1) + " of " + num(target) + " thawed");
  } else if (id == "Mission_FreezeBots") {
    // 2 stars: each bot frozen (and alive) at the end.
    auto bot = [](const Pawn& p) {
      const std::string_view t = type_name(p);
      return p.team == Team::Enemy && (t.starts_with("Snowtank") || t.starts_with("Snowlaser") || t.starts_with("Snowart"));
    };
    if (count_alive(before, bot) > 0) {
      const int lost = died(turn, bot);
      const int fz0 = count_alive(before, [&](const Pawn& p) { return bot(p) && p.frozen; });
      const int fz1 = count_alive(after, [&](const Pawn& p) { return bot(p) && p.frozen; });
      const int loose = count_alive(after, [&](const Pawn& p) { return bot(p) && !p.frozen; });
      line(id, lost + (ends ? loose : 0), (fz1 - fz0) * kStar, true,
           num(lost) + " destroyed, frozen " + num(fz0) + " -> " + num(fz1));
    }
  } else if (id == "Mission_Hacking") {
    int32_t bot = m.hacking_bot;
    if (bot < 0) {
      for (const Pawn& p : before.pawns()) {
        if (!alive(&p)) continue;
        if (type_is(p, "Snowtank1_Player") || (type_is(p, "Snowtank1") && (bot < 0 || p.shield))) bot = p.uid;
      }
    }
    if (const Pawn* b = before.find_pawn(bot); alive(b)) {
      const bool lost = fate(*b, turn) == Fate::Died;
      line("Mission_Hacking bot", lost ? 1 : 0, 0, m.hacking_bot >= 0, lost ? "bot destroyed" : "bot alive");
    }
    destroy("Mission_Hacking tower", mission_unit("Hacked_Building"));
  } else if (id == "Mission_Shields") {
    destroy(id, mission_unit("Shield_Building"));
  } else if (id == "Mission_AcidStorm") {
    destroy(id, mission_unit("Storm_Generator"));
  } else if (id == "Mission_Dam") {
    destroy(id, mission_unit("Dam_Pawn"));
  } else if (id == "Mission_Missiles") {
    line(id, 0, 0, false, "Missile_Unit shots used not modelled");
  } else if (id == "Mission_BlobBoss") {
    // Five dead blobs (1 large -> 2 medium -> 4 small); the dead so far are
    // estimated from the living ones assuming every split spawned both.
    auto blob = [](const Pawn& p) { return type_name(p).starts_with("BlobBoss"); };
    auto dead_est = [](const Board& b) {
      int big = 0, med = 0, small = 0;
      for (const Pawn& p : b.pawns()) {
        if (!alive(&p)) continue;
        const std::string_view t = type_name(p);
        big += t == "BlobBoss";
        med += t == "BlobBossMed";
        small += t == "BlobBossSmall";
      }
      if (big) return 0;
      const int med_dead = std::max(0, 2 - med);
      return 1 + med_dead + std::max(0, 2 * med_dead - small);
    };
    if (count_alive(before, blob) > 0) {
      const int d0 = dead_est(before), d1 = dead_est(after);
      line(id, ends && d1 < 5 ? 1 : 0, toward(d0, d1 - d0, 5), false, num(d0) + " -> " + num(d1) + " of 5 blobs (estimated)");
    }
  } else if (id.starts_with("Mission_") && id.ends_with("Boss")) {
    // Mission_Boss: destroy the BossPawn ("Mission_XBoss" -> "XBoss";
    // BotBoss2 in later sectors; the Psion boss is "Jelly_Boss").
    const std::string boss = id == "Mission_JellyBoss" ? "Jelly_Boss" : id.substr(8);
    destroy(id, [boss](const Pawn& p) { return type_name(p).starts_with(boss); });
  }
  return r;
}

// Mirrors evaluate_objectives line by line: each line's progress is bounded
// by what `before` allows (a count only of pawns alive at the start, a
// counter clamped at its target), never by what the turn does.
int objective_progress_cap(const Board& before, const TurnContext* ctx) {
  static const MissionData kNoMission;
  const MissionData& m = ctx ? ctx->mission : kNoMission;
  const ObjectiveData& od = m.objectives;
  const std::string& id = m.mission_id;
  constexpr int kMany = 1000;  // "any number": toward() clamps at the target
  int cap = 0;
  auto alive_count = [&](auto pred) { return count_alive(before, pred); };
  auto destroy = [&](auto pred) { cap += alive_count(pred) > 0 ? kStar : 0; };
  auto mission_unit = [](std::string_view type) { return [type](const Pawn& p) { return type_is(p, type); }; };
  // Team-6 non-mech pawns alive at the start: an upper bound on this turn's
  // EVENT_ENEMY_KILLED count as evaluate_objectives takes it.
  const int enemies = alive_count([](const Pawn& p) { return p.team == Team::Enemy && !p.mech; });

  int pods = 0;
  for (int i = 0; i < kTileCount; ++i) pods += before.tile(Point::from_index(i)).pod == PodState::Present ? 1 : 0;
  cap += pods * kStar;

  const bool kills_known = od.kills_done >= 0 && id != "Mission_AcidTank";
  const int done = kills_known ? od.kills_done : 0;
  for (BonusId b : active_bonuses(before, m)) {
    switch (b) {
      case BonusId::Block:
        cap += paced(toward(od.blocked_spawns >= 0 ? od.blocked_spawns : 0, kMany, 3), od.blocked_spawns, 3, before);
        break;
      case BonusId::KillFive:
        cap += paced(toward(done, enemies, od.kill_target > 0 ? od.kill_target : kill_bonus(m.difficulty)), done,
                     od.kill_target > 0 ? od.kill_target : kill_bonus(m.difficulty), before);
        break;
      case BonusId::Debris:
        cap += alive_count([](const Pawn& p) { return type_is(p, "BonusDebris"); }) * kStar / 2;
        break;
      case BonusId::SelfDamage:
        cap += infected(before) * kStar / 3;
        break;
      default:
        break;  // no progress
    }
  }

  if (id == "Mission_Satellite") {
    cap += alive_count(mission_unit("SatelliteRocket")) * kStar;
  } else if (id == "Mission_Disposal") {
    cap += mountains(before) > 0 ? kStar : 0;
  } else if (id == "Mission_Terraform") {
    if (od.grass_known && !od.grass.empty()) {
      const bool sprites = grass_sprites(before, od.grass);
      int open = 0;
      for (Point g : od.grass) open += is_grass(before, g, sprites) ? 1 : 0;
      cap += open * kStar / static_cast<int>(od.grass.size());
    }
  } else if (id == "Mission_Force") {
    cap += paced(toward(od.mountains_done >= 0 ? od.mountains_done : 0, mountains(before),
                        od.mountain_target > 0 ? od.mountain_target : 2),
                 od.mountains_done, od.mountain_target > 0 ? od.mountain_target : 2, before);
  } else if (id == "Mission_AcidTank") {
    // Acid kills include pawns created during the turn (a split blob, a
    // spawn), so the enemies alive now do not bound them.
    const int so_far = od.kills_done >= 0 ? od.kills_done : 0;
    auto units = [](int c) { return c <= 0 ? 0 : kStar + (std::min(c, 4) - 1) * kStar / 3; };
    cap += units(so_far + kMany) - units(so_far);
  } else if (id == "Mission_Barrels") {
    cap += alive_count([](const Pawn& p) { return type_name(p).starts_with("AcidVat"); }) * kStar;
  } else if (id == "Mission_BoomBots") {
    const int alive0 = alive_count([](const Pawn& p) { return type_name(p).ends_with("_Boom"); });
    cap += toward(std::max(0, 4 - alive0), alive0, 4, 2);
  } else if (id == "Mission_ForestFire") {
    cap += (8 - std::min(fires(before), 8)) * kStar / 4;
  } else if (id == "Mission_Repair") {
    int items = 0;
    for (int i = 0; i < kTileCount; ++i) {
      const Tile& t = before.tile(Point::from_index(i));
      items += t.item != kNoSymbol && symbol_name(t.item) == "Item_Repair_Mine" ? 1 : 0;
    }
    cap += paced(toward(od.repairs_done >= 0 ? od.repairs_done : 0, items, od.repair_target > 0 ? od.repair_target : 3),
                 od.repairs_done, od.repair_target > 0 ? od.repair_target : 3, before);
  } else if (id == "Mission_FreezeBldg") {
    const int target = od.freeze_target > 0 ? od.freeze_target : 5;
    std::vector<Point> tiles = od.freeze_buildings;
    if (tiles.empty()) {
      for (int i = 0; i < kTileCount; ++i) {
        if (before.tile(Point::from_index(i)).is_building()) tiles.push_back(Point::from_index(i));
      }
    }
    int t0 = 0;
    for (Point t : tiles) t0 += before.tile(t).is_building() && before.tile(t).frozen ? 0 : 1;
    cap += toward(t0, static_cast<int>(tiles.size()) - t0, target);
  } else if (id == "Mission_FreezeBots") {
    // Each bot frozen at the end: the bots alive now (or emerging) not frozen yet.
    auto bot = [](const Pawn& p) {
      const std::string_view t = type_name(p);
      return p.team == Team::Enemy && (t.starts_with("Snowtank") || t.starts_with("Snowlaser") || t.starts_with("Snowart"));
    };
    if (alive_count(bot) > 0) {
      const int loose = alive_count([&](const Pawn& p) { return bot(p) && !p.frozen; });
      cap += (loose + static_cast<int>(before.spawn_points.size())) * kStar;
    }
  } else if (id == "Mission_Hacking") {
    destroy(mission_unit("Hacked_Building"));
  } else if (id == "Mission_Shields") {
    destroy(mission_unit("Shield_Building"));
  } else if (id == "Mission_AcidStorm") {
    destroy(mission_unit("Storm_Generator"));
  } else if (id == "Mission_Dam") {
    destroy(mission_unit("Dam_Pawn"));
  } else if (id == "Mission_BlobBoss") {
    if (alive_count([](const Pawn& p) { return type_name(p).starts_with("BlobBoss"); }) > 0) cap += kStar;
  } else if (id.starts_with("Mission_") && id.ends_with("Boss")) {
    const std::string boss = id == "Mission_JellyBoss" ? "Jelly_Boss" : id.substr(8);
    destroy([boss](const Pawn& p) { return type_name(p).starts_with(boss); });
  }
  return cap;
}

int PositionTerms::total() const {
  return -(3 * mech_fire + 2 * mech_acid + 2 * mech_frozen + mech_smoke + 2 * mech_fragile) -
         (building_threat + unit_threat) + enemy_fire + enemy_frozen + 2 * deployed + deployed_hp;
}

bool is_deployed_unit(const Pawn& p) {
  if (p.team != Team::Player || p.mech || p.neutral) return false;
  const std::string_view t = type_name(p);
  return t.starts_with("Deploy_") || t.starts_with("DeployUnit_Aracnoid");
}

PositionTerms position_terms(const Board& b) {
  PositionTerms t;
  const bool flame_immune = b.has_passive(kPassiveFlameImmune);
  for (const Pawn& p : b.pawns()) {
    if (!alive(&p) || !p.pos.valid()) continue;
    if (p.mech && p.team == Team::Player) {
      const bool fire_immune = p.ignore_fire || flame_immune || p.has_pilot(kPilotThick);
      t.mech_fire += p.fire && !fire_immune ? 1 : 0;
      t.mech_acid += p.acid ? 1 : 0;
      t.mech_frozen += p.frozen ? 1 : 0;
      // Smoke blocks a mech's weapons only when Pawn::IsSmoked holds: not for
      // IgnoreSmoke or a Disable_Immunity pilot (Camila Vera).
      t.mech_smoke += b.tile(p.pos).smoke && !p.ignore_smoke && !p.has_pilot(kPilotDisableImmunity) ? 1 : 0;
      t.mech_fragile += p.hp == 1 ? 1 : 0;
      continue;
    }
    if (p.team == Team::Player && is_deployed_unit(p)) {
      ++t.deployed;
      t.deployed_hp += p.hp;
      continue;
    }
    if (p.team != Team::Enemy || p.neutral) continue;
    t.enemy_fire += p.fire && !p.ignore_fire ? 1 : 0;
    t.enemy_frozen += p.frozen ? 1 : 0;
    for (Point v : kDirVectors) {
      const Point n = p.pos + v;
      if (!n.valid()) continue;
      const Tile& nt = b.tile(n);
      if (nt.is_building() && nt.hp > 0) t.building_threat += nt.unique_building != kNoSymbol ? 2 : 1;
      for (const Pawn& q : b.pawns()) {
        if (q.occupies(n) && alive(&q) && q.team == Team::Player && !q.mech) ++t.unit_threat;
      }
    }
  }
  return t;
}

}  // namespace itb
