// itb_live: the engine side of live play (scripts/live_play.py).
//
// Machine-readable JSON in and out. A LiveSession holds the loaded engines
// (one per search thread) and answers requests:
//
//   {"cmd": "board", "state": "<bridge state.json>"}
//       the board as the engine loads it, in the compact form the driver
//       compares (live_board_json below)
//   {"cmd": "solve", "state": "...", "time_limit": 10, "node_limit": 0,
//    "beam_width": -1, "threads": N, "max_time": 0, "min_tiers": 0,
//    "threat_tier": false, "threat_kills": false, "objectives_first": false,
//    "mech_grid": -1, "mech_grid_turns": false}
//       ("threat_tier": rank by the next-turn threat tier, SolveOptions::
//       threat_tier; "threat_kills" / "objectives_first": its variants,
//       SolveOptions::threat_kills / objectives_first; scores then carry
//       "threat" (the tier slot) and the reply lists
//       "threatening_enemies" after the predicted enemy phase;
//       "mech_grid" N >= 0: a mech is worth up to N grid, SolveOptions::
//       mech_grid, folded into the "grid" score slot; "mech_grid_turns"
//       caps N by the mission's turns left)
//       the perfect-turn search, then its plan simulated sub-action by
//       sub-action with the default chance outcome (the outcome the plan
//       follows): the predicted board after every move, weapon and repair,
//       after the player's turn and after the enemy phase
//   {"cmd": "predict", "state": "...", "plan": [actions], "branches": 0}
//       the same simulation for a given plan. The enemy phase's
//       "hidden_choices" list its environment orders / choices and mission
//       random picks (e.g. the Final Cave's bomb drop) with the branch the
//       prediction took (0); "branches" N > 0 adds "enemy_branches": up to
//       N other outcomes of them, each {"choices", "after_enemy",
//       "enemy_phase"}, and "enemy_branches_complete" (all were run).
//       "resist": [[x, y], ...] runs the enemy phase (and its branches)
//       with those Grid Defense rolls resisted instead of none: each entry
//       resists the next roll on its tile, every other roll does not
//       (live_play, after the game kept a building the worst case loses)
//   {"cmd": "deploy", "state": "...", "time_limit": 45, "threads": N,
//    "screen_samples": 3, "finalists": 3, "samples": 12, "solve_time": 0.5,
//    "solve_nodes": 0, "seed": 1, "placements": [[[x,y], ...], ...],
//    "always": [...]}
//       the deployment search (itb/deploy.hpp) on a state with the mechs on
//       the board and a drop_zone (or deployment_zone): "best" and the ranked
//       "finalists", tiles in ascending mech uid order ("placements": only
//       those; "always": also those). The solve request's ranking options
//       ("threat_tier" ... "lookahead", "lookahead_samples", ...) rank the
//       search's turn-1 solves as the turns will be ranked (absent: plain
//       tiers); the scores then carry those tiers
//   {"cmd": "actions", "state": "...", "plan": [actions], "effects": true}
//       the fuzzer's move list (scripts/fuzz_play.py): the plan prefix played
//       as predict plays it, then "board" (the compact board there), "units"
//       (each controllable unit's active / moved / bonus_shift / can_move /
//       can_act) and "actions": every sub-action the engine accepts from that
//       board (the solver's Action model: moves, weapon targets incl. second
//       clicks, Repair), each {"sub", "uid", "weapon", "slot", "target",
//       "target2", "target_kind", "end"} and, with "effects", the "effect" of
//       running it once on a copy (HP and kills by team, pushes, bumps, falls,
//       units created, building / grid / mountain loss, tiles changed, chance
//       nodes, timing flags, Lua errors, unmodelled writes). Refused or empty
//       ones are counted in "rejected" by status. "refused" >= 0: the prefix
//       itself was refused at that step ("refused_reason")
//   {"cmd": "two_turn", "state": "...", "plans": [[actions], ...] (or "plan"),
//    "samples": N | [k, ...], "seed": 101, "greedy": true, "idle": false,
//    "search_nodes": 0, "lookahead_env": false,
//    "next": {solve request fields: "time_limit", "max_time", "min_tiers",
//             ranking options, "survival_cap", ...} | absent,
//    "threads": 1, "score": false (+ ranking options), "boards": false,
//    "next_plans": false, "describe": false}
//       studies (scripts/two_turn_oracle.py): each plan over this turn and
//       the next. Per plan ("results", in order): "ok", "refused" (the
//       action the engine refused, -1), "plan" (with "describe": each action's description), "end_hash" (the board the plan ends
//       the turn on, HashMode::EndTurn, as the solve's "end_states" list it),
//       "now" (the plan's plain worst case over chance, evaluate_plan: grid,
//       building_hp, mechs, stars, mech_hp lost; objectives, kills, vek_hp),
//       "score" (with "score": its worst case under the request's ranking
//       options), "default_outcome" (the plain losses of its default chance
//       outcome), "after_hash", "mission_over" (no next turn), and
//       "samples": for each AI seed k (lookahead.hpp next_turn_start from
//       the default outcome's end board with lookahead_seed(seed, k), hidden
//       spawns sampled as visible types; with "lookahead_env", the
//       environment's hazards planned for that turn, next_turn_mission)
//       the next turn's "greedy" (and
//       "idle"; with "search_nodes" N, "search": the lookahead's Search
//       reply with that node budget) reply loss and, with "next", "solve": a real solve of that
//       board ("worst_case", "proven_optimal", "proven_components",
//       "timed_out", "time_s") and "plain", the plain worst case of its plan.
//       "start_only": true judges the state itself the same way ("start"):
//       e.g. the game's actual next turn
//   {"cmd": "ping"}
//
// Solve and predict replies carry "outcome_key": the board after the enemy
// phase with the mechs' tiles left out (solver.hpp outcome_key). A solve
// request's "exclude_outcomes": [key, ...] makes the search skip plans that
// reach one of those outcomes (SolveOptions::exclude_outcomes): solving
// again with each new plan's key added lists the best plans of K distinct
// outcomes. two_turn results carry "outcome_key" too.
//
// A solve request with "end_states": N > 0 also lists up to N end-of-turn
// boards its search reached ("end_states": each {"plan", "key" (the search's
// value of ending there, an upper bound unless "exact"), "exact",
// "end_hash"}, best key first; SolveOptions::end_states) and how many the
// search offered ("end_states_offered").
//
// The state is a bridge dump (bare, or a recording wrapper with
// data.bridge_state), loaded by load_recording. The solver sees only what the
// player sees: turn_context(rec, Visibility::Player).
//
// Plan actions: {"uid", "move": [x,y]|null, "kind": "none"|"weapon"|"repair",
// "weapon", "target": [x,y]|null, "target2": [x,y]|null} (bridge
// coordinates), the engine's PlayerAction.
#pragma once

