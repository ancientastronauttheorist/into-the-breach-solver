// Stage 5 movement tests. Vector ids (T/B/P/S/A/U) are the test vectors of
// the stage 5 movement spec, section 8. Boards are 8x8 road unless stated;
// M = team 1 mech, E = team 6 Vek, N = team 2.

#include <doctest/doctest.h>

#include <algorithm>
#include <initializer_list>
#include <utility>
#include <vector>

#include "itb/movement.hpp"

using namespace itb;

namespace {

constexpr Pathing kMech{PathProfile::Massive, 1};    // 0x12
constexpr Pathing kVek{PathProfile::Ground, 6};      // 0x60
constexpr Pathing kFly1{PathProfile::Flyer, 1};      // 0x11
constexpr Pathing kRoad{PathProfile::RoadRunner, 1};  // 0x14
constexpr Pathing kTele{PathProfile::Teleporter, 6};  // 0x65
constexpr Pathing kJump{PathProfile::Jumper, 6};      // 0x66
constexpr Pathing kBurrow{PathProfile::Burrower, 6};  // 0x67

int32_t g_next_uid = 1;

// Adding a pawn can move the others in memory: hold uids, not references,
// across add() calls.
Pawn& add(Board& b, Team team, Point pos) {
  Pawn p;
  p.uid = g_next_uid++;
  p.type = intern("Test");
  p.team = team;
  p.pos = pos;
  p.hp = p.max_hp = 3;
  p.move = 3;
  p.active = true;
  if (team == Team::Player) p.mech = p.massive = true;
  return b.add_pawn(p);
}

Pawn& mech(Board& b, Point pos) { return add(b, Team::Player, pos); }
Pawn& vek(Board& b, Point pos) { return add(b, Team::Enemy, pos); }
Pawn& pawn(Board& b, int32_t uid) { return *b.find_pawn(uid); }

std::vector<Point> pts(std::initializer_list<std::pair<int, int>> list) {
  std::vector<Point> out;
  for (auto [x, y] : list) out.push_back({x, y});
  return out;
}

// Lua Board:GetReachable(start, dist, profile), in output order.
std::vector<Point> R(const Board& b, Point start, int dist, Pathing pr) {
  return mask_points(reachable_list(b, start, dist, pr));
}

bool has(const std::vector<Point>& v, Point p) { return std::find(v.begin(), v.end(), p) != v.end(); }

// Tiles within Manhattan distance r of c (excluding c), x-major order.
std::vector<Point> diamond(Point c, int r) {
  std::vector<Point> out;
  for (int i = 0; i < kTileCount; ++i) {
    const Point p = Point::from_index(i);
    const int d = std::abs(p.x - c.x) + std::abs(p.y - c.y);
    if (d > 0 && d <= r) out.push_back(p);
  }
  return out;
}

std::vector<Point> without(std::vector<Point> v, std::initializer_list<std::pair<int, int>> drop) {
  for (auto [x, y] : drop) std::erase(v, Point{x, y});
  return v;
}

// Board::SetWall sets both sides of the edge.
void set_wall(Board& b, Point p, Dir d) {
  b.tile(p).walls |= static_cast<uint8_t>(1u << static_cast<int>(d));
  b.tile(step(p, d)).walls |= static_cast<uint8_t>(1u << static_cast<int>(opposite(d)));
}

void set_terrain(Board& b, std::initializer_list<std::pair<int, int>> tiles, Terrain t) {
  for (auto [x, y] : tiles) b.tile({x, y}).terrain = t;
}

// T21 / P18: mountains everywhere except a corridor.
Board corridor_board() {
  Board b;
  for (int i = 0; i < kTileCount; ++i) b.tile(Point::from_index(i)).terrain = Terrain::Mountain;
  set_terrain(b, {{1, 1}, {2, 1}, {3, 1}, {3, 2}, {3, 3}, {2, 3}, {1, 3}}, Terrain::Road);
  return b;
}

}  // namespace

// --- Reachability ---------------------------------------------------------

TEST_CASE("T1 move 1 from the middle") {
  Board b;
  mech(b, {3, 3});
  CHECK(R(b, {3, 3}, 1, kMech) == pts({{2, 3}, {3, 2}, {3, 4}, {4, 3}}));
}

TEST_CASE("T2 move 3 covers the radius-3 diamond") {
  Board b;
  mech(b, {3, 3});
  CHECK(R(b, {3, 3}, 3, kMech).size() == 24);
  CHECK(R(b, {3, 3}, 3, kMech) == diamond({3, 3}, 3));
  // Tiles at exactly max_dist are labelled but not expanded.
  const DistanceMap d = distance_map(b, {3, 3}, kMech, 3);
  CHECK(d[Point{3, 6}.index()] == 3);
  CHECK(d[Point{3, 7}.index()] == kUnreached);
}

TEST_CASE("T3 board edges") {
  Board b;
  mech(b, {0, 0});
  CHECK(R(b, {0, 0}, 2, kMech) == pts({{0, 1}, {0, 2}, {1, 0}, {1, 1}, {2, 0}}));
}

TEST_CASE("T4 move 0") {
  Board b;
  mech(b, {3, 3});
  CHECK(R(b, {3, 3}, 0, kMech).empty());
}

