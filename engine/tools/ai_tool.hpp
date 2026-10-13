// itb_inspect --ai DIR: the enemy AI's planning (ai_phase.hpp) against the
// live recordings.
//
//   - Turns 1+: from the board the player left (m<MM>_turn_<TT>_end_turn_01
//     .json), the engine's enemy phase, then the AI phase with the seed the
//     game saved at that turn's start (the archived saveData.lua: the
//     mission's "aiSeed" and every pawn's "targetHistory"), compared with
//     the next turn's board: each Vek's tile and queued target.
//   - Deployment (turn 0): from the deployed board (m<MM>_turn_00_deployed_01
//     .json) the AI phase before turn 1, whose seed is not archived, with
//     `samples` seeds: how often the game's outcome is among them, and the
//     per-Vek agreement of the most common outcome.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

#include "itb/core.hpp"

namespace itb::tools {

struct AiToolOptions {
  std::filesystem::path dir;   // recordings/ (or recordings/live)
  std::filesystem::path game;
  int samples = 64;            // seeds per deployment
  int show = 10;               // mismatches to print
  bool verbose = false;        // every scored location of shown mismatches
};

int run_ai_check(const AiToolOptions& options);

// The AI data a save holds for one mission (saveData.lua, region whose
// "sMission" is `mission_key`, e.g. "Mission5").
struct SavedAi {
  int turn = -1;  // the board's iCurrentTurn (0: not played yet, at deployment)
  uint32_t seed = 0;
  std::unordered_map<int32_t, Point> last_target;  // pawn id -> targetHistory
};
std::optional<SavedAi> read_saved_ai(const std::filesystem::path& save, const std::string& mission_key);

}  // namespace itb::tools
