// Turn scoring: strict priority tiers, compared lexicographically.
//
// A Score summarizes how a turn went, comparing the board before the player's
// actions with the board after the enemy phase. Higher is better in every
// tier and an earlier tier always dominates a later one, so "optimal" has one
// exact meaning. Tiers (decided with the project owner):
//
//   1. grid and buildings: [the odds the grid survives the turn, only with
//                          ScoreOptions::grid_survival], grid power lost,
//                          then building HP lost
//   2. mech survival:      mechs destroyed, then objective stars failed
//                          (objectives.hpp), then [next-turn loss, only with
//                          the lookahead (lookahead.hpp)], then [next-turn
//                          threat, only with ScoreOptions::threat_tier], then
//                          mech HP lost
//   3. objective progress  (objectives.hpp)
//   4. kills and damage:   Vek killed, then Vek HP removed
//   5. position:           end-of-turn position quality (objectives.hpp)
//
// Losses are stored negated so that every component is "larger is better".
// HP components are net changes: a repair offsets damage, a regenerating
// psion offsets damage dealt (README "Turn score").
#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <string>

#include "itb/board.hpp"

namespace itb {

struct TurnContext;
struct PhaseResult;

enum class ScoreKey : int {
  // The probability that the grid is still above 0 at the end of the turn,
  // in kSurvivalOne units, over the Grid Defense rolls (and the game's
  // random picks) of the turn. score_turn sets one outcome's value
  // (kSurvivalOne if the grid survives it, else 0); the solver folds the
  // outcomes into the probability (solver.cpp, README "Grid survival").
  // Only with ScoreOptions::grid_survival; off it is 0 everywhere and left
  // out of printed scores. It is never a tier of its own in tier counts: it
  // refines the grid tier (tier_used).
  GridSurvival = 0,
  GridLost,          // -(grid power lost)
  BuildingHpLost,    // -(building HP lost, populated or not)
  MechsLost,         // -(mechs destroyed) (+1 for a revived corpse)
  ObjectivesFailed,  // -(objective stars lost this turn)
  // The next turn's forced loss as the lookahead estimates it
  // (lookahead.hpp next_turn_value: grid, then mechs, then building HP).
  // Never set by score_turn (it needs the engine): the solver fills it in
  // with SolveOptions::lookahead. Off: 0 and left out like Threat.
  NextTurn,
  // -(enemies able to attack a building next turn, threat.hpp). Always 0
  // unless ScoreOptions::threat_tier is set; when it is not, the tier is left
  // out of every printed score and tier count (score_tiers).
  Threat,
  MechHpLost,        // -(mech HP lost, net of repairs)
  ObjectiveProgress, // + objective progress, kStar units per star
  VekKilled,         // + enemies killed (Vek and enemy bots, not neutral props)
  VekHpRemoved,      // + enemy HP removed (net of regeneration)
  Position,          // + position quality (objectives.hpp position_terms)
  Count,
};

inline constexpr int kScoreKeys = static_cast<int>(ScoreKey::Count);

// ScoreKey::GridSurvival's unit: probability 1 (the grid surely survives).
inline constexpr int32_t kSurvivalOne = 1000000000;

// Runtime choices of score_turn.
struct ScoreOptions {
  // Score ScoreKey::Threat (README "Turn score", threat.hpp): minus the
  // building threats, minus kUnitThreatWeight per unit threat. Off: it is 0.
  bool threat_tier = false;
  // Experiment: the Threat slot holds the kill count instead (VekKilled
  // ranked above mech HP), for comparison with the threat metric. Counts as
  // the threat tier being in use. With threat_tier too, the slot holds
  // kills - 100 * threats (threats first, then kills).
  bool threat_kills = false;
  // With the tier in use: objective progress ranks above the threat (and
  // kills): the slot becomes kObjectivesFirstWeight * ObjectiveProgress plus
  // the above, so a bonus-objective step is never traded for a threat.
  // ObjectiveProgress keeps its own later component (then redundant).
  bool objectives_first = false;
  bool tier_on() const { return threat_tier || threat_kills; }
  // Experiment (README "Turn score", mech exchange): a mech lost is worth
  // more than `mech_grid` grid power and less than mech_grid + 1, so a plan
  // may give up to that much grid to keep a mech. The GridLost slot becomes
  // 2 * grid + (2 * rate + 1) * mechs (mech_grid_slot), minus
  // kGridOutPenalty when the grid ends at 0 (the run is lost: never traded).
  // MechsLost keeps its own later component (then redundant). -1 = off.
  int mech_grid = -1;
  // With mech_grid: the rate is min(mech_grid, mission turns left after this
  // one), so on a mission's last turn a mech only outranks building HP.
  bool mech_grid_turns = false;
  bool mech_exchange() const { return mech_grid >= 0; }
  // With mech_grid: a mech's rate is at least min(mech_grid, its pilot's
  // level) (pilot_level 0-2), turns left or not. A pilot dies with its mech
  // for the rest of the run (the mech comes back pilotless next mission), so
  // on a mission's last turns a levelled pilot is worth more than the turns
  // left say. Rates then differ per mech (mech_rate); see mech_grid_slot_w.
  bool mech_grid_pilot = false;
  // The NextTurn tier is in use (the solver fills it in; score_turn leaves
  // it at 0). Only tier counts and printing read this.
  bool next_turn = false;
  // Score ScoreKey::GridSurvival (README "Grid survival"): kSurvivalOne when
  // the grid ends the turn above 0, else 0. Off: 0.
  bool grid_survival = false;
};

// Optional tiers in use, as a bit set: Threat (threat_tier or threat_kills),
// NextTurn (the lookahead) and GridSurvival (grid_survival, printed only:
// it is part of the grid tier in tier counts). Every other tier is always
// in use.
using TierSet = unsigned;
inline constexpr TierSet kTierThreat = 1u;
inline constexpr TierSet kTierNextTurn = 2u;
inline constexpr TierSet kTierSurvival = 4u;
inline TierSet tier_set(const ScoreOptions& o) {
  return (o.tier_on() ? kTierThreat : 0u) | (o.next_turn ? kTierNextTurn : 0u) |
         (o.grid_survival ? kTierSurvival : 0u);
}

// Larger than any spread of the threat / kills part of the slot (threats and
// kills are each below 100 on an 8x8 board, so that part lies in
// (-100 * 100, 100)), so whole objective-progress units dominate it.
inline constexpr int kObjectivesFirstWeight = 16384;

// With the threat tier, each enemy that could attack a protected mission
// unit (threat.hpp count_unit_threats) takes this off the slot, counted up to
// kMaxUnitThreats: above whole objective-progress units (up to 4096 of them
// at kObjectivesFirstWeight) and the building threats, within int32.
inline constexpr int kUnitThreatWeight = 1 << 26;
inline constexpr int kMaxUnitThreats = 15;

// ScoreOptions::mech_grid: taken from the GridLost slot of a plan that ends
// with no grid power (larger than any spread of the rest of the slot).
inline constexpr int kGridOutPenalty = 1 << 20;
// The exchange rate on this board (the board at the start of the turn): -1
// when off, else mech_grid, capped by the turns left with mech_grid_turns.
int mech_grid_rate(const Board& before, const ScoreOptions& options);
// The rate of one mech on the board at the start of the turn: the board's
// rate, raised to min(mech_grid, pilot level) with mech_grid_pilot. -1 when
// the exchange is off.
int mech_rate(const Board& before, const Pawn& mech, const ScoreOptions& options);
// The GridLost slot with the exchange at `rate` (>= 0): grid and mechs are
// net changes (ScoreKey::GridLost, MechsLost), grid_after the grid power at
// the end of the turn.
inline int32_t mech_grid_slot(int rate, int grid, int mechs, int grid_after) {
  return 2 * grid + (2 * rate + 1) * mechs - (grid_after <= 0 ? kGridOutPenalty : 0);
}
// The same with per-mech rates: `weighted_mechs` is the sum over the mechs
// at the start of (2 * rate + 1) * (alive after - alive before). Equal to
// mech_grid_slot when every mech has the same rate.
inline int32_t mech_grid_slot_w(int grid, int weighted_mechs, int grid_after) {
  return 2 * grid + weighted_mechs - (grid_after <= 0 ? kGridOutPenalty : 0);
}

// The tiers in use, in rank order, as ScoreKey indices: every key, minus
// Threat when the threat tier is off and NextTurn when the lookahead is off
// (a bool argument means the threat tier alone, as before NextTurn). Tier
// counts (SolveOptions::min_proven_tiers, SolveResult::proven_components)
// count these. GridSurvival is never counted: with grid_survival it sits in
// front of GridLost as one tier with it ("grid"), so tier counts mean the
// same with it on or off (components_for_tiers includes it).
inline bool tier_used(int key, TierSet tiers) {
  if (key == static_cast<int>(ScoreKey::GridSurvival)) return false;
  if (key == static_cast<int>(ScoreKey::Threat)) return (tiers & kTierThreat) != 0;
  if (key == static_cast<int>(ScoreKey::NextTurn)) return (tiers & kTierNextTurn) != 0;
  return true;
}
// The components printed and listed in score vectors: the tiers in use, and
// GridSurvival with grid_survival.
inline bool tier_shown(int key, TierSet tiers) {
  if (key == static_cast<int>(ScoreKey::GridSurvival)) return (tiers & kTierSurvival) != 0;
  return tier_used(key, tiers);
}
inline int used_tier_count(TierSet tiers) {
  return kScoreKeys - 1 - ((tiers & kTierThreat) ? 0 : 1) - ((tiers & kTierNextTurn) ? 0 : 1);
}
// The number of leading ScoreKey components that hold the first `tiers`
// tiers in use (an unused optional tier, constant, is only counted when it
// lies before a used tier).
int components_for_tiers(int tiers, TierSet used);
// Short tier names ("survival", "grid", "bldg_hp", ...), by ScoreKey index.
const char* score_key_name(int key);

struct Score {
  std::array<int32_t, kScoreKeys> v{};

  int32_t& operator[](ScoreKey k) { return v[static_cast<int>(k)]; }
  int32_t operator[](ScoreKey k) const { return v[static_cast<int>(k)]; }

  // Lexicographic: the first differing component decides.
  auto operator<=>(const Score&) const = default;
  bool operator==(const Score&) const = default;

  // "grid=0 bldg_hp=-1 ...": the tiers shown (tier_shown); GridSurvival as
  // a probability ("survival=0.2500").
  std::string describe(TierSet tiers = 0) const;
};

// Scores the turn from `before` (start of the player's turn) to `after` (end
// of the enemy phase). `ctx` gives the mission (objectives) and `phase` the
// enemy phase's events (blocked spawns, spawned uids, mission end); either
// may be null.
Score score_turn(const Board& before, const Board& after, const TurnContext* ctx = nullptr,
                 const PhaseResult* phase = nullptr, const ScoreOptions& options = {});

}  // namespace itb
