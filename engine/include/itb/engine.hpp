// Stage 6 integration: player and Vek actions end to end.
//
// An Engine owns the game data and one Lua host. Each action runs the way the
// game runs it:
//
//   target area (Lua GetTargetArea, from the shooter's tile)
//   -> GetSkillEffect (two-click weapons: TranslateFirstClick,
//      GetSecondTargetArea, GetFinalEffect)
//   -> Skill::PrepareEffect and CheckAlterations (Boost, Vek Hormones)
//   -> the frame-exact executor, with every sScript and Pawn:GetDeathEffect
//      run by the same Lua host against the board as it is at that frame
//   -> the Pawn::FireWeapon bookkeeping (moved/active, pilot bonuses).
//
// Moves are the Lua `Move` skill (walk, leap or teleport, plus the Web_Vek and
// Adjacent_Heal abilities). Repairs are the pawn's repair skill
// (`Skill_Repair` unless the caller names a pilot variant). Queued Vek
// attacks recompute their effect from the shooter's current tile and fizzle
// when the stored target left the target area (Skill::FireQueued).
//
// Lua writes made by scripts and death effects (Board:AddEffect,
// Board:SetTerrain, Pawn:SetFrozen, Game:ModifyPowerGrid, ...) are applied to
// the board when the script returns; writes the engine does not model are
// reported in ActionResult::unapplied.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "itb/board.hpp"
#include "itb/enemy_phase.hpp"
#include "itb/executor.hpp"
#include "itb/lua_host.hpp"
#include "itb/space_damage.hpp"
#include "itb/tile_rules.hpp"

namespace itb {

class GameData;
struct AiPhaseOptions;
struct AiPhaseResult;

struct EngineOptions {
  LuaHostOptions lua;
  ResolveConfig config;
  std::optional<Durations> durations;
};

// Per-action hooks and outputs. Everything is optional.
struct ActionOptions {
  // Refuse actions the game would not allow (dead, unpowered or frozen
  // shooter, a pawn that already acted or moved, a target outside the target
  // area). Off: fire whatever was asked, like a script would.
  bool check_legal = true;
  // Grid Defense: return true if the populated building at p resists. Empty =
  // never resists. Every roll is a ChanceRecord in ActionResult::resolve.
  std::function<bool(Point building, int amount)> grid_resist;
  // Seed for a Lua death effect (the pawn's hidden seed in game). Empty =
  // the pawn's uid + 1. Death effects that draw numbers are logged as
  // ChanceKind::LuaRandom.
  std::function<uint32_t(const Pawn& dying)> death_seed;
  // A spider psion egg landing (see ResolveContext::spider_egg).
  std::function<int(Resolver&, const Pawn& pawn, const std::vector<Point>& tiles)> spider_egg;
  // The mission the action is played in. The game runs the mission's
  // per-frame hook (Mission:BaseUpdate, from BoardPlayer::OnLoop) on every
  // frame of the player's turn too: set, every frame of the action runs the
  // native environment's update (environment.hpp), e.g. the acid storm
  // re-applying ACID right after a repair cleared it. Null: no mission hooks
  // (a bare rules test). Must outlive the call.
  const MissionData* mission = nullptr;
  // Hidden choices of a mission hook (MissionRandom; see TurnContext::choose).
  // Empty = branch 0. Logged as chance nodes in ActionResult::resolve.
  std::function<int(const ChanceRecord& node)> choose;
  std::vector<RulesEvent>* events = nullptr;
  std::vector<ResolveEvent>* log = nullptr;
  // The skill's target area from the actor's tile on this same board, when
  // the caller already has it: LuaHost::target_area for this actor and skill
  // that raised no Lua error and drew no random numbers (so it does not
  // depend on the random stream). The legality check uses it instead of
  // asking Lua again.
  const std::vector<Point>* known_area = nullptr;
};

enum class ActionStatus : uint8_t {
  Ok,
  NoPawn,           // no such pawn on the board
  NoWeapon,         // empty slot or unknown weapon table
  CannotAct,        // dead, unpowered, frozen, already acted / moved
  NotInArea,        // the target is not in the (second) target area
  NeedsSecondClick, // a two-click weapon fired without its second target
  NoEffect,         // the skill produced nothing to apply (nothing happened)
  NoUses,           // a limited weapon with no uses left (Pawn::uses)
};

const char* to_string(ActionStatus s);

struct ActionResult {
  ActionStatus status = ActionStatus::Ok;
  std::string weapon;  // the Lua table that fired ("Move", "Prime_Punchmech_A", ...)
  SkillEffect effect;  // the effect as fired: prepared, alterations applied
  ResolveResult resolve;
  Point end = kInvalidPoint;  // where the actor stands afterwards (kInvalidPoint: gone)
  // Errors the game would swallow: the weapon's own Lua calls, scripts and
  // death effects (the game falls back to its defaults, as here).
  std::vector<std::string> lua_errors;
  // Lua writes from scripts and death effects the engine does not model.
  std::vector<LuaWrite> unapplied;
  // What the mission's per-frame hook did during the action (MissionHook
  // events, ActionOptions::mission).
  std::vector<PhaseEvent> mission_events;

