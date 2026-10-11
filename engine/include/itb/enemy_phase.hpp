// Stage 7: the enemy phase, from End Turn to the end of the enemy's spawns.
//
// Engine::end_turn (engine.hpp) runs what the game does after End Turn, in
// order (stage 7 spec section 1), each step starting from an idle board and
// resolved to idle before the next:
//
//   1. Pawn::EndTurn on player-team pawns (the my-turn flag goes off, so
//      Networked Shielding is off for the whole phase), every web released.
//   2. Status ticks in six phases (necro, fire, Storm Generator, Psion
//      Tyrant, psion regeneration, Regen pilots), one pawn at a time in
//      pawn-list order over a fresh copy of the list per phase, with
//      Board::UpdateLeaders after each pawn.
//   3. Environment steps while the environment has more (Environment::apply).
//   4. Queued attacks: the first pawn in list order with a queued shot fires
//      it from its current tile (Engine's queued-effect path), until none is
//      left. Smoke, death, freezing and water cancel shots as frames run.
//   5. State 2: the victory check (the final turn ends the mission here),
//      the mission's NextTurn hooks, then the queued spawns one at a time:
//      a free tile gets the Vek, a blocked one takes 1 push-mode damage and
//      keeps its spawn for next turn; spawns on water or chasm are dropped.
//   6. "Temp Units Die" (Board::KillTempUnits, armed by Board::SpawnQueued,
//      so not when the mission ended in step 5): every living pawn whose type
//      has Lua TempUnit = true (the Bombling Mech's walking bomb) is killed,
//      an ordinary death with its usual death effects, in list order.
//
// It stops before AI planning: the result is the board the player's turn was
// scored against ("this turn's result"). Mission per-frame hooks run at every
// simulated frame (Environment::update).
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "itb/board.hpp"
#include "itb/environment.hpp"
#include "itb/executor.hpp"
#include "itb/tile_rules.hpp"

namespace itb {

struct LuaWrite;

// What the enemy phase needs beyond the board.
struct TurnContext {
  MissionData mission;
  // The pawn types of Board::spawn_points, in the same order; "" = unknown.
  // The bridge only shows the markers, so recorded boards have no types:
  // such spawns emerge as PhaseResult::emerged_unknown, without a pawn.
  std::vector<std::string> spawn_types;
  // Board::spawn_points are in the game's queue order. The bridge reports
  // them in scan order, which only matters when blocked spawns interact.
  bool spawn_order_known = false;

  // Chance nodes. Grid Defense: true = the populated building resists.
  std::function<bool(Point building, int amount)> grid_resist;
  // Hidden choices (EnvOrder, EnvChoice, MissionRandom): return a branch in
  // [0, node.options). Empty = 0 (the first branch, e.g. scan order).
  std::function<int(const ChanceRecord& node)> choose;
  std::function<uint32_t(const Pawn& dying)> death_seed;
  std::function<int(Resolver&, const Pawn& pawn, const std::vector<Point>& tiles)> spider_egg;

  // Optional logs.
  std::vector<RulesEvent>* events = nullptr;
  std::vector<ResolveEvent>* log = nullptr;
};

struct PhaseResult {
  std::string environment;  // Environment::name() of the mission that ran
  std::vector<PhaseEvent> events;
  // Every chance node taken, in order: Grid Defense rolls, environment
  // orders and choices, Lua death-effect randomness, spider eggs, mission
  // picks. Branch by re-running with other choices.
  std::vector<ChanceRecord> chances;
  std::vector<TimingFlag> timing;  // frame-rate sensitive outcomes
  std::vector<std::string> lua_errors;
  std::vector<LuaWrite> unapplied;  // Lua writes the engine does not model
  // Spawns that emerged with an unknown type: no pawn was added there.
  std::vector<Point> emerged_unknown;
  bool mission_ended = false;
  bool quiescent = true;  // false: some step never settled
  // False if an EnvInexact or EnvUnsupported event was raised.
  bool exact = true;

  bool timing_sensitive() const { return !timing.empty(); }
  int count(PhaseEventType type) const;
};

// One player action for Engine::play_turn.
struct PlayerAction {
  enum class Kind : uint8_t { None, Weapon, Repair };
  int32_t uid = -1;
  Point move = kInvalidPoint;  // move here first (invalid: no move)
  Kind kind = Kind::None;
  // Weapon: the Lua table name (upgrade suffix included). Repair: the repair
  // skill ("" = Skill_Repair).
  std::string weapon;
  Point target = kInvalidPoint;
  std::optional<Point> target2;  // second click of a two-click weapon
};

}  // namespace itb
