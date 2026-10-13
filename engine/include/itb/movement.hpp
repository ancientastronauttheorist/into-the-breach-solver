// Movement and pathfinding (stage 5): which tiles a unit can reach, the exact
// path it walks, its move budget, and relocating it.
//
// Every function that relocates a pawn returns the tile it ended on and takes
// an optional stage 2 RulesContext. With one, Injured's HP loss goes through
// the stage 2 health change (turn shield, Retaliation), a walk's final step
// checks the terrain dangers there (drowning, falling, mines), and the
// whole-move entry points (move_pawn, player_move, ai_move) settle the tile the
// move ended on (settle_at). Without one, the caller applies arrival hazards.
// Intermediate tiles of a walk never trigger hazards. The frame-exact
// executor (executor.hpp) drives moves step by step instead.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "itb/board.hpp"
#include "itb/core.hpp"
#include "itb/tile_rules.hpp"

namespace itb {

// A pathing profile as the game passes it around: (team << 4) | type. Pawns
// carry their own team; Lua callers pass bare PATH_* constants, i.e. team 0.
// A few rules compare the whole value (e.g. "exactly PATH_PROJECTILE"), so the
// team is part of the identity.
struct Pathing {
  PathProfile type = PathProfile::Ground;
  int team = 0;

  constexpr int raw() const { return team * 16 + static_cast<int>(type); }
  // A bare Lua PATH_* constant.
  static constexpr Pathing lua(PathProfile t) { return {t, 0}; }
  constexpr bool operator==(const Pathing&) const = default;
};

// Bit i set = Point::from_index(i). Index order is the game's Point order
// (x-major, then y), which is the order reachable lists come out in.
using TileMask = uint64_t;
std::vector<Point> mask_points(TileMask mask);
constexpr bool mask_has(TileMask m, Point p) { return p.valid() && ((m >> p.index()) & 1u); }

inline constexpr int kUnreached = 0x7fffffff;
using DistanceMap = std::array<int, kTileCount>;

// --- Pawn-level facts ----------------------------------------------------

// is_corpse (tile_rules.hpp) decides which dead pawns still occupy a tile.

// Pawn::GetPathingProfile: teleporter > jumper > burrower > flying >
// Road_Runner pilot > massive > ground, tagged with the pawn's team.
Pathing path_profile(const Board& b, const Pawn& p);

// --- Move budget ---------------------------------------------------------

// Pawn::GetBaseMove: MoveSpeed plus pilot, Kickoff and upgrade bonuses and the
// turn-dependent pilot terms (Youth_Move, Reset_Bonus, Arrogant_Boost),
// clamped at 0.
int base_move(const Board& b, const Pawn& p);
// The part of base_move that follows the board as it changes (Pawn::GetBaseMove
// reads it live): Youth_Move, the level-up skills' move (Move, Skilled,
// Opener, Closer, Pain) and Arrogant_Boost. A recorded move speed minus this
// is the standing MoveState::pilot_bonus.
int live_move_modifiers(const Board& b, const Pawn& p);
// Pawn::GetMoveSpeed: 0 when dead, webbed or unpowered; a pending bonus shift
// replaces the base move.
int move_speed(const Board& b, const Pawn& p);
// Pawn::IsMovable: may use its move skill now.
bool can_move(const Pawn& p);
// Pawn::IsMoved: has moved and has no bonus shift left.
bool is_moved(const Pawn& p);
// Pawn::IsActable (0087a200) during the player's turn (nothing is busy): may
// arm a weapon or Repair in the UI now. Active, alive and powered, not on
// smoke (unless immune), not submerged (water or lava, not flying) and no
// bonus shift pending: after a Shifty or Post_Move pilot's attack the pawn is
// active again for its bonus move only. The UI's weapon box (PawnBox::
// SwapSkill, OnKeyAction, OnMouseAction, Update) arms nothing else; a frozen
// pawn is checked separately (weapons, not Repair, in the UI). Not modelled:
// IsActable's "has a usable weapon" clause (every mech keeps an unlimited
// weapon or a Repair).
bool can_act(const Board& b, const Pawn& p);

// Bookkeeping after a skill fires (Pawn::FireWeapon): `move_skill` for the
// Move skill (slot 0), false for any weapon or repair. Handles moved/active,
// Shifty, Double_Shot, Post_Move and, for weapons, clears every pawn's move
// undo and the shooter's Boosted.
void on_skill_fired(Board& b, Pawn& p, bool move_skill);

// The movement part of Pawn::StartTurn for one pawn: resets moved/bonus
// shift/reset bonus, counts the turn, and (Kickoff Boosters) gives adjacent
// mechs their bonus. Run it for every pawn at turn start; order does not
// matter because the Kickoff bonus is assigned, not added.
void start_turn_movement(Board& b, int32_t uid);
// The movement part of Pawn::EndTurn: drops the Kickoff bonus.
void end_turn_movement(Pawn& p);

// --- Tile predicates -----------------------------------------------------

// BoardSpace::IsPawnSpace: some pawn on p is alive or a corpse.
bool tile_occupied(const Board& b, Point p);
// Board::IsWall: the wall on `from`'s `d` edge stops this profile.
bool wall_blocks(const Board& b, Point from, Dir d, Pathing pr);
// Board::IsEnterable: a walker with this profile may pass through p.
bool can_pass(const Board& b, Point p, Pathing pr);
// BoardSpace::IsTerrainBlocking: the terrain alone forbids ending on p.
bool terrain_blocks(const Board& b, Point p, Pathing pr);
// Board::IsBlocked: a unit with this profile may not end its move on p.
bool is_blocked(const Board& b, Point p, Pathing pr);
// GridSearchable::GetWalkPenalty for a step into p (not out of the start):
// 1000 for an exact PATH_PROJECTILE entering a building, else 1.
int step_cost(const Board& b, Point p, Pathing pr);

// --- Searches ------------------------------------------------------------

// GridSearchable::CalculateDijkstraMap: distances from `start`, finalized for
// every tile within max_dist; kUnreached elsewhere.
DistanceMap distance_map(const Board& b, Point start, Pathing pr, int max_dist);
// GridSearchable::GetReachablePoints: tiles within max_dist that are not
// blocked for this profile (the start is normally excluded, being occupied).
TileMask reachable_points(const Board& b, Point start, int max_dist, Pathing pr);
// Board::GetReachablePointList (Lua Board:GetReachable): as above, minus
// water/lava for jumpers (unless the profile is exactly PATH_PROJECTILE).
TileMask reachable_list(const Board& b, Point start, int max_dist, Pathing pr);

// Player move target area (Lua Move:GetTargetArea), in the game's order.
// Does not check can_move(); an immobile pawn's speed is usually 0 anyway.
TileMask move_area(const Board& b, const Pawn& p);
std::vector<Point> reachable(const Board& b, const Pawn& p);

// AI move candidates (AiPlanner::SetPawn): GetReachablePoints from the move
// origin minus blocked tiles, then the pawn's own tile. Unlike the player
// area, jumpers keep water/lava tiles.
std::vector<Point> ai_move_candidates(const Board& b, const Pawn& p);

// Board::GetPath: the weighted A* path the game walks, [from, ..., to];
// empty if from == to, either is invalid, or there is no path. The goal itself
// need not be passable.
std::vector<Point> find_path(const Board& b, Point from, Point to, Pathing pr);

// --- Executing moves -----------------------------------------------------
// Each returns the tile the pawn ended on.

// Pawn::PushQueuedShot: a queued attack travels with its pawn. Re-validating
// the shifted attack (ComputeAffectedPoints) is the attack stage's job.
void shift_queued_shot(Pawn& p, Point delta);

// Pawn::SetSpace: relocates the pawn (last in the new tile's occupant order),
// records the tile it left, frees it from webs, and costs an Injured pawn
// 1 HP per tile change (unless `no_injury`, i.e. the pawn is busy). Pushes and
// swaps relocate through this too.
void set_space(Board& b, Pawn& p, Point to, bool no_injury = false, RulesContext* ctx = nullptr);

// SetManualPath: walk a path step by step. `forced` is true when the pawn
// moves itself; a non-Pushable pawn ignores unforced walks. The last step is
// re-checked and refused if that tile is now blocked or occupied, leaving the
// pawn on the previous tile.
Point walk_path(Board& b, Pawn& p, const std::vector<Point>& path, bool forced,
                RulesContext* ctx = nullptr);
// Whether a walk's final step onto `to` goes ahead (Pawn::Move): not a
// mountain or building, and no living pawn or corpse there.
bool final_step_allowed(const Board& b, Point to);
// Leap / Charge: jump straight to `to` (no intermediate tiles, no re-check).
Point leap(Board& b, Pawn& p, Point to, bool forced, RulesContext* ctx = nullptr);
Point charge(Board& b, Pawn& p, Point to, bool forced, RulesContext* ctx = nullptr);
// Teleport: relocate to `to`, ignoring Pushable.
Point teleport(Board& b, Pawn& p, Point to, RulesContext* ctx = nullptr);
// Burrow: dive and resurface at `to`; puts out fire. `ai` (forced) clears
// the queued shot instead of carrying it along.
Point burrow(Board& b, Pawn& p, Point to, bool ai, RulesContext* ctx = nullptr);

// The Move skill's effect for pawn `uid` (Lua Move:GetSkillEffect): jumpers
// leap, teleporters teleport, everyone else walks the find_path route with
// its own profile. No legality check and no moved/active bookkeeping. With a
// context the end tile is settled.
Point move_pawn(Board& b, int32_t uid, Point dest, RulesContext* ctx = nullptr);

// A player's move action, undoable until any weapon fires.
struct MoveUndo {
  int32_t uid = -1;
  Point from = kInvalidPoint;
  Point to = kInvalidPoint;   // the chosen destination
  Point end = kInvalidPoint;  // where the move actually ended: settle here
  Tile to_tile;               // the destination tile before the move
  int8_t hp = 0;
  int8_t bonus_shift = 0;
  bool fire = false;
  bool frozen = false;
  bool acid = false;
  bool shield = false;
  bool boosted = false;
  bool injured = false;
  bool infected = false;
};

// Checks can_move() and that dest is in move_area(), then moves (move_pawn)
// and books the Move skill. nullopt if the move is not legal. With a context
// the end tile is settled; without one, settle the result's `end`.
std::optional<MoveUndo> player_move(Board& b, int32_t uid, Point dest,
                                    RulesContext* ctx = nullptr);
// Pawn::Undo. False (and nothing changes) once a weapon has fired since the
// move, or for a pawn that cannot undo.
bool undo_move(Board& b, const MoveUndo& undo, RulesContext* ctx = nullptr);

// Pawn::ManualMove (AI): burrowers burrow, jumpers leap, everyone else
// (teleporters included) walks the find_path route. Returns the pawn's tile
// unchanged if it cannot move. With a context the end tile is settled.
Point ai_move(Board& b, int32_t uid, Point dest, RulesContext* ctx = nullptr);

}  // namespace itb