TEST_CASE("T5 mountain detour") {
  Board b;
  mech(b, {3, 3});
  b.tile({3, 2}).terrain = Terrain::Mountain;
  CHECK(R(b, {3, 3}, 2, kMech) ==
        pts({{1, 3}, {2, 2}, {2, 3}, {2, 4}, {3, 4}, {3, 5}, {4, 2}, {4, 3}, {4, 4}, {5, 3}}));
}

TEST_CASE("T6 massive wades through and ends in water") {
  Board b;
  mech(b, {3, 3});
  set_terrain(b, {{4, 3}, {5, 3}}, Terrain::Water);
  CHECK(R(b, {3, 3}, 2, kMech) == diamond({3, 3}, 2));
}

TEST_CASE("T7 ground Vek avoid water") {
  Board b;
  vek(b, {3, 3});
  b.tile({4, 3}).terrain = Terrain::Water;
  CHECK(R(b, {3, 3}, 2, kVek) == without(diamond({3, 3}, 2), {{4, 3}, {5, 3}}));
}

TEST_CASE("T8 chasm blocks ground, flyers hover") {
  Board b;
  mech(b, {3, 3});
  b.tile({4, 3}).terrain = Terrain::Hole;
  CHECK(R(b, {3, 3}, 2, kMech) == without(diamond({3, 3}, 2), {{4, 3}, {5, 3}}));
  SUBCASE("T8b flyer may end over the chasm") {
    CHECK(R(b, {3, 3}, 1, kFly1) == pts({{2, 3}, {3, 2}, {3, 4}, {4, 3}}));
  }
}

TEST_CASE("T9 pass through a friendly, don't end on it") {
  Board b;
  mech(b, {3, 3});
  mech(b, {4, 3});
  const auto r = R(b, {3, 3}, 2, kMech);
  CHECK(has(r, {5, 3}));
  CHECK_FALSE(has(r, {4, 3}));
}

TEST_CASE("T10 enemy blocks; T10b Road_Runner passes") {
  Board b;
  mech(b, {3, 3});
  vek(b, {4, 3});
  CHECK_FALSE(has(R(b, {3, 3}, 2, kMech), {5, 3}));
  const auto road = R(b, {3, 3}, 2, kRoad);
  CHECK(has(road, {5, 3}));
  CHECK_FALSE(has(road, {4, 3}));
}

TEST_CASE("T11 civilians (team 2) block a mech") {
  Board b;
  mech(b, {3, 3});
  add(b, Team::None, {4, 3});
  CHECK_FALSE(has(R(b, {3, 3}, 2, kMech), {5, 3}));
}

TEST_CASE("T12 same-team NonGrid blocks") {
  Board b;
  mech(b, {3, 3});
  mech(b, {4, 3}).non_grid = true;
  CHECK_FALSE(has(R(b, {3, 3}, 2, kMech), {5, 3}));
}

TEST_CASE("T13 building blocks walkers, flyers cross but can't land") {
  Board b;
  mech(b, {3, 3});
  b.tile({4, 3}).terrain = Terrain::Building;
  CHECK_FALSE(has(R(b, {3, 3}, 2, kMech), {5, 3}));
  const auto fly = R(b, {3, 3}, 2, kFly1);
  CHECK(has(fly, {5, 3}));
  CHECK_FALSE(has(fly, {4, 3}));
}

TEST_CASE("T14 walls stop walkers, not flyers") {
  Board b;
  mech(b, {3, 3});
  set_wall(b, {3, 3}, Dir::Right);
  CHECK(R(b, {3, 3}, 1, kMech) == pts({{2, 3}, {3, 2}, {3, 4}}));
  CHECK(R(b, {3, 3}, 1, kFly1) == pts({{2, 3}, {3, 2}, {3, 4}, {4, 3}}));
  SUBCASE("T14b walk around the wall") {
    CHECK(R(b, {3, 3}, 2, kMech) ==
          pts({{1, 3}, {2, 2}, {2, 3}, {2, 4}, {3, 1}, {3, 2}, {3, 4}, {3, 5}, {4, 2}, {4, 4}}));
  }
}

TEST_CASE("T15 jumper: player list drops water") {
  Board b;
  vek(b, {3, 3});
  b.tile({4, 3}).terrain = Terrain::Water;
  b.tile({3, 4}).terrain = Terrain::Hole;
  b.tile({2, 3}).terrain = Terrain::Mountain;
  b.tile({3, 2}).terrain = Terrain::Building;
  CHECK(R(b, {3, 3}, 2, kJump) ==
        pts({{1, 3}, {2, 2}, {2, 4}, {3, 1}, {3, 5}, {4, 2}, {4, 4}, {5, 3}}));
  CHECK(mask_has(reachable_points(b, {3, 3}, 2, kJump), {4, 3}));
}

TEST_CASE("T16 burrower can't end on ice, water or conveyors") {
  Board b;
  vek(b, {3, 3});
  b.tile({4, 3}).terrain = Terrain::Ice;
  b.tile({3, 4}).terrain = Terrain::Water;
  b.tile({2, 3}).custom_tile = intern("conveyor1.png");
  CHECK(R(b, {3, 3}, 1, kBurrow) == pts({{3, 2}}));
  // The conveyor direction alone marks a conveyor too.
  b.tile({2, 3}).custom_tile = kNoSymbol;
  b.tile({2, 3}).conveyor = 0;
  CHECK(R(b, {3, 3}, 1, kBurrow) == pts({{3, 2}}));
}

