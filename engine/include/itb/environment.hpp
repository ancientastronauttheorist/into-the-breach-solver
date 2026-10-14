// Stage 7: mission environments and mission combat hooks.
//
// In game a mission is a Lua object (GAME.Missions[k]) with a LiveEnvironment
// (Env_Airstrike, Env_Tides, ...). The enemy phase drives both through a few
// Lua calls: IsEnvironmentEffect / ApplyEnvironmentEffect (the environment
// steps), BaseUpdate (every frame: UpdateMission hooks such as the dam flood
// or the acid storm), NextTurn with the enemy team, and IsEndBlocked.
// `Environment` is that interface. The native implementations
// (make_native_environment) rebuild the mission's state from what the bridge
// recorded; a Lua-backed implementation that runs the real mission scripts can
// replace them behind the same interface once the bridge exports the mission
// instance (stage 7 spec section 4).
//
// Where the recorded data does not pin the behaviour down, an environment
// says so (PhaseEventType::EnvInexact / EnvUnsupported) or, for hidden random
// choices, asks the chance-node resolver (EnvHost::choose).
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "itb/board.hpp"
#include "itb/executor.hpp"
#include "itb/space_damage.hpp"
#include "itb/tile_rules.hpp"

namespace itb {

class GameData;

// ---- What the bridge recorded about the mission -----------------------------

// One environment_danger_v2 row: Board:IsEnvironmentDanger(p) with the
// bridge's guesses about it. Only the tile is trusted: damage and kill come
// from bridge heuristics (wind is tagged damage 1, old bridges mislabel
// Mission_Crack and Terratide), so environments dispatch on the mission id.
struct DangerTile {
  Point p = kInvalidPoint;
  int damage = 1;
  bool kill = true;
  int flying_immune = -1;  // -1: not recorded
};

// Env_Volcano / Env_Final instance state (bridge mission_final_volcano /
// mission_final_cave): the ordered Locations the next steps strike.
struct FinalEnvState {
  bool complete = false;
  int mode = 0;   // 1 rocks, 2 lava (tentacles in the cave)
  int phase = 0;  // 1..4
  bool instant = false;
  std::vector<Point> locations;
};

// Stage 8: the mission's objective bookkeeping (missions.lua Mission fields
// and the mission subclasses' counters) as far as the bridge exports it.
// -1 / empty = not recorded. See objectives.hpp for how each is scored.
struct ObjectiveData {
  // Mission.BonusObjs (BONUS_* ids, objectives.hpp BonusId). `bonus_known`:
  // the bridge exported the list (bonus_objective_ids); otherwise only the
  // bonuses other fields reveal are known (objectives.hpp active_bonuses).
  std::vector<int> bonus;
  bool bonus_known = false;
  int kills_done = -1;   // Mission.KilledVek (Mission_AcidTank: AcidKills)
  int kill_target = -1;  // BONUS_KILL_FIVE GetKillBonus() (Mission_AcidTank: 4)
  int kill_limit = -1;   // BONUS_PACIFIST GetPacifistCount()
  int blocked_spawns = -1;  // Mission.BlockedSpawns so far (BONUS_BLOCK)
  int power_start = -1;     // Mission.PowerStart: grid power at deployment (BONUS_GRID)
  int repairs_done = -1, repair_target = -1;      // Mission_Repair RepairPickups / 3
  int mountains_done = -1, mountain_target = -1;  // Mission_Force Mountains / MountainsGoal
  int freeze_target = -1;                         // Mission_FreezeBldg (5)
  std::vector<Point> freeze_buildings;            // Mission_FreezeBldg Buildings
  // Mission_Terraform: zone "grass" tiles still grass (custom tile).
  std::vector<Point> grass;
  bool grass_known = false;
};

struct MissionData {
  std::string mission_id;  // M.ID: what environments dispatch on
  std::string env_type;    // the bridge's heuristic label (informational only)
  std::vector<DangerTile> danger;  // in the bridge's scan order (y, then x)
  std::vector<Point> freeze;       // Ice Storm tiles (environment_freeze)
  std::optional<int> tides_index;  // Env_Tides / Env_Terratide Index
  std::optional<Dir> wind_dir;     // Env_RandomWind WindDir
  std::optional<FinalEnvState> volcano;     // Mission_Final
  std::optional<FinalEnvState> final_cave;  // Mission_Final_Cave
  // Ordered Env_Attack Locations when known (e.g. a future bridge export of
  // the env instance); empty = only the danger tiles are known.
  std::vector<Point> ordered_locations;
  // Env_Belt Belts / BeltsDir (bridge env_instance), in list order: the
  // order the belts push in. `belts_known` false = not recorded (the tiles'
  // conveyor directions are all there is).
  std::vector<std::pair<Point, Dir>> belts;
  bool belts_known = false;
  int32_t hacking_bot = -1;      // Mission_Hacking BotID
  int32_t hacking_building = -1;  // Mission_Hacking HackID
  // Mission_Reactivation's Enemies list (mission instance) was recorded; its
  // members carry Pawn::reactivation_listed on the board, which keeps the
  // list across simulated turns. False: old recordings, every frozen enemy
  // is a candidate.
  bool reactivation_known = false;
  std::optional<bool> infinite_spawn;
  std::vector<int32_t> launching;  // satellite rockets with a queued launch
  int difficulty = -1;
  ObjectiveData objectives;  // stage 8

