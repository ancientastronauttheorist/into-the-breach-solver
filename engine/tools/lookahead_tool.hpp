// itb_inspect --lookahead-study (lookahead_tool.cpp).
#pragma once

#include <cstdint>
#include <filesystem>

#include "itb/lookahead.hpp"

namespace itb::tools {

struct LookaheadStudyOptions {
  std::filesystem::path target;  // a recording, or a directory of them
  std::filesystem::path game;
  double time = 10.0;       // turn solve (s)
  double next_time = 4.0;   // reference solve of each next-turn board (s); 0 = none
  int threads = 2;
  int samples = 4;          // AI seeds per plan
  uint32_t seed = 101;      // base of those seeds (not the solver's own)
  bool compare = false;     // also solve with SolveOptions::lookahead
  LookaheadOptions la;      // the estimates measured on each next-turn board
  LookaheadOptions solver_la;  // the solver's lookahead (--compare)
  std::filesystem::path json_out;
  bool verbose = false;
  bool skip_base = false;   // --compare: the lookahead arm only
};

int run_lookahead_study(const LookaheadStudyOptions& options);

}  // namespace itb::tools
