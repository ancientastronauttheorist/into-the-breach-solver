// Stage 9: the perfect-turn search.
//
// solve_turn finds the player's best plan for this turn: the sequence of
// moves, weapon shots and repairs that maximizes score_turn(before, after)
// (score.hpp, strict lexicographic tiers), where `after` is the board at the
// end of the enemy phase (Engine::end_turn). Only what the player can see is
// used: hidden spawn types emerge as unknown (TurnContext::spawn_types), as
// the engine already does.
//
// Chance is adversarial. Every chance node the engine exposes (Grid Defense
// rolls, environment orders and choices, mission picks, spider eggs, the seed
// of a death effect that draws random numbers) is resolved to the outcome
// that is worst for the player:
//   - a plan's value is the minimum score over every chance outcome of the
//     enemy phase;
//   - a chance node during the player's own actions branches the plan: the
//     player sees the outcome before choosing the next action, so the value
//     is min over the outcomes of the best continuation (Plan::contingent).
//
// Action model. At every point of the turn the player may end the turn, or
// give any controllable unit (Pawn::controlled(), alive) one sub-action the
// engine accepts (ActionOptions::check_legal):
//   - Move to any tile of its move area (the Lua Move skill's target area), if
//     it can still move (movement.hpp can_move: not moved, or a bonus move
//     from Shifty / Post_Move);
//   - fire any non-passive weapon slot at any tile of its target area, and
//     for two-click weapons at any tile of the second target area;
//   - Repair (mechs; the pilot's repair skill) at any tile of its area.
// Sub-actions of different units interleave freely (A moves, B shoots, A
// shoots), and the engine's Pawn::FireWeapon bookkeeping decides what each
// unit may still do (Double_Shot, Shifty, Post_Move). Move undo is not part
// of the model (an undone move is the same as not moving).
//
// Search (solver.cpp has the details and the soundness argument of every
// bound):
//   - depth-first over sub-actions; every node is also a leaf ("end the turn
//     here"), so every prefix of a plan is itself a plan;
//   - a transposition table on the exact board hash (board_hash.hpp): orders
//     that reach the same board are one subproblem;
//   - end-of-turn values memoized on the End Turn board hash;
//   - chance branches enumerated lazily: the default outcome first, then each
//     recorded chance node's other options depth-first, stopping as soon as
//     the minimum can no longer beat the incumbent;
//   - alpha-beta over chance (min) nodes and branch-and-bound with sound,
//     tier-by-tier upper bounds;
//   - children ordered by the value of ending the turn right after them.
// The search is anytime: it returns the best plan found when the budget runs
// out, with a sound upper bound on what any plan can score.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "itb/board.hpp"
#include "itb/enemy_phase.hpp"
#include "itb/score.hpp"

