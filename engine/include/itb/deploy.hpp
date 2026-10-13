// Deployment search: where to drop the mechs at mission start.
//
// After the player confirms the deployment the game runs an enemy phase
// before the first player turn (BoardPlayer::FinalizeDeployment ->
// StartState(1)): the enemy action state (webs, status ticks, environment,
// nothing queued yet), then state 2 (victory check, the mission's NextTurn,
// queued spawns), then the AI's planning (ai_phase.hpp): every Vek moves and
// queues the attack the player faces on turn 1. Where the mechs stand decides
// what the Vek can reach, block and target, so the placement is a search
// over the drop zone:
//
//   1. Screen: every set of distinct free drop-zone tiles, one per mech (the
//      mechs in uid order on its tiles in zone order: the AI scores a mech by
//      its tile, not by which mech it is), is played through that phase
//      (Engine::end_turn, then Engine::ai_phase) for `screen_samples` AI
//      seeds, and each turn-1 board is valued by ending turn 1 at once (no
//      player action): score_turn, plus what the phase itself cost.
//   2. Finalists: every assignment of the mechs to the best `finalists`
//      sets (or the caller's placements) gets `samples` AI seeds; every
//      distinct turn-1 board is solved (solve_turn, a short budget) and
//      valued as the phase's cost plus the turn-1 plan's worst case.
//
// Ranking (DeploySummary): the AI's choice is chance, taken as the worst
// case over the sampled outcomes, tier by tier in the solver's order (grid,
// buildings, mechs lost, objectives failed, mech HP, ...): at each tier the
// lowest value over the outcomes first, then the mean; then the next tier.
// Finalists rank by their solved outcomes, then by the screen; the earlier
// placement wins a full tie.
//
// What the search cannot see: the AI seed (hidden; sampled, so a placement's
// outcomes are a sample of what the game can do, not all of it), and turn
// 1's spawn tiles and environment marks (decided during that phase, hidden):
// the turn-1 boards have no spawn markers and keep the deployment's
// environment data. Values are lower bounds (a solve stopped by its budget).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "itb/board.hpp"
#include "itb/enemy_phase.hpp"
#include "itb/score.hpp"
#include "itb/solver.hpp"

namespace itb {

class Engine;

struct DeployOptions {
  // Overall wall-clock budget (s): the screen may use half; the finalists'
  // solves share what is left. <= 0: no limit.
  double time_limit_s = 45.0;
  int screen_samples = 3;     // AI seeds per tile set in the screen
  int finalists = 3;          // best tile sets whose assignments get turn-1 solves
  bool permute = true;        // every assignment of a finalist set (else uid order only)
  int samples = 12;           // AI seeds per finalist placement
  double solve_time_s = 0.5;  // per turn-1 solve; <= 0: nodes only
  uint64_t solve_nodes = 0;   // per turn-1 solve; 0 = no node limit
  uint32_t seed = 1;          // base of the sampled AI seeds (reproducible)
  // Evaluate only these placements (each: one tile per mech, in the order of
  // DeployResult::mechs), e.g. a heuristic's, instead of searching.
  std::vector<std::vector<Point>> placements;
  // Also evaluate these placements as finalists (comparisons).
  std::vector<std::vector<Point>> always;
  // Template for the turn-1 solves (repair skills, beam, score options).
  SolveOptions solve;
  // More engines (one thread each), as SolveOptions::helper_engines.
  std::vector<Engine*> helper_engines;
};

// A placement's sampled outcomes, tier by tier.
struct DeploySummary {
  Score worst;               // the worst outcome (lexicographic)
  Score tier_min;            // per tier, the lowest value over the outcomes
  std::vector<double> mean;  // per tier, the mean over the seeds
  int outcomes = 0;          // distinct outcomes valued
  int seeds = 0;             // AI seeds behind them
};

// Folds outcome values (with the number of seeds that gave each) into a
// summary.
DeploySummary summarize(const std::vector<std::pair<Score, int>>& values);

// a ranks above b: per tier in order, the lower tier_min loses, then the
// lower mean.
bool deploy_better(const DeploySummary& a, const DeploySummary& b);

struct DeployCandidate {
  std::vector<Point> tiles;  // per mech, DeployResult::mechs order
  DeploySummary screen;      // stage 1 (ending turn 1 at once)
  std::optional<DeploySummary> value;  // stage 2 (turn-1 solves), all outcomes solved
  bool all_proven = false;   // every turn-1 solve was proven optimal
};

struct DeployResult {
  bool ok = false;
  std::string error;
  std::vector<int32_t> mechs;  // uids, ascending
  std::vector<Point> zone;     // free drop-zone tiles considered
  int sets = 0;                // tile sets screened (0: placements given)
  std::vector<DeployCandidate> finalists;  // ranked best first
  std::optional<DeployCandidate> best;
  double screen_s = 0, finalist_s = 0, total_s = 0;
  int solves = 0;
  bool timed_out = false;  // the budget cut the search short
  std::vector<std::string> warnings;
};

// The board the player would face on turn 1 after placing the mechs on
// `tiles` (DeployResult::mechs order) and the enemy phase before it with the
// AI seed `ai_seed`. `phase_cost` (optional) gets score_turn of the phase
// itself (deployed board -> turn 1 start, position tier left at 0).
Board deploy_outcome(Engine& engine, const Board& deployed, const TurnContext& ctx,
                     const std::vector<int32_t>& mechs, const std::vector<Point>& tiles, uint32_t ai_seed,
                     Score* phase_cost = nullptr);

// The mechs to place: living player-team mechs on the board, ascending uid.
std::vector<int32_t> deploy_mechs(const Board& deployed);

// Free drop-zone tiles for the mechs: in the zone, on the board, no other
// pawn there.
std::vector<Point> deploy_tiles(const Board& deployed, const std::vector<Point>& zone,
                                const std::vector<int32_t>& mechs);

// The AI seed of sample k (base `seed`), as the search draws them.
uint32_t deploy_sample_seed(uint32_t seed, int k);

DeployResult deploy_search(Engine& engine, const Board& deployed, const TurnContext& ctx,
                           const std::vector<Point>& zone, const DeployOptions& options = {});

}  // namespace itb