TEST_CASE("T17 teleporter may end over chasm and water") {
  Board b;
  vek(b, {3, 3});
  b.tile({4, 3}).terrain = Terrain::Hole;
  b.tile({3, 4}).terrain = Terrain::Water;
  CHECK(R(b, {3, 3}, 1, kTele) == pts({{2, 3}, {3, 2}, {3, 4}, {4, 3}}));
}

TEST_CASE("T18 Road_Runner wades") {
  Board b;
  mech(b, {3, 3});
  b.tile({4, 3}).terrain = Terrain::Water;
  const auto r = R(b, {3, 3}, 2, kRoad);
  CHECK(has(r, {4, 3}));
  CHECK(has(r, {5, 3}));
}

TEST_CASE("T19 massive crosses a water strip") {
  Board b;
  mech(b, {3, 3});
  set_terrain(b, {{4, 3}, {5, 3}, {6, 3}}, Terrain::Water);
  CHECK(has(R(b, {3, 3}, 3, kMech), {6, 3}));
}

TEST_CASE("T20 mech wreck occupies its tile") {
  Board b;
  Pawn& wreck = mech(b, {4, 3});
  wreck.hp = 0;
  SUBCASE("T20 friendly mech passes") {
    mech(b, {3, 3});
    const auto r = R(b, {3, 3}, 2, kMech);
    CHECK(has(r, {5, 3}));
    CHECK_FALSE(has(r, {4, 3}));
  }
  SUBCASE("T20b Vek can't") {
    vek(b, {3, 3});
    CHECK_FALSE(has(R(b, {3, 3}, 2, kVek), {5, 3}));
  }
  SUBCASE("a dead plain Vek does not occupy") {
    Board c;
    mech(c, {3, 3});
    vek(c, {4, 3}).hp = 0;
    CHECK(has(R(c, {3, 3}, 2, kMech), {4, 3}));
  }
}

TEST_CASE("T21 corridor distances") {
  Board b = corridor_board();
  mech(b, {1, 1});
  CHECK(R(b, {1, 1}, 4, kMech) == pts({{2, 1}, {3, 1}, {3, 2}, {3, 3}}));
  CHECK(R(b, {1, 1}, 6, kMech) == pts({{1, 3}, {2, 1}, {2, 3}, {3, 1}, {3, 2}, {3, 3}}));
  SUBCASE("T21b flyer crosses mountains, never ends on one") {
    CHECK(R(b, {1, 1}, 2, kFly1) == pts({{1, 3}, {2, 1}, {3, 1}}));
  }
}

TEST_CASE("T22 fully walled tile") {
  Board b;
  vek(b, {3, 3});
  for (Dir d : {Dir::Up, Dir::Right, Dir::Down, Dir::Left}) set_wall(b, {3, 3}, d);
  CHECK(R(b, {3, 3}, 3, kVek).empty());
  CHECK(R(b, {3, 3}, 1, kJump) == pts({{2, 3}, {3, 2}, {3, 4}, {4, 3}}));
  CHECK(R(b, {3, 3}, 1, kBurrow).empty());
}

TEST_CASE("B1 exact PATH_PHASING destinations") {
  Board b;
  b.tile({4, 3}).terrain = Terrain::Building;
  vek(b, {5, 3});
  mech(b, {6, 3});
  const Pathing phase = Pathing::lua(PathProfile::Phasing);
  CHECK_FALSE(is_blocked(b, {4, 3}, phase));
  CHECK(is_blocked(b, {5, 3}, phase));
  CHECK_FALSE(is_blocked(b, {6, 3}, phase));
  // With a team tag the occupant rule no longer applies.
  CHECK(is_blocked(b, {6, 3}, Pathing{PathProfile::Phasing, 1}));
}

// --- Paths -----------------------------------------------------------------

TEST_CASE("P1-P5, P10-P13, P21-P22 open-board paths") {
  Board b;
  struct Case {
    const char* id;
    Point from, to;
    std::vector<Point> path;
  };
  const Case cases[] = {
      {"P1", {3, 3}, {5, 5}, pts({{3, 3}, {3, 4}, {3, 5}, {4, 5}, {5, 5}})},
      {"P2", {3, 3}, {1, 1}, pts({{3, 3}, {2, 3}, {1, 3}, {1, 2}, {1, 1}})},
      {"P3", {3, 3}, {5, 1}, pts({{3, 3}, {3, 2}, {3, 1}, {4, 1}, {5, 1}})},
      {"P4", {3, 3}, {1, 5}, pts({{3, 3}, {2, 3}, {1, 3}, {1, 4}, {1, 5}})},
      {"P5", {3, 3}, {3, 6}, pts({{3, 3}, {3, 4}, {3, 5}, {3, 6}})},
      {"P10", {0, 0}, {2, 1}, pts({{0, 0}, {0, 1}, {1, 1}, {2, 1}})},
      {"P11", {0, 0}, {1, 2}, pts({{0, 0}, {0, 1}, {0, 2}, {1, 2}})},
      {"P12", {7, 7}, {5, 6}, pts({{7, 7}, {6, 7}, {5, 7}, {5, 6}})},
      {"P13", {2, 5}, {6, 3}, pts({{2, 5}, {2, 4}, {2, 3}, {3, 3}, {4, 3}, {5, 3}, {6, 3}})},
      {"P21", {6, 6}, {4, 7}, pts({{6, 6}, {5, 6}, {4, 6}, {4, 7}})},
      {"P22", {4, 4}, {6, 2}, pts({{4, 4}, {4, 3}, {4, 2}, {5, 2}, {6, 2}})},
  };
  for (const Case& c : cases) {
    INFO(c.id);
    CHECK(find_path(b, c.from, c.to, kMech) == c.path);
  }
}