  bool ok() const { return status == ActionStatus::Ok; }
};

// The options for the player's actions of the turn `ctx` describes: its
// chance hooks, logs and mission (`ctx` must outlive them). Engine::play_turn,
// the solver and the live tools all play actions with these.
ActionOptions action_options(const TurnContext& ctx);

// Engine::play_turn: the player's actions, then the enemy phase.
struct TurnResult {
  std::vector<ActionResult> actions;  // one per PlayerAction that ran
  // Index of the first action the engine refused (the enemy phase did not
  // run), -1 if every action ran.
  int refused = -1;
  PhaseResult enemy;

  bool ok() const { return refused < 0; }
};

class Engine {
 public:
  // Loads the game data and the Lua host from a game install
  // (GameData::default_game_root() for the configured one). Throws
  // std::runtime_error if the scripts cannot be found.
  static std::unique_ptr<Engine> create(const std::filesystem::path& game_root,
                                        const EngineOptions& options = {});
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  const GameData& data() const;
  LuaHost& lua();
  const EngineOptions& options() const;

  // The Move skill to `dest` (Pawn::FireWeapon slot 0).
  ActionResult move(Board& board, int32_t uid, Point dest, const ActionOptions& opts = {});

  // Fires the weapon in Pawn::weapons[slot] (the Lua table name, upgrade
  // suffix included) at `target`; `target2` is the second click of a
  // two-click weapon.
  ActionResult fire_weapon(Board& board, int32_t uid, int slot, Point target,
                           std::optional<Point> target2 = std::nullopt,
                           const ActionOptions& opts = {});
  // The same with a weapon named directly (need not be carried).
  ActionResult fire_weapon(Board& board, int32_t uid, std::string_view weapon, Point target,
                           std::optional<Point> target2 = std::nullopt,
                           const ActionOptions& opts = {});

  // Repair: the repair skill (Skill_Repair, or a pilot's variant such as
  // Skill_Repair_A / Skill_Repair_Power / Skill_Repair_Punch) at `target`
  // (default: the pawn's own tile). Same rules as a weapon.
  ActionResult repair(Board& board, int32_t uid, Point target = kInvalidPoint,
                      std::string_view skill = "Skill_Repair", const ActionOptions& opts = {});

  // A Vek's telegraphed attack (SkillManager::FireQueued): the stored target
  // with the effect recomputed from the shooter's current tile. The stored
  // shot is used up whether or not it fizzles.
  ActionResult fire_queued(Board& board, int32_t uid, const ActionOptions& opts = {});

  // A pilot's rule-changing ability (board.hpp PilotAbility) from its Lua
  // table's Skill, e.g. "Pilot_Rock" -> Rock_Skill. kPilotNone if none.
  uint32_t pilot_ability(std::string_view pilot_id);
  // The repair skill a pilot brings: Skill_Repair unless the pilot's Skill
  // replaces it (Power_Repair -> Skill_Repair_Power, Mantis_Skill ->
  // Skill_Repair_Punch; the mapping is inferred from the names).
  std::string repair_skill(std::string_view pilot_id);

  // Stage 7: End Turn and the enemy phase (enemy_phase.hpp), up to the end of
  // the enemy's spawns. `board` must be the board as the player leaves it.
  PhaseResult end_turn(Board& board, const TurnContext& ctx);
  // The player's actions in order (move, then weapon or repair), then
  // end_turn. Stops before the enemy phase at the first refused action.
  TurnResult play_turn(Board& board, const std::vector<PlayerAction>& actions,
                       const TurnContext& ctx);

  // The enemy AI's planning after the enemy phase (ai_phase.hpp): every
  // enemy moves and queues its attack, one at a time. `board` is the board
  // end_turn leaves (or a deployed board before the first turn).
  AiPhaseResult ai_phase(Board& board, const AiPhaseOptions& options);

  // Resolves a ready effect with the Lua hooks wired (scripts, death
  // effects), e.g. an environment effect. `out` collects Lua diagnostics.
  ResolveResult resolve(Board& board, const SkillEffect& effect, const WeaponInfo& weapon,
                        const ActionOptions& opts = {}, ActionResult* out = nullptr);

  // Applies Lua writes reported by a script or death effect to the board
  // under resolution. Writes the engine does not model are appended to
  // `unapplied`.
  void apply_writes(Resolver& r, RulesContext& rules, const std::vector<LuaWrite>& writes,
                    std::vector<LuaWrite>& unapplied);

  struct Impl;

 private:
  Engine();
  std::unique_ptr<Impl> impl_;
};

}  // namespace itb
