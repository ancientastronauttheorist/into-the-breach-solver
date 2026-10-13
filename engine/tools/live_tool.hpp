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
//   {"cmd": "predict", "state": "...", "plan": [actions]}
//       the same simulation for a given plan
//   {"cmd": "deploy", "state": "...", "time_limit": 45, "threads": N,
//    "screen_samples": 3, "finalists": 3, "samples": 12, "solve_time": 0.5,
//    "solve_nodes": 0, "seed": 1, "placements": [[[x,y], ...], ...],
//    "always": [...]}
//       the deployment search (itb/deploy.hpp) on a state with the mechs on
//       the board and a drop_zone (or deployment_zone): "best" and the ranked
//       "finalists", tiles in ascending mech uid order ("placements": only
//       those; "always": also those)
//   {"cmd": "ping"}
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

  std::filesystem::path game_;
  std::unique_ptr<Engine> main_;
  std::vector<std::unique_ptr<Engine>> helpers_;
};

// The itb_live command line (itb_live.cpp's main).
int run_live(int argc, char** argv);

}  // namespace itb::tools