TEST_CASE("P6 mountain in the way: UP-first tie") {
  Board b;
  b.tile({4, 3}).terrain = Terrain::Mountain;
  CHECK(find_path(b, {3, 3}, {5, 3}, kMech) == pts({{3, 3}, {3, 2}, {4, 2}, {5, 2}, {5, 3}}));
}

TEST_CASE("P7 two mountains") {
  Board b;
  set_terrain(b, {{4, 3}, {4, 2}}, Terrain::Mountain);
  CHECK(find_path(b, {3, 3}, {5, 3}, kMech) == pts({{3, 3}, {3, 4}, {4, 4}, {5, 4}, {5, 3}}));
}

TEST_CASE("P8 through a friendly") {
  Board b;
  mech(b, {3, 3});
  mech(b, {4, 3});
  CHECK(find_path(b, {3, 3}, {5, 3}, kMech) == pts({{3, 3}, {4, 3}, {5, 3}}));
}

TEST_CASE("P9 around an enemy") {
  Board b;
  mech(b, {3, 3});
  vek(b, {4, 4});
  CHECK(find_path(b, {3, 3}, {5, 5}, kMech) == pts({{3, 3}, {3, 4}, {3, 5}, {4, 5}, {5, 5}}));
}

TEST_CASE("P14 wall forces a detour") {
  Board b;
  set_wall(b, {3, 3}, Dir::Right);
  CHECK(find_path(b, {3, 3}, {4, 3}, kMech) == pts({{3, 3}, {3, 2}, {4, 2}, {4, 3}}));
}

TEST_CASE("P15 the goal need not be enterable") {
  Board b;
  mech(b, {3, 3});
  vek(b, {4, 3});
  CHECK(find_path(b, {3, 3}, {4, 3}, kMech) == pts({{3, 3}, {4, 3}}));
}

TEST_CASE("P16 Vek through a Vek") {
  Board b;
  vek(b, {3, 3});
  vek(b, {4, 3});
  CHECK(find_path(b, {3, 3}, {5, 3}, kVek) == pts({{3, 3}, {4, 3}, {5, 3}}));
}

TEST_CASE("P17 water wall: ground detours, massive wades") {
  Board b;
  set_terrain(b, {{4, 2}, {4, 3}, {4, 4}}, Terrain::Water);
  CHECK(find_path(b, {3, 3}, {5, 3}, kVek) ==
        pts({{3, 3}, {3, 2}, {3, 1}, {4, 1}, {5, 1}, {5, 2}, {5, 3}}));
  CHECK(find_path(b, {3, 3}, {5, 3}, kMech) == pts({{3, 3}, {4, 3}, {5, 3}}));
}

TEST_CASE("P18 corridor path") {
  Board b = corridor_board();
  CHECK(find_path(b, {1, 1}, {1, 3}, kMech) ==
        pts({{1, 1}, {2, 1}, {3, 1}, {3, 2}, {3, 3}, {2, 3}, {1, 3}}));
}

TEST_CASE("P19 P20 symmetric detours go around the low-x side") {
  Board b;
  b.tile({3, 4}).terrain = Terrain::Mountain;
  CHECK(find_path(b, {3, 3}, {3, 5}, kMech) == pts({{3, 3}, {2, 3}, {2, 4}, {2, 5}, {3, 5}}));
  Board c;
  c.tile({3, 2}).terrain = Terrain::Mountain;
  CHECK(find_path(c, {3, 3}, {3, 1}, kMech) == pts({{3, 3}, {2, 3}, {2, 2}, {2, 1}, {3, 1}}));
}

TEST_CASE("P23 no path to self; invalid ends; unreachable goal") {
  Board b;
  CHECK(find_path(b, {3, 3}, {3, 3}, kMech).empty());
  CHECK(find_path(b, {3, 3}, {8, 3}, kMech).empty());
  CHECK(find_path(b, kInvalidPoint, {3, 3}, kMech).empty());
  // Goal ringed by mountains: the goal is enterable only as the goal, and its
  // neighbours are not.
  Board c;
  set_terrain(c, {{4, 3}, {6, 3}, {5, 2}, {5, 4}}, Terrain::Mountain);
  CHECK(find_path(c, {3, 3}, {5, 3}, kVek).empty());
}

