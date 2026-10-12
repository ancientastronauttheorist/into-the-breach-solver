// Stage 8: mission objectives (score tier 3) and the position metric (tier 5).
//
// The game judges objectives in Lua (missions.lua Mission:GetBonusStatus,
// each mission's GetCompletedObjectives) at the end of the mission, plus a
// few native counters (Game:GetEventCount: EVENT_ENEMY_KILLED,
// EVENT_SPAWNBLOCKED, EVENT_ACID_DESTROYED, EVENT_MOUNTAIN_DESTROYED,
// EVENT_REPAIR_PICKUP). A turn is scored on one concrete outcome, from the
// board at the start of the player's turn to the board after the enemy
// phase, in two quantities:
//
//   ObjectivesFailed  stars lost for good during this turn. Stars are the
//                     game's own reward units: each objective's Objective(...,
//                     value) reputation, power or asset reward (a bonus
//                     objective is 1; "protect both tanks" is 2, 1 per tank).
//                     Latched failures count when they happen (a protected
//                     unit dies, an objective building is damaged, the kill
//                     limit is exceeded); objectives decided at mission end
//                     count on the turn the mission ends (phase->mission_ended).
//   ObjectiveProgress progress towards stars not yet earned, in kStar units
//                     per star (840 = lcm(1..8), so "1 of 5 kills" is exact):
//                     kills toward a kill target, blocked spawns, destroyed
//                     targets, launched rockets, thawed buildings, collected
//                     pods, ... It can be negative (fires put out, bots
//                     thawed).
//
// A pawn "dies" when it was alive at the start and is dead or gone at the end,
// except pawns that left without dying: a retreat (the minor flag set, HP 0,
// tile_rules retreat), a satellite rocket that launched (removed, not dead) and
// the Mission_Hacking bot replaced by its player-team copy.
//
// Each line says whether recorded data pins it down (`exact`); see the
// README's objective table.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "itb/board.hpp"

namespace itb {

struct TurnContext;
struct PhaseResult;
struct MissionData;

// missions.lua BONUS_* (Mission.BonusObjs entries).
enum class BonusId : int {
  Asset = 1,       // protect the unique building (AssetLoc)
  Kill = 2,        // kill every enemy before they retreat
  Grid = 3,        // take less than 3 grid damage
  Mechs = 4,       // end with less than 4 mech damage
  Block = 5,       // block Vek spawning 3 times
  KillFive = 6,    // kill at least GetKillBonus() enemies (5 easy, 7 otherwise)
  Debris = 7,      // destroy the 2 Vek egg sacks (BonusDebris)
  SelfDamage = 8,  // knock the mites off the mechs (infected)
  Pacifist = 9,    // kill GetPacifistCount() or fewer enemies
};

inline constexpr int kStar = 840;  // ObjectiveProgress units per star

// The bonus objectives the recorded data says are active: the bridge's list
// when exported, else what the other fields reveal (a kill target, a kill
// limit, an asset building, egg sacks, infected mechs). Grid, Mechs and Block
// cannot be inferred without the list.
std::vector<BonusId> active_bonuses(const Board& board, const MissionData& mission);

struct ObjectiveLine {
  std::string id;      // "BONUS_KILL_FIVE", "Mission_Tanks", "pod", ...
  int failed = 0;      // stars lost this turn (>= 0)
  int progress = 0;    // kStar units gained this turn (may be < 0)
  bool exact = true;   // false: some input was not recorded (detail says which)
  std::string detail;
};

struct ObjectiveReport {
  std::vector<ObjectiveLine> lines;
  bool mission_ends = false;  // the mission ends after this enemy phase
  // Native event counts for this turn, as the game would raise them.
  int enemy_kills = 0;     // EVENT_ENEMY_KILLED: team-6 non-minor deaths, any killer
  int acid_kills = 0;      // EVENT_ACID_DESTROYED: team-6 non-mech deaths with ACID as they die (Board::acid_destroyed)
  int spawns_blocked = 0;  // EVENT_SPAWNBLOCKED (PhaseResult SpawnBlocked events)
  int mountains_destroyed = 0;  // mountains that became rubble
  int repairs_used = 0;    // repair platforms used by player-team pawns

  int failed() const;
  int progress() const;
  bool exact() const;
};

// Objectives from `before` (start of the player's turn) to `after` (end of the
// enemy phase). `ctx` supplies the mission; `phase` the enemy phase's events
// (blocked spawns, mission end). Without `phase` the mission is taken to end
// when before.turn >= before.total_turns.
ObjectiveReport evaluate_objectives(const Board& before, const Board& after, const TurnContext* ctx,
                                    const PhaseResult* phase);

// An upper bound on ObjectiveReport::progress() for any turn starting from
// `before` (any actions, any enemy phase): per objective line, the most its
// progress can reach given what is on `before` (targets alive, pods, counters
// so far). Used as the solver's tier cap when objective progress ranks above
// the threat tier (SolveOptions::objectives_first).
int objective_progress_cap(const Board& before, const TurnContext* ctx);

// The pawn `p` (from the start of the turn) on `after`, or null if it is gone.
// Uids of removed pawns can be handed out again within a turn (new pawns take
// one more than the largest uid still on the board), so a pawn with the same
// uid only counts if it has the same type and did not emerge from a spawn
// during `phase`.
const Pawn* same_pawn(const Pawn& p, const Board& after, const PhaseResult* phase);

// Tier 5: the end-of-turn position, a tie-break between outcomes equal in
// every other tier. Computed on `after` only, one pass over the pawns:
//   -3 per mech on fire (not fire-immune): 1 damage next turn unless spent on a repair
//   -2 per mech with ACID: doubles the next weapon hits
//   -2 per frozen mech: cannot act next turn
//   -1 per mech on a smoke tile: cannot attack or repair from there
//   -2 per mech at 1 HP: one hit from being destroyed
//   -1 per (enemy, adjacent building) pair, -2 for an objective building,
//      -1 per enemy next to a player-team non-mech unit: what the next
//      enemy turn can reach without moving
//   +1 per enemy on fire (not immune) or frozen: damage or a lost attack
// Enemies are team-6 pawns that are not neutral (as in tier 4).
struct PositionTerms {
  int mech_fire = 0, mech_acid = 0, mech_frozen = 0, mech_smoke = 0, mech_fragile = 0;
  int building_threat = 0, unit_threat = 0;
  int enemy_fire = 0, enemy_frozen = 0;
  int total() const;
};

PositionTerms position_terms(const Board& after);

}  // namespace itb
