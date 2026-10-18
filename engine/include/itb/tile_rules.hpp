// Stage 2: what one SpaceDamage does to one tile.
//
// Covers the per-tile damage pipeline (Board::DamageSpace and
// BoardSpace::DamageSpace), pawn damage and healing, terrain transitions,
// buildings and grid damage, tile and pawn status setters, terrain dangers and
// the continuous "settle" rules the game runs every frame.
//
// The game is real-time. On its own this layer is turn-step: things the game
// finishes in later frames are either recorded for a later stage (pushes,
// spawn requests, burrow dives) or completed by `settle` (deferred chasms,
// drowning, falling, acid/fire pickup), and every pawn is treated as not busy.
// The frame-exact executor (executor.hpp) plugs in through
// `RulesContext::frame`, which tells these rules which pawns are busy, when a
// deferred chasm may open and takes over falls. Deaths only set
// `Pawn::dying` (and HP 0); the executor processes and removes dead pawns.
//
// Grid Defense is the one random branch here. It is a chance node: each hit on
// a populated building asks `RulesContext::grid_resist`, and every such call is
// logged as a GridDamaged or GridResisted event.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "itb/board.hpp"
#include "itb/core.hpp"
#include "itb/space_damage.hpp"
#include "itb/symbols.hpp"

namespace itb {

class GameData;

enum class RulesEventType : uint8_t {
  GridDamaged,        // populated building hit, not resisted: grid -amount
  GridResisted,       // populated building hit resisted (amount = the loss avoided)
  BuildingDamaged,    // building lost `amount` HP (populated or not)
  BuildingDestroyed,  // building reached 0 HP
  TerrainDamaged,     // mountain or ice lost 1 HP
  TerrainChanged,     // amount = new Terrain id
  PawnDamaged,        // pawn `uid` lost `amount` HP
  PawnHealed,         // pawn `uid` gained `amount` HP
  PawnKilled,         // pawn `uid` died (death pending)
  PawnRevived,        // a mech corpse was healed back to life
  PawnFell,           // pawn `uid` fell into a chasm
  PawnBurrowed,       // burrower started its dive (it leaves the board when the dive ends)
  PawnSpawned,        // sPawn created pawn `uid`
  ItemTriggered,      // `symbol` = the item that went off
  PodDestroyed,
  PodCollected,
};

struct RulesEvent {
  RulesEventType type;
  Point point = kInvalidPoint;
  int32_t uid = -1;
  int amount = 0;
  Symbol symbol = kNoSymbol;
};

// A push started by a SpaceDamage (Pawn::Push). Resolution is stage 3.
// `dir` may be Dir::Flip (reverses the pawn's queued attack).
struct PendingPush {
  int32_t uid = -1;
  Point from = kInvalidPoint;
  Dir dir = Dir::None;
};

// An sPawn that could not be created here (no GameData, or unknown type).
struct SpawnRequest {
  Symbol type = kNoSymbol;
  Point loc = kInvalidPoint;
  Team team = Team::None;  // Team::None = the type's default team
};

// What the frame-exact executor tells the rules (RulesContext::frame). Without
// it every pawn is idle, deferred chasms open at the next settle, and falls
// kill at once.
class FrameHooks {
 public:
  virtual ~FrameHooks() = default;
  // Pawn::IsBusy. With `ignore_push`, a pawn busy only because a push is
  // running counts as idle (CheckTerrainDangers).
  virtual bool pawn_busy(const Pawn& pawn, bool ignore_push) const = 0;
  // The deferred chasm on p finished its animation and may open now.
  virtual bool hole_ready(Point p) const = 0;
  // Pawn::Fall reached the animation: return true if the executor animates
  // the fall (and calls finish_fall later) or ignores it for now.
  virtual bool start_fall(Pawn& pawn) = 0;
  // KillInstant on `pawn`: its death animation stops where it is.
  virtual void instant_kill(const Pawn& pawn) = 0;
  // Pawn::Burrow(-1, -1) of a hurt burrower (stage 2 H4): the executor plays
  // the dive and takes the pawn off the board when it ends.
  virtual void start_dive(Pawn& pawn) = 0;
  // PowerBar::Damage raised event 17 (an unresisted hit left the grid at
  // exactly 1): the next frame's Board::OnLoop runs crit_defense_shields.
  virtual void grid_critical() = 0;
};

struct RulesContext {
  // Pawn definitions for sPawn. Without it, spawns become SpawnRequests.
  const GameData* data = nullptr;