TEST_CASE("exact PATH_PROJECTILE pays 1000 to enter a building") {
  Board b;
  b.tile({5, 3}).terrain = Terrain::Building;
  const Pathing proj = Pathing::lua(PathProfile::Projectile);
  CHECK(step_cost(b, {5, 3}, proj) == 1000);
  CHECK(step_cost(b, {5, 3}, Pathing{PathProfile::Projectile, 1}) == 1);
  // The goal exception lets A* end on it; the cost only shows in distances.
  CHECK(find_path(b, {3, 3}, {5, 3}, proj) == pts({{3, 3}, {4, 3}, {5, 3}}));
  // Projectiles don't pass buildings, so a building is never a Dijkstra node.
  CHECK(distance_map(b, {3, 3}, proj, 10)[Point{5, 3}.index()] == kUnreached);
}

// --- Profiles ----------------------------------------------------------------

TEST_CASE("pathing profile order") {
  Board b;
  Pawn& p = mech(b, {3, 3});
  CHECK(path_profile(b, p) == kMech);
  CHECK(path_profile(b, p).raw() == 0x12);
  p.pilot_abilities = kPilotRoadRunner;
  CHECK(path_profile(b, p) == kRoad);
  p.pilot_abilities |= kPilotFlying;  // flying beats Road_Runner
  CHECK(path_profile(b, p) == kFly1);
  Pawn& v = vek(b, {5, 5});
  CHECK(path_profile(b, v).raw() == 0x60);
  v.burrows = true;
  v.flying = true;
  CHECK(path_profile(b, v) == kBurrow);
  v.jumper = true;
  CHECK(path_profile(b, v) == kJump);
  v.teleporter = true;
  CHECK(path_profile(b, v) == kTele);
  // Teleporter/jumper/flying need the pawn alive; burrowing does not.
  v.hp = 0;
  CHECK(path_profile(b, v) == kBurrow);
  v.burrows = false;
  CHECK(path_profile(b, v) == kVek);
  // A dead massive pawn stays massive only as a corpse.
  v.massive = true;
  CHECK(path_profile(b, v) == kVek);
  b.psion = Leader::Necro;
  CHECK(path_profile(b, v) == Pathing{PathProfile::Massive, 6});
}

TEST_CASE("player move area and AI candidates") {
  Board b;
  Pawn& j = vek(b, {3, 3});
  j.jumper = true;
  j.move = 1;
  b.tile({4, 3}).terrain = Terrain::Water;
  CHECK(reachable(b, j) == pts({{2, 3}, {3, 2}, {3, 4}}));
  // The AI keeps water for jumpers and appends its own tile.
  CHECK(ai_move_candidates(b, j) == pts({{2, 3}, {3, 2}, {3, 4}, {4, 3}, {3, 3}}));
  // An underground burrower moves from the tile it dived from; that tile is
  // empty now, so it is a candidate too.
  Pawn& u = vek(b, {6, 6});
  u.burrows = true;
  u.move = 1;
  set_space(b, u, kInvalidPoint);
  CHECK(u.movement.prev_pos == Point{6, 6});
  CHECK(ai_move_candidates(b, u) == std::vector<Point>{{5, 6}, {6, 5}, {6, 6}, {6, 7}, {7, 6}, kInvalidPoint});
}

// --- Move budget -------------------------------------------------------------

TEST_CASE("S1 base move") {
  Board b;
  Pawn& p = mech(b, {3, 3});
  CHECK(move_speed(b, p) == 3);
  CHECK(can_move(p));
}

TEST_CASE("S2 upgrade and pilot level bonus") {
  Board b;
  Pawn& p = mech(b, {3, 3});
  p.movement.move_upgrade = true;
  p.movement.pilot_bonus = 1;
  CHECK(move_speed(b, p) == 5);
}

TEST_CASE("S3 Youth_Move on the first turn only") {
  Board b;
  Pawn& p = mech(b, {3, 3});
  p.pilot_abilities = kPilotYouthMove;
  p.movement.turn_count = 1;
  CHECK(move_speed(b, p) == 6);
  p.movement.turn_count = 2;
  CHECK(move_speed(b, p) == 3);
}

TEST_CASE("S4 Arrogant_Boost loses 1 when damaged") {
  Board b;
  Pawn& p = mech(b, {3, 3});
  p.pilot_abilities = kPilotArrogantBoost;
  p.hp = 2;
  CHECK(move_speed(b, p) == 2);
  p.hp = 3;
  CHECK(move_speed(b, p) == 3);
}

TEST_CASE("S5 webbed") {
  Board b;
  Pawn& p = mech(b, {3, 3});
  p.webbed = true;
  CHECK(move_speed(b, p) == 0);
  CHECK_FALSE(can_move(p));
  CHECK(reachable(b, p).empty());
}

TEST_CASE("S6 frozen keeps its speed but can't move") {
  Board b;
  Pawn& p = mech(b, {3, 3});
  p.frozen = true;
  CHECK_FALSE(can_move(p));
  CHECK(move_speed(b, p) == 3);
}

TEST_CASE("S7 Shifty: one tile after shooting") {
  Board b;
  Pawn& p = mech(b, {3, 3});
  p.pilot_abilities = kPilotShifty;
  on_skill_fired(b, p, /*move_skill=*/false);
  CHECK(move_speed(b, p) == 1);
  CHECK(can_move(p));
  CHECK(p.active);
  // Using the bonus move ends the turn.
  REQUIRE(player_move(b, p.uid, {3, 4}));
  CHECK(p.pos == Point{3, 4});
  CHECK_FALSE(p.active);
  CHECK_FALSE(can_move(p));
  CHECK(is_moved(p));
}

