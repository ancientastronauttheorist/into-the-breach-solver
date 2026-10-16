// Loading boards from the Lua bridge's recordings (recordings/<run>/m*_solve_input.json).
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "itb/board.hpp"
#include "itb/enemy_phase.hpp"
#include "itb/environment.hpp"

namespace itb {

class GameData;

// Warning for boards from bridges whose terrain-name table called ice "lava"
// (before 2026-05-04): those tiles load as ice, and the same run's other files
// (predictions, verify diffs) say "lava" for ice too.
inline constexpr const char* kIceNamedLava = "terrain id 5 named 'lava' (old bridge name table): loaded as ice";

struct Recording {
  std::string run_id;
  int mission_index = 0;
  int turn = 0;
  std::string label;
  std::string mission_id;
  std::string phase;
  // The bridge dumped this state while the board was busy or a command was
  // still waiting for its effects (bridge `board_busy` / `command_waiting`;
  // `stable` is their negation): not a settled board. Also in `warnings`.
  bool board_busy = false;
  Board board;
  std::vector<int32_t> attack_order;  // Vek uids in the order the game reported
  // Pilot table name per unit uid (e.g. "Pilot_Rock"); the pilot's Lua Skill
  // gives its ability (Engine::pilot_ability), which load_recording already
  // puts on the board's pawns when it has the GameData. A pilot whose Skill
  // needs reactor power (Lua PowerCost > 0) that the recording shows
  // unpowered is left out: the game gives it no Skill (Pilot::IsAbility),
  // and none of these Skills changes the Repair skill either. Its entry in
  // pilot_info says so (skill_unpowered).
  std::vector<std::pair<int32_t, std::string>> pilots;
  int difficulty = -1;  // the bridge's GetDifficulty(), -1 if not recorded
  // The mission and environment data stage 7 needs (TurnContext::mission).
  MissionData mission;
  std::vector<std::string> warnings;  // fields the loader could not map

  // Bridge extension fields (bridge_ext_version >= 1; empty otherwise).
  int bridge_ext_version = 0;
  std::vector<std::string> bridge_errors;  // exports the bridge failed ("where: error")
  // Every unit with a queued shot, any team, in pawn-list order.
  std::vector<int32_t> attack_order_all;
  // Hidden information (not on screen): the queued spawns' pawn types for
  // Board::spawn_points (same order, "" = unknown), and whether
  // spawn_points are in the game's queue order (else scan order).
  std::vector<std::string> spawn_types;
  bool spawn_order_known = false;
  struct PilotInfo {
    int32_t uid = -1;
    std::string id;
    int level = -1, xp = -1, skill1 = -1, skill2 = -1;
    // The pilot's own Skill is recorded as off: the bridge's live
    // "skill_active" (Pawn:IsAbility of the pilot's Skill) is false, or,
    // without it, the save's pilot "power" cells are unset (PowerState::
    // IsPowered: non-empty and the first cell set) for a Skill with a
    // PowerCost. False when the recording does not say (older bridges).
    bool skill_unpowered = false;
  };
  std::vector<PilotInfo> pilot_info;
  // The environment steps the bridge saw (env_strike_log; validation data
  // for the enemy phase that already happened): turn and struck tiles.
  struct EnvStrike {
    int turn = -1;
    std::vector<Point> tiles;
  };
  std::vector<EnvStrike> env_strikes;
};

// What a TurnContext may contain from a recording.
enum class Visibility : uint8_t {
  Player,  // what the player sees: hidden spawn types and queue order left out
  Full,    // everything recorded, for validating the engine against the game
};

// The TurnContext for a recorded board (mission data; with Visibility::Full
// also the spawn types and queue order).
TurnContext turn_context(const Recording& rec, Visibility visibility = Visibility::Player);

// Parses a recording file (the wrapper with a "data.bridge_state" object, or a
// bare bridge_state). Unit types missing from `data` are kept with their
// recorded stats and reported in `warnings`. Squad passives come from the
// mechs' Passive_* weapons and the board psion from the living leaders.
// Returns nullopt and sets `error` if the file is not a bridge board.
std::optional<Recording> load_recording(const std::filesystem::path& path,
                                        const GameData* data, std::string* error);

}  // namespace itb
