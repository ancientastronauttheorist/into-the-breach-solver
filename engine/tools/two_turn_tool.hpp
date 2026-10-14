// itb_inspect --two-turn: plans (or deployments) judged over two turns.
//
// Turn mode (a turn-start state): candidate plans come from solver "arms"
// (the live flags with and without the next-turn lookahead, with more
// samples or the sum fold) and from plan files (a recorded solve, an
// itb_live plan). Each distinct plan is played through its enemy phase
// (default chance outcomes), the AI plans the next turn with independent
// sampled seeds (lookahead.hpp next_turn_start), and each of those
// next-turn boards is solved with the live flags: its plan's plain losses
// (grid, mechs, building HP) are what that board forced as far as the
// solver can tell.
//
// Deploy mode (--deploy, a deployed turn-0 state): each placement (given,
// or the deployment search's top finalists) is played through the
// deployment's AI phase for sampled seeds (deploy_outcome); every distinct
// turn-1 board is solved with one arm and then judged over the next turn as
// above. Turn 1's spawn tiles are hidden at deployment: --spawn adds them
// (a retrospective study may take them from the recorded turn-1 board).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "itb/core.hpp"
#include "itb/deploy.hpp"

namespace itb::tools {

struct TwoTurnOptions {
  std::filesystem::path target;
  std::filesystem::path game;
  int threads = 2;
  double time = 10.0;        // the candidate solves (turn mode) / turn-1 solves (deploy mode)
  double next_time = 3.0;    // each next-turn solve
  int next_seeds = 8;        // AI seeds per plan for the next turn
  uint32_t seed = 101;       // base of those seeds (not the solver's)
  // Solver arms: "base" (live flags), "la<N>" (+ lookahead, worst of N),
  // "sum<N>" (sum of N), "idle<N>" (worst of N, idle reply).
  std::vector<std::string> arms;
  std::vector<std::filesystem::path> plan_files;
  // Deploy mode.
  bool deploy = false;
  int deploy_seeds = 8;
  uint32_t deploy_seed = 1000;
  int deploy_top = 0;  // also the deployment search's best N finalists
  DeployOptions deploy_options;
  std::vector<std::vector<Point>> placements;
  std::vector<Point> spawns;
  bool state_placement = true;  // also the state's own placement (the mechs' tiles on it)
  std::filesystem::path json_out;
  bool verbose = false;
};

int run_two_turn(const TwoTurnOptions& options);

}  // namespace itb::tools
