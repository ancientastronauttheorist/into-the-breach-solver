// Turn scoring: strict priority tiers, compared lexicographically.
//
// A Score summarizes how a turn went, comparing the board before the player's
// actions with the board after the enemy phase. Higher is better in every
// tier and an earlier tier always dominates a later one, so "optimal" has one
// exact meaning. Tiers (decided with the project owner):
//
//   1. grid and buildings: grid power lost, then building HP lost
//   2. mech survival:      mechs destroyed, then objective stars failed
//                          (objectives.hpp), then [next-turn threat, only
//                          with ScoreOptions::threat_tier], then mech HP lost
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
  GridLost = 0,      // -(grid power lost)
  BuildingHpLost,    // -(building HP lost, populated or not)
  MechsLost,         // -(mechs destroyed) (+1 for a revived corpse)
  ObjectivesFailed,  // -(objective stars lost this turn)
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

// Runtime choices of score_turn.
struct ScoreOptions {
  // Score ScoreKey::Threat (README "Turn score", threat.hpp). Off: it is 0.
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
};

// Larger than any spread of the threat / kills part of the slot (threats and
// kills are each below 100 on an 8x8 board, so that part lies in
// (-100 * 100, 100)), so whole objective-progress units dominate it.
inline constexpr int kObjectivesFirstWeight = 16384;

// ScoreOptions::mech_grid: taken from the GridLost slot of a plan that ends
// with no grid power (larger than any spread of the rest of the slot).
inline constexpr int kGridOutPenalty = 1 << 20;
// The exchange rate on this board (the board at the start of the turn): -1
// when off, else mech_grid, capped by the turns left with mech_grid_turns.
int mech_grid_rate(const Board& before, const ScoreOptions& options);
// The GridLost slot with the exchange at `rate` (>= 0): grid and mechs are
// net changes (ScoreKey::GridLost, MechsLost), grid_after the grid power at
// the end of the turn.
inline int32_t mech_grid_slot(int rate, int grid, int mechs, int grid_after) {
  return 2 * grid + (2 * rate + 1) * mechs - (grid_after <= 0 ? kGridOutPenalty : 0);
}

// The tiers in use, in rank order, as ScoreKey indices: every key, minus
// Threat when the threat tier is off. Tier counts (SolveOptions::
// min_proven_tiers, SolveResult::proven_components) and printed scores
// count these.
inline bool tier_used(int key, bool threat_tier) {
  return threat_tier || key != static_cast<int>(ScoreKey::Threat);
}
inline int used_tier_count(bool threat_tier) { return threat_tier ? kScoreKeys : kScoreKeys - 1; }
// The number of leading ScoreKey components that hold the first `tiers`
// tiers in use (Threat, constant when off, is only counted when it is used
// or lies before a used tier).
int components_for_tiers(int tiers, bool threat_tier);
// Short tier names ("grid", "bldg_hp", ...), by ScoreKey index.
const char* score_key_name(int key);

struct Score {
  std::array<int32_t, kScoreKeys> v{};

  int32_t& operator[](ScoreKey k) { return v[static_cast<int>(k)]; }
  int32_t operator[](ScoreKey k) const { return v[static_cast<int>(k)]; }

  // Lexicographic: the first differing component decides.
  auto operator<=>(const Score&) const = default;
  bool operator==(const Score&) const = default;

  // "grid=0 bldg_hp=-1 ...": the tiers in use.
  std::string describe(bool threat_tier = false) const;
};

// Scores the turn from `before` (start of the player's turn) to `after` (end
// of the enemy phase). `ctx` gives the mission (objectives) and `phase` the
// enemy phase's events (blocked spawns, spawned uids, mission end); either
// may be null.
Score score_turn(const Board& before, const Board& after, const TurnContext* ctx = nullptr,
                 const PhaseResult* phase = nullptr, const ScoreOptions& options = {});

}  // namespace itb