TEST_CASE("S8 Post_Move: full move after moving and shooting") {
  Board b;
  Pawn& p = mech(b, {3, 3});
  p.pilot_abilities = kPilotPostMove;
  p.movement.move_upgrade = true;
  REQUIRE(player_move(b, p.uid, {3, 5}));
  CHECK_FALSE(can_move(p));
  on_skill_fired(b, p, false);
  CHECK(p.movement.bonus_shift == 4);
  CHECK(move_speed(b, p) == 4);
  CHECK(can_move(p));
  CHECK_FALSE(is_moved(p));
}

TEST_CASE("S9 Double_Shot: second shot, no move") {
  Board b;
  Pawn& p = mech(b, {3, 3});
  p.pilot_abilities = kPilotDoubleShot;
  on_skill_fired(b, p, false);
  CHECK(p.moved);
  CHECK(p.active);
  CHECK_FALSE(can_move(p));
  on_skill_fired(b, p, false);
  CHECK_FALSE(p.active);
}

// Pawn::IsActable gates arming a weapon or Repair in the UI (PawnBox): a
// pending bonus shift leaves the pawn active for its bonus move only.
TEST_CASE("S7b Shifty: after attacking, a 1-tile move and nothing else") {
  Board b;
  Pawn& p = mech(b, {3, 3});
  p.pilot_abilities = kPilotShifty;
  CHECK(can_act(b, p));
  on_skill_fired(b, p, false);
  CHECK(p.active);
  CHECK_FALSE(can_act(b, p));  // no second weapon, no Repair
  CHECK(can_move(p));
  CHECK(move_speed(b, p) == 1);
  REQUIRE(player_move(b, p.uid, {3, 4}));
  CHECK_FALSE(p.active);
  CHECK_FALSE(can_act(b, p));
  CHECK_FALSE(can_move(p));
}

TEST_CASE("S8b Post_Move: after attacking, a move and nothing else") {
  Board b;
  Pawn& p = mech(b, {3, 3});
  p.pilot_abilities = kPilotPostMove;
  REQUIRE(player_move(b, p.uid, {3, 5}));
  CHECK(can_act(b, p));
  on_skill_fired(b, p, false);
  CHECK(p.active);
  CHECK_FALSE(can_act(b, p));
  CHECK(can_move(p));
}

TEST_CASE("S9b Double_Shot: two actions unmoved, one after a move") {
  Board b;
  Pawn& p = mech(b, {3, 3});
  p.pilot_abilities = kPilotDoubleShot;
  int acts = 0;
  while (can_act(b, p) && acts < 5) {
    on_skill_fired(b, p, false);
    ++acts;
  }
  CHECK(acts == 2);
  CHECK_FALSE(can_move(p));

  Board b2;
  Pawn& q = mech(b2, {3, 3});
  q.pilot_abilities = kPilotDoubleShot;
  REQUIRE(player_move(b2, q.uid, {3, 5}));
  acts = 0;
  while (can_act(b2, q) && acts < 5) {
    on_skill_fired(b2, q, false);
    ++acts;
  }
  CHECK(acts == 1);
}

TEST_CASE("S9c no pilot: one action, then done") {
  Board b;
  Pawn& p = mech(b, {3, 3});
  on_skill_fired(b, p, false);
  CHECK_FALSE(p.active);
  CHECK_FALSE(can_act(b, p));
  CHECK_FALSE(can_move(p));
}

TEST_CASE("S9d smoke and water: no weapon or Repair, the move stays") {
  Board b;
  Pawn& p = mech(b, {3, 3});
  b.tile({3, 3}).smoke = true;
  CHECK_FALSE(can_act(b, p));
  CHECK(can_move(p));
  p.ignore_smoke = true;
  CHECK(can_act(b, p));
  b.tile({3, 3}).smoke = false;
  b.tile({3, 3}).terrain = Terrain::Water;
  CHECK_FALSE(can_act(b, p));  // submerged
  CHECK(can_move(p));
  p.flying = true;
  CHECK(can_act(b, p));
}

TEST_CASE("S9e Disable_Immunity (Camila Vera): acts from smoke, not from water") {
  // Pawn::IsSmoked is false for a Disable_Immunity pilot, so IsActable holds
  // on smoke; IsSubmerged has no pilot exemption.
  Board b;
  const int32_t camila = mech(b, {3, 3}).uid;
  const int32_t plain = mech(b, {3, 4}).uid;
  Pawn& p = pawn(b, camila);
  const Pawn& other = pawn(b, plain);
  p.pilot_abilities = kPilotDisableImmunity;
  b.tile({3, 3}).smoke = true;
  b.tile({3, 4}).smoke = true;
  CHECK(can_act(b, p));
  CHECK_FALSE(can_act(b, other));  // the immunity is the pilot's, not the tile's
  b.tile({3, 3}).smoke = false;
  b.tile({3, 3}).terrain = Terrain::Water;
  CHECK_FALSE(can_act(b, p));
}