#include <filesystem>
#include <memory>
#include <vector>

#include <nlohmann/json.hpp>

#include "itb/board.hpp"

namespace itb {
class Engine;
struct SolveOptions;
}

namespace itb::tools {

// Bumped when the output format changes incompatibly.
inline constexpr int kLiveFormat = 1;

// The compact board the live driver compares with the game: grid power,
// every building tile's HP, and every living on-board unit (sorted by uid)
// with type, tile, HP, team, mech flag, statuses and queued shot ("queued":
// null, or {"weapon": index into the pawn's weapons, "origin": [x,y]|null,
// "target": [x,y]}).
nlohmann::json live_board_json(const Board& board);

class LiveSession {
 public:
  // Loads `threads` engines (one per search thread) from the game install.
  // Throws std::runtime_error if the scripts cannot be loaded.
  LiveSession(const std::filesystem::path& game, int threads);
  ~LiveSession();
  LiveSession(const LiveSession&) = delete;
  LiveSession& operator=(const LiveSession&) = delete;

  // Answers one request. Never throws: failures are {"ok": false, "error"}.
  nlohmann::json handle(const nlohmann::json& request);

  int threads() const { return 1 + static_cast<int>(helpers_.size()); }

 private:
  void ensure_threads(int threads);
  // The helper engines of a `threads`-thread search (SolveOptions::helper_engines).
  void add_helpers(SolveOptions& so, int threads);

  std::filesystem::path game_;
  std::unique_ptr<Engine> main_;
  std::vector<std::unique_ptr<Engine>> helpers_;
};

// The itb_live command line (itb_live.cpp's main).
int run_live(int argc, char** argv);

}  // namespace itb::tools
