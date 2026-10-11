// itb_inspect --solve: the stage 9 search on recorded boards.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

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
  // JSON: also each plan's actions and its worst case scored with the threat
  // tier on ("plan", "threat_eval"), whatever the search ranked by.
  bool threat_eval = false;
};

int run_solve(const SolveToolOptions& options);

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