TEST_CASE("S10 Kickoff Boosters bonus is assigned, not summed") {
  Board b;
  b.passives = kPassiveKickoff;
  const int32_t left = mech(b, {2, 3}).uid;
  const int32_t mid = mech(b, {3, 3}).uid;
  const int32_t right = mech(b, {4, 3}).uid;
  const int32_t alone = mech(b, {7, 7}).uid;
  for (int32_t uid : {left, mid, right, alone}) start_turn_movement(b, uid);
  CHECK(pawn(b, mid).movement.kickoff_bonus == 1);
  CHECK(move_speed(b, pawn(b, mid)) == 4);
  CHECK(move_speed(b, pawn(b, left)) == 4);
  CHECK(move_speed(b, pawn(b, alone)) == 3);
  b.passives |= kPassiveKickoffUpgraded;
  start_turn_movement(b, left);
  CHECK(move_speed(b, pawn(b, mid)) == 5);
  end_turn_movement(pawn(b, mid));
  CHECK(move_speed(b, pawn(b, mid)) == 3);
}

TEST_CASE("S11 Reset_Bonus until the next turn start") {
  Board b;
  Pawn& p = mech(b, {3, 3});
  p.movement.reset_bonus = true;
  CHECK(move_speed(b, p) == 5);
  start_turn_movement(b, p.uid);
  CHECK(move_speed(b, p) == 3);
}

TEST_CASE("S12 unpowered or dead") {
  Board b;
  Pawn& p = mech(b, {3, 3});
  p.movement.powered = false;
  CHECK(move_speed(b, p) == 0);
  CHECK_FALSE(can_move(p));
  p.movement.powered = true;
  p.hp = 0;
  CHECK(move_speed(b, p) == 0);
  CHECK_FALSE(can_move(p));
}

TEST_CASE("S13 base move clamps at 0; raw MoveSpeed 0 can't move") {
  Board b;
  Pawn& p = mech(b, {3, 3});
  p.pilot_abilities = kPilotArrogantBoost;
  p.move = 0;
  p.hp = 2;
  CHECK(base_move(b, p) == 0);
  p.movement.kickoff_bonus = 1;
  p.hp = 3;
  CHECK(move_speed(b, p) == 1);
  CHECK_FALSE(can_move(p));
}

// --- Executing moves ----------------------------------------------------------

TEST_CASE("A1 walking through a burning friendly tile") {
  Board b;
  const int32_t uid = mech(b, {3, 3}).uid;
  mech(b, {4, 3});
  b.tile({4, 3}).fire = FireState::Burning;
  CHECK(move_pawn(b, uid, {5, 3}) == Point{5, 3});
  const Pawn& p = pawn(b, uid);
  CHECK_FALSE(p.fire);  // hazards are stage 2's, at (5,3) only
  CHECK(p.movement.prev_pos == Point{4, 3});
}

TEST_CASE("A2 final tile taken before the last step") {
  Board b;
  const int32_t uid = mech(b, {3, 3}).uid;
  const auto path = find_path(b, {3, 3}, {5, 3}, kMech);
  vek(b, {5, 3});
  CHECK(walk_path(b, pawn(b, uid), path, true) == Point{4, 3});
  SUBCASE("stopping on a friendly it was passing") {
    Board c;
    const int32_t q = mech(c, {3, 3}).uid;
    mech(c, {4, 3});
    const auto through = find_path(c, {3, 3}, {5, 3}, kMech);
    mech(c, {5, 3});
    CHECK(walk_path(c, pawn(c, q), through, true) == Point{4, 3});
    CHECK(c.pawns_at({4, 3}).size() == 2);
  }
  SUBCASE("final re-check allows water and chasm") {
    Board c;
    Pawn& q = mech(c, {3, 3});
    c.tile({4, 3}).terrain = Terrain::Hole;
    CHECK(walk_path(c, q, pts({{3, 3}, {4, 3}}), true) == Point{4, 3});
    c.tile({5, 3}).terrain = Terrain::Mountain;
    CHECK(walk_path(c, q, pts({{4, 3}, {5, 3}}), true) == Point{4, 3});
  }
}

TEST_CASE("A3 any relocation frees a webbed pawn") {
  Board b;
  Pawn& p = vek(b, {3, 3});
  p.webbed = true;
  b.tile({3, 3}).web_in = 1;
  teleport(b, p, {6, 6});
  CHECK_FALSE(p.webbed);
  // Only the pawn's flag: the web stays on the tile until the emitter's web
  // check sees it empty.
  CHECK(b.tile({3, 3}).web_in == 1);
  p.webbed = true;
  set_space(b, p, {6, 5});  // e.g. a push
  CHECK_FALSE(p.webbed);
}

TEST_CASE("A4 burrowing puts out fire") {
  Board b;
  Pawn& p = vek(b, {3, 3});
  p.burrows = true;
  p.fire = true;
  CHECK(ai_move(b, p.uid, {3, 5}) == Point{3, 5});
  CHECK_FALSE(p.fire);
}

TEST_CASE("A5 a non-Pushable pawn ignores others' move effects") {
  Board b;
  Pawn& p = vek(b, {3, 3});
  p.pushable = false;
  CHECK(walk_path(b, p, pts({{3, 3}, {3, 4}}), /*forced=*/false) == Point{3, 3});
  CHECK(leap(b, p, {5, 5}, false) == Point{3, 3});
  CHECK(charge(b, p, {3, 6}, false) == Point{3, 3});
  // Its own moves still work.
  CHECK(walk_path(b, p, pts({{3, 3}, {3, 4}}), true) == Point{3, 4});
  CHECK(teleport(b, p, {6, 6}) == Point{6, 6});
  CHECK(burrow(b, p, {1, 1}, false) == Point{1, 1});
}

