// Next-turn threat: a cheap, conservative proxy for "how many enemies will be
// able to attack a building in the coming AI phase" (ScoreKey::Threat).
//
// The scored board (score_turn's `after`) is the end of the enemy phase, just
// before AI planning: every enemy is about to move and queue an attack that
// the player then has one turn to answer. Simulating the AI per leaf is far
// too slow, so this counts the enemies that *could* reach a building:
//
//   An enemy counts when it is a scored enemy (team 6, not neutral, not a
//   mech: Vek and enemy bots alike, Minor pawns included), alive, not frozen,
//   on the board (or underground: it moves from the tile it dived from), has
//   a weapon whose reach class can damage a tile, and some tile it can end
//   its AI move on (the AI's own candidates: GetReachablePoints with its
//   pathing profile and move speed, plus staying put), not smoked unless it
//   ignores smoke, puts a building in that reach.
//
// Buildings: building tiles with HP left that are not frozen (a frozen
// building takes no damage). Grid, unpopulated and objective buildings count
// alike; each enemy counts once however many buildings it reaches.
//
// Unit threats are counted the same way with the units a mission objective
// protects as the targets (the Terraformer, trains, tanks, bombs,
// satellites...: their tiles, unless frozen) instead of buildings. Losing
// one costs a star, and it has few HP, so they rank above objective
// progress (score.hpp kUnitThreatWeight): run 8 lost the Terraformer, and
// both its stars, to two Vek that turn 1 had left next to it while it
// terraformed; ranked this way the replay killed them and kept it.
//
// Reach classes (threat_class), by weapon table name, from the shipped
// weapon scripts (our summary of their target areas and effects):
//   - footprint: the tiles a shot from tile o can damage are fixed offsets
//     from o in each of the four directions (melee, self-centred blasts,
//     artillery at range 2..N with its extra tiles, diagonal blasts);
//   - projectile: the first tile along a line that stops a projectile
//     (mountain, building, a pawn that will not move), or the board's last
//     tile; with splash also the two tiles beside that end;
//   - laser: every tile along a line up to and including the first building
//     or mountain (pawns do not stop it);
//   - none: weapons that never damage a tile this turn (egg layers, blob and
//     totem throwers, hatching, healing, psion tooltips).
// Pawns that will move in the AI phase (enemies with move speed > 0) do not
// stop projectiles here; everything else on the board does. Unknown weapon
// names get a wide footprint (every tile in line, range 1..7): when in
// doubt, an enemy counts.
//
// It ignores what the AI would actually choose (its target scores, other Vek
// moving first, tiles freed or taken during planning), so it is an upper
// bound in spirit rather than a prediction.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "itb/board.hpp"
#include "itb/symbols.hpp"

namespace itb {

enum class ThreatReach : uint8_t { None, Footprint, Projectile, ProjectileSplash, Laser };

struct ThreatClass {
  ThreatReach reach = ThreatReach::None;
  // Footprint: bit t of hit[o] = a shot from tile o can damage tile t.
  const std::array<uint64_t, kTileCount>* hit = nullptr;
};

// The reach class of a weapon table (cached per symbol; thread-safe).
ThreatClass threat_class(Symbol weapon);

// The enemies on `b` that could attack a building in the coming AI phase, in
// board-list order.
std::vector<int32_t> threatening_enemies(const Board& b);
// Their number: what ScoreKey::Threat negates.
int count_threats(const Board& b);
// The same for the units a mission objective protects.
std::vector<int32_t> unit_threatening_enemies(const Board& b);
int count_unit_threats(const Board& b);

}  // namespace itb