namespace itb {

class Engine;

struct SolveOptions {
  double time_limit_s = 10.0;  // <= 0: no limit
  uint64_t node_limit = 0;     // search nodes expanded; 0 = no limit
  // Adaptive budget (off unless both are set and max_time_s > time_limit_s
  // > 0). At time_limit_s the search stops only if the best plan is already
  // proven optimal in its first `min_proven_tiers` score tiers (ScoreKey
  // order; 5 = grid through objectives failed). Otherwise it goes on, now
  // proving only those tiers (a plan that merely ties the best plan on them
  // is cut), until they are proven or max_time_s. See solver.cpp "Adaptive
  // budget".
  int min_proven_tiers = 0;
  double max_time_s = 0;
  // Score the next-turn threat tier (ScoreOptions::threat_tier, threat.hpp),
  // ranked between objective stars and mech HP. Off (the default): the tier
  // is 0 everywhere and left out of tier counts, so results are unchanged.
  // Tier counts (min_proven_tiers, proven_components) count the tiers in
  // use: with it on, 5 = grid through threat.
  bool threat_tier = false;
  // Experiment (ScoreOptions::threat_kills): the tier slot holds kills.
  bool threat_kills = false;
  // ScoreOptions::objectives_first: objective progress first in the tier
  // slot (with a derived cap, objective_progress_cap).
  bool objectives_first = false;
  // ScoreOptions::mech_grid / mech_grid_turns: a mech is worth up to this
  // much grid power (folded into the GridLost slot); -1 = off.
  int mech_grid = -1;
  bool mech_grid_turns = false;
  bool tier_on() const { return threat_tier || threat_kills; }
  ScoreOptions score_options() const {
    ScoreOptions s;
    s.threat_tier = threat_tier;
    s.threat_kills = threat_kills;
    s.objectives_first = objectives_first;
    s.mech_grid = mech_grid;
    s.mech_grid_turns = mech_grid_turns;
    return s;
  }
  // Repair skill per unit uid (Engine::repair_skill of its pilot). Units not
  // listed use Skill_Repair.
  std::vector<std::pair<int32_t, std::string>> repair_skills;
  // Seeds tried for a death effect that draws random numbers (the pawn's
  // seed is hidden in game): the default (uid + 1) and this many more. Seeds
  // are a sample, so a turn where one fires is never proven optimal.
  int extra_death_seeds = 3;
  // Upper limit of chance-tree leaves enumerated for one evaluation; beyond
  // it the evaluation is not exhaustive and the result is not proven.
  int max_chance_leaves = 4096;
  // Global upper bound per score tier, for tiers whose maximum this search
  // cannot derive (ObjectiveProgress, Position: unbounded by default). Each
  // value must be a sound cap on that component of any turn's score.
  std::optional<Score> tier_caps;
  // Switches for testing (all on = the real search).
  bool use_tt = true;      // transposition table and end-of-turn memo
  bool use_bounds = true;  // branch-and-bound with the tier bounds
  bool order_children = true;
  // Entries per table (transposition table, end-of-turn memo).
  size_t tt_max_entries = size_t{1} << 21;
  // Beam pre-pass for a quick first plan: this many states kept per set of
  // units that have played (0 = off). With helper threads and a time limit,
  // thread 0 then widens it (x3 per round) for up to 40% of the time.
  int beam_width = 6;
  // More engines to search with, one thread each (with `engine`, threads =
  // 1 + helper_engines.size()). Each must be loaded from the same game
  // install and is used by its thread alone while the search runs.
  std::vector<Engine*> helper_engines;
};

struct Plan {
  // In execution order, ready for Engine::play_turn. A unit's move and the
  // action that follows it directly are one PlayerAction; interleaved
  // sub-actions are separate entries (a move-only entry has Kind::None).
  std::vector<PlayerAction> actions;
  // The guaranteed value: the minimum over every chance outcome (of the
  // player's actions and of the enemy phase) of score_turn.
  Score worst_case;
  // A chance node fires during the player's actions: `actions` follows its
  // default outcome (no Grid Defense resist, first choice); after any other
  // outcome, solve again from the board as it is to keep the guarantee.
  bool contingent = false;
};

struct SolveStats {
  int threads = 1;
  // Counts and engine times are summed over the threads.
  uint64_t nodes = 0;            // boards expanded (children generated)
  uint64_t sub_actions = 0;      // engine sub-action runs (all chance branches)
  uint64_t enemy_phases = 0;     // Engine::end_turn runs (all chance branches)
  uint64_t chance_branches = 0;  // runs beyond the first, for chance outcomes
  uint64_t tt_hits = 0;          // node values reused from the table
  uint64_t leaf_hits = 0;        // end-of-turn values reused from the memo
  uint64_t duplicate_children = 0;  // sub-actions giving a sibling's board
  uint64_t bound_prunes = 0;     // nodes cut by the tier upper bound
  uint64_t chance_cutoffs = 0;   // chance enumerations stopped early
  uint64_t tt_entries = 0;       // transposition table entries at the end
  uint64_t leaf_entries = 0;     // end-of-turn memo entries at the end
  double time_s = 0;
  double sub_action_s = 0;  // time inside the engine's sub-actions
  double enemy_phase_s = 0; // time inside Engine::end_turn
  double first_plan_s = 0;  // when the first complete plan was found
  double best_plan_s = 0;   // when the returned plan was found
};

struct SolveResult {
  Plan best;
  // The search completed: no plan has a better worst case than `best`.
  bool proven_optimal = false;
  // A sound upper bound on the worst-case score of any plan (equal to
  // best.worst_case when proven). INT32_MAX components are unbounded.
  std::optional<Score> upper_bound;
  // Leading score tiers in which `best` is proven optimal: no plan has a
  // better prefix of this many tiers in use (used_tier_count when proven).
  int proven_components = 0;
  // Every chance node was enumerated exactly (false: a sampled death-effect
  // seed or a chance tree over max_chance_leaves was involved).
  bool chance_exact = true;
  // The search was cut by its budget (time or nodes) before it completed.
  // With the adaptive budget a search that completed its tier proof is not
  // timed out, yet only proven in those tiers (proven_components).
  bool timed_out = false;
  // Adaptive budget: the search ran past time_limit_s, proving only the
  // first min_proven_tiers tiers from then on.
  bool extended = false;
  std::vector<std::string> warnings;
  SolveStats stats;
};

SolveResult solve_turn(Engine& engine, const Board& board, const TurnContext& ctx,
                       const SolveOptions& options = {});

// The worst case of a fixed plan run open-loop (each action as given, chance
// outcomes of the player's actions included): min over every chance outcome
// of score_turn. nullopt if the engine refuses an action in some outcome
// (`refused`, if given, says which).
std::optional<Score> evaluate_plan(Engine& engine, const Board& board, const TurnContext& ctx,
                                   const std::vector<PlayerAction>& plan,
                                   const SolveOptions& options = {}, std::string* refused = nullptr);

// "Grid", "B3 move to C3", ... : one line per action in A1-H8 notation.
std::string describe_action(const Board& board, const PlayerAction& action);

}  // namespace itb