  // Bridge extension (bridge_ext_version >= 1, stage 7 spec section 4).
  int mission_key = -1;  // the mission's key in GAME.Missions ("Mission<key>")
  int turn_limit = -1;   // the instance's TurnLimit
  // Lua classes of the mission and its LiveEnvironment, most derived first.
  std::vector<std::string> mission_classes, env_classes;
  // Board:GetZone(name) for the zones combat hooks read (empty ones absent).
  std::map<std::string, std::vector<Point>> zones;
  // Raw instance dumps (JSON text) of the mission and its LiveEnvironment,
  // for a Lua-backed Environment.
  std::string mission_instance_json, env_instance_json;
  // The bridge exported every unit's queued shot (attack_order_all), so a
  // unit without one has none (e.g. a stopped train).
  bool all_queued_known = false;
};

// ---- Events ----------------------------------------------------------------------

enum class PhaseEventType : uint8_t {
  EndTurn,         // player-team pawns ended their turn (Pawn::EndTurn)
  WebsCleared,     // Board::NextTurn(6) released `amount` webbed pawns
  StatusTick,      // pawn `uid` ticked in phase `amount` (detail: what it did)
  EnvStep,         // the environment applied step `amount` (detail: what)
  EnvInexact,      // the recorded data does not pin this down (detail: why)
  EnvUnsupported,  // no native implementation for what was recorded
  MissionHook,     // a mission hook changed the board (detail: which)
  ShotFired,       // pawn `uid` fired its queued attack at `point`
  ShotFizzled,     // pawn `uid`'s queued target left its target area
  ShotCancelled,   // pawn `uid` lost its queued attack before firing
  Burrowed,        // pawn `uid` dove underground (off the board until the AI moves)
  MissionEnd,      // state 2: the mission ended (no spawns, no AI)
  SpawnEmerged,    // a Vek emerged at `point` (uid -1: type unknown, no pawn added)
  SpawnBlocked,    // the spawn at `point` was blocked (by `uid`, -1: terrain)
  SpawnDropped,    // the spawn at `point` was removed (tile became water/chasm)
  Thawed,          // Mission_Reactivation thawed `uid`
  TempUnitDied,    // "Temp Units Die": temporary unit `uid` (Lua TempUnit) was killed (detail: type)
};

const char* to_string(PhaseEventType t);

struct PhaseEvent {
  PhaseEventType type;
  Point point = kInvalidPoint;
  int32_t uid = -1;
  int amount = 0;
  std::string detail;
};

// ---- The interface -----------------------------------------------------------------

// What an environment step or mission hook may do. Mirrors the Lua calls
// shipped missions make: Board:AddEffect, direct board edits (SetAcid,
// SetShield, AddPawn, RemovePawn, ...) and random draws.
class EnvHost {
 public:
  virtual ~EnvHost() = default;
  virtual Board& board() = 0;
  virtual const GameData& data() const = 0;
  // Stage 2 context for direct rule edits (set_pawn_acid, set_terrain, ...).
  virtual RulesContext& rules() = 0;
  // Board:AddEffect: append to the stacked effects (FULL_DELAY).
  virtual void add_effect(SkillEffect effect) = 0;
  // A hidden choice (Lua RNG, or state the bridge does not record): returns
  // a branch in [0, options), logged as a chance node. `options` <= 1 -> 0.
  virtual int choose(ChanceKind kind, int options, Point where = kInvalidPoint, int amount = 0) = 0;
  virtual void note(PhaseEventType type, std::string detail, Point where = kInvalidPoint,
                    int32_t uid = -1, int amount = 0) = 0;
  // A uid for a pawn a hook adds (Board:AddPawn).
  virtual int32_t new_uid() = 0;
  // Board:IsBusy() right now (some hooks only run on an idle board).
  virtual bool busy() const = 0;
  // Game:GetTurnCount().
  virtual int turn() const = 0;
};

// The game calls Mission:BaseUpdate() from BoardPlayer::OnLoop (0x008c9760)
// on every frame of the mission, the player's turn included, so the per-frame
// hooks (update) run during the player's actions too: Engine::move /
// fire_weapon / repair run them when ActionOptions::mission is set. An
// Environment lives for one resolution (one player action, or the enemy
// phase) and rebuilds what the Lua instance remembers from the board (bind),
// which is exact because the hooks also ran on every frame before it.
class Environment {
 public:
  virtual ~Environment() = default;
  // The Lua class (and mission) this implements, e.g. "Env_Tides".
  virtual std::string name() const = 0;
  // The instance state the per-frame hook reads (the pawns it watches), from
  // the board before anything resolves. Read-only; called once, before
  // begin() in the enemy phase and before a player action's frames.
  virtual void bind(const Board&) {}
  // Once, at End Turn, after bind and before anything resolves: recorded
  // state the bridge leaves implicit (e.g. the train's queued move) is
  // filled in here. Never called for player actions.
  virtual void begin(EnvHost&) {}
  // update() can change something (else player actions skip the hook).
  virtual bool has_update() const { return false; }
  // Mission:IsEnvironmentEffect(), read once at the start of the enemy phase.
  virtual bool is_effect(EnvHost&) { return false; }
  // Mission:ApplyEnvironmentEffect(): queue this step's effects (add_effect);
  // returns true if another step follows.
  virtual bool apply(EnvHost&) { return false; }
  // Mission:BaseUpdate() (UpdateMission), every simulated frame.
  virtual void update(EnvHost&) {}
  // Mission:BaseNextTurn() with TEAM_ENEMY, in state 2 before the spawns.
  virtual void next_turn(EnvHost&) {}
  // Mission:IsEndBlocked().
  virtual bool end_blocked(EnvHost&) { return false; }
};

// The native implementation for a recorded mission (dispatch on mission_id;
// see the README's environment table for what is exact).
std::unique_ptr<Environment> make_native_environment(const MissionData& mission);
// make_native_environment(mission)->has_update(), without building it (the
// fast path of every player action on a mission without a per-frame hook).
bool native_environment_has_update(const MissionData& mission);

}  // namespace itb