TEST_CASE("A6 a leap lands in a chasm (the fall is stage 2 settle)") {
  Board b;
  Pawn& p = mech(b, {3, 3});
  b.tile({3, 5}).terrain = Terrain::Hole;
  CHECK(leap(b, p, {3, 5}, true) == Point{3, 5});
  CHECK(p.pos == Point{3, 5});
}

TEST_CASE("moves carry the queued shot along") {
  Board b;
  Pawn& p = vek(b, {3, 3});
  p.queued = {0, {3, 3}, {3, 4}};
  ai_move(b, p.uid, {5, 3});
  CHECK(p.queued.origin == Point{5, 3});
  CHECK(p.queued.target == Point{5, 4});
  Pawn& q = vek(b, {0, 0});
  q.burrows = true;
  q.queued = {0, {0, 0}, {0, 1}};
  ai_move(b, q.uid, {0, 2});  // AI burrowing drops the queued shot
  CHECK_FALSE(q.queued.active());
}

TEST_CASE("Injured loses 1 HP per tile changed") {
  Board b;
  Pawn& p = mech(b, {3, 3});
  p.injured = true;
  move_pawn(b, p.uid, {3, 5});
  CHECK(p.hp == 1);
  leap(b, p, {6, 5}, true);
  CHECK(p.hp == 0);
  CHECK(p.dying);
  // Surface burrowers relocate while busy and are spared.
  Pawn& v = vek(b, {0, 0});
  v.burrows = v.injured = true;
  burrow(b, v, {0, 2}, true);
  CHECK(v.hp == 3);
}

TEST_CASE("Lua Move: jumpers leap, teleporters teleport, burrowers walk") {
  Board b;
  Pawn& j = vek(b, {3, 3});
  j.jumper = true;
  set_terrain(b, {{3, 4}, {4, 3}, {2, 3}, {3, 2}}, Terrain::Mountain);
  CHECK(move_pawn(b, j.uid, {3, 5}) == Point{3, 5});
  Pawn& t = vek(b, {0, 0});
  t.teleporter = true;
  CHECK(move_pawn(b, t.uid, {0, 7}) == Point{0, 7});
  Pawn& w = vek(b, {6, 0});
  w.burrows = true;
  w.fire = true;
  CHECK(move_pawn(b, w.uid, {6, 2}) == Point{6, 2});
  CHECK(w.fire);  // walked, did not burrow
}

TEST_CASE("player_move rejects illegal moves") {
  Board b;
  Pawn& p = mech(b, {3, 3});
  CHECK_FALSE(player_move(b, p.uid, {3, 7}));  // too far
  CHECK_FALSE(player_move(b, p.uid, {3, 3}));  // own tile
  p.active = false;
  CHECK_FALSE(player_move(b, p.uid, {3, 4}));
  p.active = true;
  auto m = player_move(b, p.uid, {3, 4});
  REQUIRE(m);
  CHECK(m->end == Point{3, 4});
  CHECK(p.moved);
  CHECK(p.active);  // may still attack
  CHECK_FALSE(player_move(b, p.uid, {3, 5}));
}

// --- Undo ----------------------------------------------------------------------

TEST_CASE("U1 move then undo") {
  Board b;
  Pawn& p = mech(b, {3, 3});
  b.tile({3, 5}).item = intern("Item_Mine");
  auto m = player_move(b, p.uid, {3, 5});
  REQUIRE(m);
  // Pretend stage 2 settle set off the mine.
  b.tile({3, 5}).item = kNoSymbol;
  p.hp = 1;
  REQUIRE(undo_move(b, *m));
  CHECK(p.pos == Point{3, 3});
  CHECK_FALSE(p.moved);
  CHECK(p.hp == 3);
  CHECK(p.movement.bonus_shift == 0);
  CHECK(b.tile({3, 5}).item == intern("Item_Mine"));
  CHECK(can_move(p));
  CHECK_FALSE(undo_move(b, *m));  // only once
}

TEST_CASE("U1 undoing a bonus move hands it back") {
  Board b;
  Pawn& p = mech(b, {3, 3});
  p.pilot_abilities = kPilotPostMove;
  on_skill_fired(b, p, false);  // shoot first
  auto m = player_move(b, p.uid, {3, 6});
  REQUIRE(m);
  CHECK_FALSE(p.active);
  REQUIRE(undo_move(b, *m));
  CHECK(p.active);
  CHECK(p.movement.bonus_shift == 3);
  CHECK(can_move(p));
}

TEST_CASE("U2 firing a weapon clears move undo") {
  Board b;
  const int32_t p = mech(b, {3, 3}).uid;
  const int32_t q = mech(b, {6, 6}).uid;
  auto m = player_move(b, p, {3, 5});
  auto n = player_move(b, q, {6, 4});
  REQUIRE(m);
  REQUIRE(n);
  on_skill_fired(b, pawn(b, p), false);
  CHECK_FALSE(undo_move(b, *m));
  CHECK_FALSE(undo_move(b, *n));  // every pawn's undo is gone
  CHECK(pawn(b, p).pos == Point{3, 5});
}
