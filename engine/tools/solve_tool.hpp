// itb_inspect --solve: the stage 9 search on recorded boards.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "itb/lookahead.hpp"

namespace itb::tools {

struct SolveToolOptions {
  std::filesystem::path target;  // a recording, or a directory of them
  std::filesystem::path game;
  double time_limit = 10.0;
  uint64_t node_limit = 0;
  double max_time = 0;  // SolveOptions::max_time_s (adaptive budget)
  int min_tiers = 0;    // SolveOptions::min_proven_tiers
  int threads = 1;  // search threads (one engine each)
  int beam = -1;    // beam width (-1: the solver's default)
  int sample = 0;  // directory: this many evenly spaced boards (0 = all)
  int shard = 0;   // directory: boards i with i % shards == shard
  int shards = 1;
  bool verbose = false;            // directory: print every plan
  std::filesystem::path json_out;  // one JSON line per board
  bool lua_counts = false;         // print Lua calls by table and method at the end
  uint64_t tt_entries = 0;         // SolveOptions::tt_max_entries (0: the default)
  bool threat_tier = false;        // SolveOptions::threat_tier
  bool kills_tier = false;         // SolveOptions::threat_kills (experiment: kills above mech HP)
  bool objectives_first = false;   // SolveOptions::objectives_first
  int mech_grid = -1;              // SolveOptions::mech_grid (experiment: mechs worth N grid)
  bool mech_grid_turns = false;    // SolveOptions::mech_grid_turns
  bool mech_grid_pilot = false;    // SolveOptions::mech_grid_pilot
  bool lookahead = false;          // SolveOptions::lookahead (next-turn tier)
  int lookahead_samples = 8;       // LookaheadOptions::samples
  LookaheadOptions::Reply lookahead_reply = LookaheadOptions::Reply::Greedy;  // LookaheadOptions::reply
  LookaheadOptions::Fold lookahead_fold = LookaheadOptions::Fold::Worst;      // LookaheadOptions::fold
  uint64_t lookahead_search_nodes = 200;  // LookaheadOptions::search_nodes (Reply::Search)
  bool lookahead_env = false;             // LookaheadOptions::plan_env
  bool grid_survival = false;      // SolveOptions::grid_survival (Grid Defense odds when every line loses the grid)
  double survival_cap = 0;         // SolveOptions::survival_cap_share (the certificate pre-pass; 0: off)
  bool survival_cap_strict = false;  // SolveOptions::survival_cap_strict
  // JSON: also each plan's actions and its worst case scored with the threat
  // tier on ("plan", "threat_eval"), whatever the search ranked by.
  bool threat_eval = false;
};

int run_solve(const SolveToolOptions& options);

// itb_inspect --survival-cert: an upper bound on the grid-survival odds of
// every plan (README "Grid survival", certificate), next to the recorded
// plan's odds. Changes no search.
struct SurvivalCertOptions {
  std::filesystem::path target;  // a recording
  std::filesystem::path game;
  double time_limit = 600;  // per proof (each budget M)
  int threads = 1;
  bool plain = false;  // also prove D*: building HP lost with no resist (plain tiers)
  std::filesystem::path json_out;
};

int run_survival_cert(const SurvivalCertOptions& options);

// itb_inspect --threat-check DIR (threat_tool.cpp): the next-turn threat
// proxy against the engine's enemy phase on recorded boards, and its cost.
int run_threat_check(const std::filesystem::path& dir, const std::filesystem::path& game);

struct DiffWeaponsOptions {
  std::filesystem::path game;
  std::filesystem::path recordings;  // recorded boards to fire on ("" = none)
  int random = 200;                  // random boards
  uint32_t seed = 1;
  std::string weapon;                // only this weapon table
  bool all_tables = false;           // every ported table, not one per behaviour
  bool ports = false;                // list what runs natively instead
};

// itb_inspect --diff-weapons (diff_weapons.cpp).
int run_diff_weapons(const DiffWeaponsOptions& options);

}  // namespace itb::tools