  // Chance node for Grid Defense: called once per hit on a populated building
  // with the grid power at stake; return true if the building resists. Empty =
  // never resists.
  std::function<bool(Point building, int amount)> grid_resist;

  // Optional event log (scoring, chance-node bookkeeping).
  std::vector<RulesEvent>* events = nullptr;

  // Lua sScript hook, called before the hit. Without it, scripts are recorded.
  std::function<void(Board&, const std::string& script, Point loc)> run_script;

  // Name of the shot being resolved (EventSystem::GetLastShot). Train shots
  // ("Train_Move", "Armored_Train_Move") do not collapse cracked tiles.
  Symbol current_shot = kNoSymbol;

  // Preview / event-freeze mode: populated buildings lose HP but no grid.
  bool freeze_events = false;

  // EventSystem::HitSomething: the shot being resolved dealt damage to a
  // unit or a building (Void Shocker, Passive_VoidShock). Set by
  // damage_pawn (a living pawn's HP would drop; never a Wall, RockThrown or
  // BombRock) and damage_building (an unresisted hit that takes HP); the
  // executor clears it when a new shot is recorded.
  bool shot_hit = false;

  // Frame-exact executor hooks (busy pawns, deferred chasms, falls, dives).
  FrameHooks* frame = nullptr;

  // Outputs, in the order they happened.
  std::vector<PendingPush> pushes;
  std::vector<SpawnRequest> spawns;
  std::vector<std::string> scripts;   // sScripts seen while run_script was empty
  // Uids that dove (Pawn::Burrow). With `frame` the executor also takes them
  // off the board when the dive ends; without it the caller does.
  std::vector<int32_t> burrow_dives;
};

// ---- Damage pipeline -------------------------------------------------------

// GetDamage: DAMAGE_ZERO reads as 0.
int effective_damage(const SpaceDamage& sd);

// Board::DamageSpace(SD): script, the tile pipeline in weapon mode, then sPawn.
void apply_space_damage(Board& board, const SpaceDamage& sd, RulesContext& ctx);

// BoardSpace::DamageSpace(SD, mode): the ordered per-tile steps.
void damage_tile(Board& board, Point p, SpaceDamage sd, DamageMode mode, RulesContext& ctx);
// Same with an SD whose only field is iDamage = damage (BoardSpace::DamageSpace(int, int)
// and, in weapon mode, Board::DamageSpace(Point, int)).
void damage_tile(Board& board, Point p, int damage, DamageMode mode, RulesContext& ctx);

// Pawn::Damage(SD, mode) and Pawn::Damage(int, mode).
void damage_pawn(Board& board, Pawn& pawn, SpaceDamage sd, DamageMode mode, RulesContext& ctx);
void damage_pawn(Board& board, Pawn& pawn, int damage, DamageMode mode, RulesContext& ctx);

// Pawn::ModifyHealth: raw HP change (negative = damage) with turn shield,
// burrow dive and Retaliation.
void modify_health(Board& board, Pawn& pawn, int delta, DamageMode mode, RulesContext& ctx);

// Pawn::Retreat (bEvacuate, Mission_Volatile): see tile_rules.cpp.
void retreat_pawn(Board& board, Pawn& pawn, RulesContext& ctx);

// Pawn::Kill: death pending, HP 0, queued shot cleared. No-op on a dead pawn.
void kill_pawn(Board& board, Pawn& pawn, RulesContext& ctx);
// Pawn::KillInstant (drowning, falling, chasms): Kill, and the death
// animation stops where it is (also on an already dead pawn).
void kill_pawn_instant(Board& board, Pawn& pawn, RulesContext& ctx);

// Pawn::Fall: a non-flying (or frozen) pawn drops into the chasm and dies.
// With frame hooks the executor animates the fall and calls finish_fall.
void fall_pawn(Board& board, Pawn& pawn, RulesContext& ctx);
// The end of the fall animation: off the board (never a corpse), KillInstant.
void finish_fall(Board& board, Pawn& pawn, RulesContext& ctx);

// ---- Terrain ----------------------------------------------------------------

// SetTerrain, including the pseudo-ids Fire (11), Acid (12), Lava (14),
// spikes (15) and crack (16).
void set_terrain(Board& board, Point p, int terrain, RulesContext& ctx);
inline void set_terrain(Board& board, Point p, Terrain t, RulesContext& ctx) {
  set_terrain(board, p, static_cast<int>(t), ctx);
}
void damage_terrain(Board& board, Point p, int damage, RulesContext& ctx);  // ice, mountains
void add_building(Board& board, Point p, RulesContext& ctx);  // iTerrain = BUILDING
bool is_populated(const Tile& tile);
bool is_crackable(const Board& board, Point p);
void set_cracked(Board& board, Point p, bool on, RulesContext& ctx);

// ---- Tile statuses ----------------------------------------------------------

void set_tile_fire(Board& board, Point p, bool on, RulesContext& ctx);
void set_tile_smoke(Board& board, Point p, bool on, RulesContext& ctx);
void set_tile_acid(Board& board, Point p, bool on, RulesContext& ctx);
void set_tile_frozen(Board& board, Point p, bool on, RulesContext& ctx);
// SetForceField / RemoveForceField: structure shield, else the first pawn's.
void set_tile_shield(Board& board, Point p, bool on, RulesContext& ctx);
// Passive_CritDefense (Board::OnLoop @00955070, the frame after PowerBar::
// Damage @008f4230 left the grid at exactly 1): SetForceField on every tile
// whose terrain is still BUILDING. A plain building at 0 HP is rubble by
// then and is skipped; a unique building at 0 HP is not.
void crit_defense_shields(Board& board, RulesContext& ctx);
// Tile IsForceField / IsFrozen: the structure's, or the first pawn's.
bool tile_shielded(const Board& board, Point p);
bool tile_frozen(const Board& board, Point p);

// Items (items.lua). The SpaceDamage an item applies when it goes off; unknown
// names give an empty SpaceDamage.
SpaceDamage item_damage(Symbol item);
// ClearItem: remove the tile's item and apply its damage to the tile.
void trigger_item(Board& board, Point p, RulesContext& ctx);

// ---- Pawn statuses ----------------------------------------------------------

void set_pawn_fire(Board& board, Pawn& pawn, bool on);
void set_pawn_frozen(Board& board, Pawn& pawn, bool on);
void set_pawn_acid(Board& board, Pawn& pawn, bool on);
void set_pawn_shield(Board& board, Pawn& pawn, bool on);
void set_pawn_injured(Pawn& pawn, bool on);

// ---- Pawn predicates ---------------------------------------------------------

bool is_vek(const Pawn& pawn);  // IsTeam(7): enemy team, Vek faction
bool mutation_affects(const Board& board, const Pawn& pawn, Leader mutation);
// Pawn::ComputeHealthTotal adds +1 max HP under mutation 1 (Soldier psion,
// LEADER_HEALTH) or 7 (Psion Abomination, LEADER_BOSS).
inline bool mutation_adds_health(Leader mutation) {
  return mutation == Leader::Health || mutation == Leader::Boss;
}
bool is_corpse(const Board& board, const Pawn& pawn);  // leaves / is a corpse
bool counts_as_pawn(const Board& board, const Pawn& pawn);  // alive or corpse
bool is_flying(const Pawn& pawn);   // flying (trait or pilot) and alive
bool is_massive(const Board& board, const Pawn& pawn);
bool is_armored(const Board& board, const Pawn& pawn);
bool is_fire_immune(const Board& board, const Pawn& pawn);
bool is_turn_shielded(const Board& board, const Pawn& pawn);  // Networked Shielding

// ---- Occupants ----------------------------------------------------------------

// The pawns on p (excluding fallen ones), in board-list order.
std::vector<Pawn*> occupants(Board& board, Point p);
// BoardSpace::IsPawnSpace(true): some occupant is alive or a corpse.
bool has_pawn(const Board& board, Point p);
// occ[0]: the first occupant (alive or not), or null.
Pawn* first_occupant(Board& board, Point p);
// Board::GetPawn(p): the first living occupant, else the last one (a corpse);
// null when the tile has no pawn (IsPawnSpace).
Pawn* board_pawn(Board& board, Point p);
// Board::GetPawn(p, false): the first living occupant, else the last one even
// if it is a dead body still on the tile; null only when nobody is listed.
// Board::ApplyEffect finds the pawn of a movement entry (charge, leap, walk,
// teleport, burrow) this way, so a body killed earlier in the same effect is
// still moved.
Pawn* listed_pawn(Board& board, Point p);

// ---- Continuous rules ----------------------------------------------------------

// CheckAcidFire: healing smoke, then tile fire/lava, acid pool, spikes.
void check_acid_fire(Board& board, Point p, Pawn& pawn, RulesContext& ctx);
// CheckTerrainDangers: drowning, falling, Fire Boost, item pickup.
void check_terrain_dangers(Board& board, Point p, RulesContext& ctx);

// ---- Webs ----------------------------------------------------------------------
// Natively a web is tile state: the emitting tile lists the directions it
// webs (Tile::web_out), the held tile counts its webs (Tile::web_in). A pawn
// is webbed by several tiles at once and stays webbed until the last of
// them lets go. Pawn::webbed is the flag the pawn reads (IsGrappled, moves).

// BoardSpace::SetGrappled(d) (@0091b300) on `from`: if its Dir(d) neighbour
// is grappleable (a building, or a living pawn or corpse), d joins from's
// web_out, the neighbour holds one web more (SetGrappled(5)) and its first
// occupant is webbed. A direction the tile already webs is not added twice.
void add_web(Board& board, Point from, Dir d);
// The tiles webbing p: bit d set = p's Dir(d) neighbour lists the web.
uint8_t web_sources(const Board& board, Point p);
// Board::NextTurn(6): every tile's ClearGrapple(0, true): no web anywhere
// and no pawn webbed. Returns the number of pawns that were webbed.
int clear_all_webs(Board& board);

// BoardSpace::OnLoop's web part (part of settle_tile_frame): breaks the webs
// tile p emits whose emitter or target no longer holds, then webs p's first
// occupant if p still holds a web and that pawn is not busy.
void check_webs(Board& board, Point p, RulesContext& ctx);

// One frame of BoardSpace::OnLoop's rules on p: deferred chasms, tile
// cleanup, then hazards for every idle occupant and CheckTerrainDangers.
void settle_tile_frame(Board& board, Point p, RulesContext& ctx);
// One frame of Pawn::OnLoop's status rules (Fire Boost, smoke and water put
// out fire).
void settle_pawn_frame(Board& board, Pawn& pawn, RulesContext& ctx);

// The tile rules on p and the pawn rules of its occupants, until they stop
// changing anything: what arriving on p does once the mover is idle.
int settle_at(Board& board, Point p, RulesContext& ctx);

// Runs the per-frame tile and pawn rules over the whole board until nothing
// changes (every tile in x-major order, then every pawn in list order, as the
// game's frame does). Returns the passes used.
int settle(Board& board, RulesContext& ctx);

}  // namespace itb
