// Stage 3/4: pushes, deaths and the SkillEffect executor. "eNN" test names
// are the numbered vectors of the stage 3/4 spec (section E); the rest cover
// the clock and branches the vectors don't reach.

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "itb/engine.hpp"
#include "itb/executor.hpp"
#include "itb/game_data.hpp"
#include "itb/movement.hpp"
#include "itb/objectives.hpp"
#include "itb/timing.hpp"

using namespace itb;

namespace {

const GameData* game_data() {
  static const GameData* data = [] {
    const auto root = GameData::default_game_root();
    if (root.empty() || !std::filesystem::exists(root / "scripts" / "scripts.lua")) {
      return static_cast<GameData*>(nullptr);
    }
    return new GameData(GameData::load(root));
  }();
  return data;
}

#define REQUIRE_GAME_DATA()                                         \
  if (!game_data()) {                                               \
    MESSAGE("game scripts not found (set ITB_GAME_DIR); skipping"); \
    return;                                                         \
  }

struct World {
  Board board;
  ResolveContext ctx;
  std::vector<RulesEvent> events;
  std::vector<ResolveEvent> log;
  ResolveResult last;

  explicit World(double fps = 60.0) {
    ctx.rules.events = &events;
    ctx.log = &log;
    ctx.config.fps = fps;
    ctx.data = game_data();
  }

  Pawn& add(const Pawn& p) { return board.add_pawn(p); }
  Pawn* pawn(int32_t uid) { return board.find_pawn(uid); }
  Tile& tile(Point p) { return board.tile(p); }

  ResolveResult fire(const SkillEffect& se, WeaponInfo w = {}) {
    last = resolve_effect(board, se, w, ctx);
    return last;
  }

  // Frames of the logged events of one kind (optionally for one pawn).
  std::vector<int64_t> frames(ResolveEventType type, int32_t uid = -2) const {
    std::vector<int64_t> out;
    for (const ResolveEvent& e : log) {
      if (e.type == type && (uid == -2 || e.uid == uid)) out.push_back(e.frame);
    }
    return out;
  }
  int64_t frame_of(ResolveEventType type, int32_t uid = -2) const {
    const auto f = frames(type, uid);
    return f.empty() ? -1 : f.front();
  }
  bool removed(int32_t uid) const {
    return std::any_of(log.begin(), log.end(), [&](const ResolveEvent& e) {
      return e.type == ResolveEventType::PawnRemoved && e.uid == uid;
    });
  }
  int count(RulesEventType type) const {
    return static_cast<int>(std::count_if(events.begin(), events.end(),
                                          [&](const RulesEvent& e) { return e.type == type; }));
  }
};

Pawn vek(int32_t uid, Point pos, int hp = 3, int max_hp = 3) {
  Pawn p;
  p.uid = uid;
  p.type = intern("Test_Vek");
  p.pos = pos;
  p.hp = static_cast<int8_t>(hp);
  p.max_hp = static_cast<int8_t>(max_hp);
  p.team = Team::Enemy;
  p.faction = Faction::Default;
  p.active = true;
  return p;
}

Pawn mech(int32_t uid, Point pos, int hp = 3) {
  Pawn p;
  p.uid = uid;
  p.type = intern("Test_Mech");
  p.pos = pos;
  p.hp = p.max_hp = static_cast<int8_t>(hp);
  p.team = Team::Player;
  p.mech = true;
  p.corpse = true;
  p.active = true;
  return p;
}

SkillEffect effect_from(Point origin) {
  SkillEffect se;
  se.origin = origin;
  return se;
}

SpaceDamage push_sd(Point p, Dir d, int damage = 0) { return space_damage(p, damage, d); }

void make_mountain(Tile& t, int hp = 2) {
  t.terrain = Terrain::Mountain;
  t.hp = static_cast<int8_t>(hp);
  t.max_hp = 2;
}

constexpr Point A{2, 2};
constexpr Point R{3, 2};

}  // namespace

// ---- The clock -------------------------------------------------------------------

TEST_CASE("clock: 60 fps steady step") {
  const FrameClock c = FrameClock::steady(60.0);
  CHECK(c.step == doctest::Approx(1.0 / 60.0).epsilon(1e-6));
  CHECK(c.speed == doctest::Approx(16.0 / 60.0).epsilon(1e-6));
  // Below 32 fps the per-frame step is clamped (the game slows down).
  CHECK(FrameClock::steady(20.0).step == doctest::Approx(0.5f * 0.0625f));
}

TEST_CASE("clock: straight projectiles impact by distance, artillery all together") {
  const FrameClock c = FrameClock::steady(60.0);
  const int expect[] = {5, 10, 16, 21, 26, 32, 37};
  for (int d = 1; d <= 7; ++d) {
    CHECK(projectile_updates(c, {0, 0}, {0, d}, 0.7f) == expect[d - 1]);
    CHECK(arc_updates(c, {0, 0}, {0, d}, 18.0f, 3.0f) == 48);
  }
}

TEST_CASE("clock: trackers and delays") {
  const FrameClock c = FrameClock::steady(60.0);
  // The steady 60 fps step is 1/60 in float32 (0.016666668, a hair above
  // 1/60): a push is past 0.4 s after 24 updates, and repeated subtraction
  // leaves a 0.15 s delay just above zero after 9.
  CHECK(c.tracker_updates(0.4f) == 24);
  CHECK(c.tracker_updates(0.15f) == 10);
  CHECK(c.delay_updates(0.15f) == 10);
  // A standard Vek death animation: 8 frames of 0.14 s, 9 updates each.
  const AnimTimeline death(c, 8, 0.14f);
  CHECK(death.total_updates() == 72);
  CHECK(death.progress(35) < 0.5f);
  CHECK(death.progress(36) == doctest::Approx(0.5f));
}

// ---- Stage 3 vectors: pushes ------------------------------------------------------

TEST_CASE("e01 free push") {
  World w;
  w.add(vek(1, A));
  SkillEffect se = effect_from({0, 2});
  se.add_damage(push_sd(A, Dir::Right));
  w.fire(se);
  CHECK(w.pawn(1)->pos == R);
  CHECK(w.pawn(1)->hp == 3);
}

TEST_CASE("e02 blocked by a pawn: both take 1, nobody moves") {
  World w;
  w.add(vek(1, A));
  w.add(vek(2, R));
  SkillEffect se = effect_from({0, 2});
  se.add_damage(push_sd(A, Dir::Right));
  w.fire(se);
  CHECK(w.pawn(1)->pos == A);
  CHECK(w.pawn(1)->hp == 2);
  CHECK(w.pawn(2)->pos == R);
  CHECK(w.pawn(2)->hp == 2);
}

TEST_CASE("e03 blocked by a mountain: bump damages it") {
  World w;
  w.add(vek(1, A));
  make_mountain(w.tile(R));
  SkillEffect se;
  se.add_damage(push_sd(A, Dir::Right));
  w.fire(se);
  CHECK(w.pawn(1)->hp == 2);
  CHECK(w.tile(R).terrain == Terrain::Mountain);
  CHECK(w.tile(R).hp == 1);
}

TEST_CASE("e04 blocked by a building: grid loss") {
  World w;
  w.board.grid_power = 5;
  w.add(vek(1, A));
  Tile& b = w.tile(R);
  b.terrain = Terrain::Building;
  b.hp = b.max_hp = 1;
  b.populated = true;
  SkillEffect se;
  se.add_damage(push_sd(A, Dir::Right));
  w.fire(se);
  CHECK(w.pawn(1)->hp == 2);
  CHECK(w.tile(R).terrain == Terrain::Rubble);
  CHECK(w.board.grid_power == 4);
  REQUIRE(w.last.chances.size() == 1);
  CHECK(w.last.chances[0].kind == ChanceKind::GridDefense);
}

TEST_CASE("e05 a push off the board does nothing") {
  World w;
  w.add(vek(1, {7, 2}));
  SkillEffect se;
  se.add_damage(push_sd({7, 2}, Dir::Right));
  w.fire(se);
  CHECK(w.pawn(1)->pos == Point{7, 2});
  CHECK(w.pawn(1)->hp == 3);
  CHECK(w.frames(ResolveEventType::PushStarted).empty());
}

TEST_CASE("e06 pushed into water: drowns there, death processed after") {
  World w;
  w.add(vek(1, A));
  w.tile(R).terrain = Terrain::Water;
  SkillEffect se;
  se.add_damage(push_sd(A, Dir::Right));
  w.fire(se);
  const int64_t moved = w.frame_of(ResolveEventType::PushMoved, 1);
  CHECK(moved == 23);  // 24 tracker updates, the first on frame 0
  CHECK(w.frame_of(ResolveEventType::DeathProcessed, 1) == moved);
  bool killed_on_water = false;
  for (const RulesEvent& e : w.events) {
    if (e.type == RulesEventType::PawnKilled && e.uid == 1) killed_on_water = e.point == R;
  }
  CHECK(killed_on_water);
  CHECK(w.removed(1));
}

TEST_CASE("e07 pushed into a chasm: falls (alive, busy), then dies") {
  World w;
  w.add(vek(1, A));
  w.tile(R).terrain = Terrain::Hole;
  SkillEffect se;
  se.add_damage(push_sd(A, Dir::Right));
  w.fire(se);
  const int64_t moved = w.frame_of(ResolveEventType::PushMoved, 1);
  const int64_t died = w.frame_of(ResolveEventType::DeathProcessed, 1);
  CHECK(moved == 23);
  // The fall animation (0.4 s) runs first: the death comes 24 frames later.
  CHECK(died == moved + 24);
  CHECK(w.count(RulesEventType::PawnFell) == 1);
  CHECK(w.removed(1));
}

TEST_CASE("e08 a flyer pushed over water survives") {
  World w;
  Pawn f = vek(1, A);
  f.flying = true;
  w.add(f);
  w.tile(R).terrain = Terrain::Water;
  SkillEffect se;
  se.add_damage(push_sd(A, Dir::Right));
  w.fire(se);
  CHECK(w.pawn(1)->pos == R);
  CHECK(w.pawn(1)->alive());
}

TEST_CASE("e09 a massive unit pushed into water survives") {
  World w;
  Pawn m = vek(1, A);
  m.massive = true;
  w.add(m);
  w.tile(R).terrain = Terrain::Water;
  SkillEffect se;
  se.add_damage(push_sd(A, Dir::Right));
  w.fire(se);
  CHECK(w.pawn(1)->pos == R);
  CHECK(w.pawn(1)->alive());
}

TEST_CASE("e10 Pushable = false ignores pushes") {
  World w;
  Pawn s = vek(1, A);
  s.pushable = false;
  w.add(s);
  SkillEffect se;
  se.add_damage(push_sd(A, Dir::Right));
  w.fire(se);
  CHECK(w.pawn(1)->pos == A);
  CHECK(w.frames(ResolveEventType::PushStarted).empty());
}

TEST_CASE("e11 a shield absorbs the bump") {
  World w;
  Pawn s = vek(1, A);
  s.shield = true;
  w.add(s);
  make_mountain(w.tile(R));
  SkillEffect se;
  se.add_damage(push_sd(A, Dir::Right));
  w.fire(se);
  CHECK(w.pawn(1)->hp == 3);
  CHECK_FALSE(w.pawn(1)->shield);
  CHECK(w.tile(R).hp == 1);
}

TEST_CASE("e12 bumps ignore armor and ACID") {
  World w;
  Pawn a = vek(1, A);
  a.armor = true;
  a.acid = true;
  w.add(a);
  w.add(vek(2, R));
  SkillEffect se;
  se.add_damage(push_sd(A, Dir::Right));
  w.fire(se);
  CHECK(w.pawn(1)->hp == 2);
  CHECK(w.pawn(2)->hp == 2);
}

TEST_CASE("e13 Force Amp adds 1 to bumps on Vek") {
  World w;
  w.board.passives = kPassiveForceAmp;
  w.add(vek(1, A));
  w.add(vek(2, R));
  SkillEffect se;
  se.add_damage(push_sd(A, Dir::Right));
  w.fire(se);
  CHECK(w.pawn(1)->hp == 1);
  CHECK(w.pawn(2)->hp == 1);
}

TEST_CASE("e14 frozen absorbs the bump") {
  World w;
  Pawn f = vek(1, A);
  f.frozen = true;
  w.add(f);
  w.add(vek(2, R));
  SkillEffect se;
  se.add_damage(push_sd(A, Dir::Right));
  w.fire(se);
  CHECK_FALSE(w.pawn(1)->frozen);
  CHECK(w.pawn(1)->hp == 3);
  CHECK(w.pawn(2)->hp == 2);
}

TEST_CASE("e15 pushed onto a mine: dies in the finish frame") {
  World w;
  w.add(vek(1, A));
  w.tile(R).item = intern("Item_Mine");
  SkillEffect se;
  se.add_damage(push_sd(A, Dir::Right));
  w.fire(se);
  CHECK(w.tile(R).item == kNoSymbol);
  const int64_t moved = w.frame_of(ResolveEventType::PushMoved, 1);
  CHECK(moved >= 0);
  CHECK(w.frame_of(ResolveEventType::DeathProcessed, 1) == moved);
  CHECK(w.removed(1));
}

TEST_CASE("e16 Injured loses 1 HP when pushed") {
  World w;
  Pawn i = vek(1, A);
  i.injured = true;
  w.add(i);
  SkillEffect se;
  se.add_damage(push_sd(A, Dir::Right));
  w.fire(se);
  CHECK(w.pawn(1)->pos == R);
  CHECK(w.pawn(1)->hp == 2);
}

TEST_CASE("e17 same-frame pushes, leader first in the list: both move") {
  World w;
  w.add(vek(1, R));  // A
  w.add(vek(2, A));  // B
  SkillEffect se;
  se.add_damage(push_sd(R, Dir::Right));
  se.add_damage(push_sd(A, Dir::Right));
  w.fire(se);
  CHECK(w.pawn(1)->pos == Point{4, 2});
  CHECK(w.pawn(2)->pos == R);
  CHECK(w.pawn(1)->hp == 3);
  CHECK(w.pawn(2)->hp == 3);
}

TEST_CASE("e18 same-frame pushes, follower first: follower bumps, leader moves") {
  World w;
  w.add(vek(2, A));  // B first in the list
  w.add(vek(1, R));  // A
  SkillEffect se;
  se.add_damage(push_sd(R, Dir::Right));
  se.add_damage(push_sd(A, Dir::Right));
  w.fire(se);
  CHECK(w.pawn(2)->pos == A);
  CHECK(w.pawn(2)->hp == 2);
  CHECK(w.pawn(1)->pos == Point{4, 2});
  CHECK(w.pawn(1)->hp == 2);
}

TEST_CASE("e19 converging pushes: the first moves in, the second bumps it") {
  World w;
  w.add(vek(1, {1, 2}));
  w.add(vek(2, R));
  SkillEffect se;
  se.add_damage(push_sd({1, 2}, Dir::Right));
  se.add_damage(push_sd(R, Dir::Left));
  w.fire(se);
  CHECK(w.pawn(1)->pos == A);
  CHECK(w.pawn(1)->hp == 2);
  CHECK(w.pawn(2)->pos == R);
  CHECK(w.pawn(2)->hp == 2);
}

TEST_CASE("e20 converging on water: the drowned body blocks the second") {
  World w;
  w.add(vek(1, {1, 2}));
  w.add(vek(2, R));
  w.tile(A).terrain = Terrain::Water;
  SkillEffect se;
  se.add_damage(push_sd({1, 2}, Dir::Right));
  se.add_damage(push_sd(R, Dir::Left));
  w.fire(se);
  CHECK(w.removed(1));
  CHECK(w.pawn(2)->pos == R);
  CHECK(w.pawn(2)->hp == 2);
}

TEST_CASE("e21 chains don't transfer") {
  World w;
  w.add(vek(1, A));
  w.add(vek(2, R));
  w.add(vek(3, {4, 2}));
  SkillEffect se;
  se.add_damage(push_sd(A, Dir::Right));
  w.fire(se);
  CHECK(w.pawn(1)->hp == 2);
  CHECK(w.pawn(2)->hp == 2);
  CHECK(w.pawn(3)->hp == 3);
  CHECK(w.pawn(3)->pos == Point{4, 2});
}

TEST_CASE("e22 a killed ACID Vek slides, its pool lands where the body ended") {
  World w;
  Pawn a = vek(1, A, 1, 3);
  a.acid = true;
  w.add(a);
  SkillEffect se;
  se.add_damage(push_sd(A, Dir::Right, 1));
  w.fire(se);
  CHECK(w.tile(R).acid);
  CHECK_FALSE(w.tile(A).acid);
  CHECK(w.removed(1));
}

TEST_CASE("e23 a dead pawn's own bump is ignored, the obstacle is still hit") {
  World w;
  w.add(vek(1, A, 1, 3));
  w.add(vek(2, R));
  SkillEffect se;
  se.add_damage(push_sd(A, Dir::Right, 1));
  w.fire(se);
  CHECK(w.removed(1));
  CHECK(w.pawn(2)->hp == 2);
  CHECK(w.frame_of(ResolveEventType::PushBlocked, 1) >= 0);
}

TEST_CASE("e24 a body killed in the same chunk still blocks (dying < 50%)") {
  World w;
  w.add(vek(1, R, 1, 3));  // V
  w.add(vek(2, A));        // U
  SkillEffect se;
  se.add_damage(space_damage(R, 1));
  se.add_damage(push_sd(A, Dir::Right));
  w.fire(se);
  CHECK(w.removed(1));
  CHECK(w.pawn(2)->pos == A);
  CHECK(w.pawn(2)->hp == 2);
  CHECK_FALSE(w.last.timing_sensitive());
}

TEST_CASE("e25 an explosion animation outlasts half the death: the push goes through") {
  REQUIRE_GAME_DATA();
  World w;
  w.add(vek(1, R, 1, 3));
  w.add(vek(2, A));
  SkillEffect se;
  SpaceDamage hit = space_damage(R, 1);
  hit.animation = intern("ExploAir1");
  hit.delay = kFullDelay;
  se.add_damage(hit);
  se.add_damage(push_sd(A, Dir::Right));
  w.fire(se);
  CHECK(w.pawn(2)->pos == R);
  CHECK(w.pawn(2)->hp == 3);
}

TEST_CASE("e26 without an animation the FULL_DELAY chunk follows at once: blocked") {
  World w;
  w.add(vek(1, R, 1, 3));
  w.add(vek(2, A));
  SkillEffect se;
  SpaceDamage hit = space_damage(R, 1);
  hit.delay = kFullDelay;
  se.add_damage(hit);
  se.add_damage(push_sd(A, Dir::Right));
  w.fire(se);
  CHECK(w.pawn(2)->pos == A);
  CHECK(w.pawn(2)->hp == 2);
  // The chunk fired in the first frame's queue pass; the push it started
  // ticks from the next frame and is blocked 24 updates later.
  CHECK(w.frame_of(ResolveEventType::PushBlocked, 2) == 24);
}

// Tri-Rocket (Ranged_Crack): three pushing rockets 0.15 s apart, far tile first.
namespace {

SkillEffect tri_rocket(Point shooter, Point target, Dir dir) {
  SkillEffect se = effect_from(shooter);
  const Point v = dir_vector(dir);
  for (int i = 0; i < 3; ++i) {
    SpaceDamage d = space_damage(target + v * (1 - i), 1, dir);
    d.animation = intern("explopush1_" + std::to_string(static_cast<int>(dir)));
    se.add_artillery(d, "effects/shotup_tricrack.png", kNoDelay);
    se.add_delay(i < 2 ? 0.15f : 0.5f);
  }
  return se;
}

struct TriOutcome {
  bool mid_blocked;
  int mid_hp;
  bool sensitive;
};

TriOutcome tri_rocket_at(double fps) {
  World w(fps);
  w.add(vek(1, {2, 0}, 1, 3));  // far: dies to its rocket
  w.add(vek(2, {2, 1}));        // mid
  // (The near tile is left empty: a near Vek pushed into a blocked mid one
  // would finish it off.)
  w.add(mech(10, {2, 5}));
  WeaponInfo wi;
  wi.name = intern("Ranged_Crack");
  w.fire(tri_rocket({2, 5}, {2, 1}, Dir::Up), wi);
  const Pawn* mid = w.pawn(2);
  REQUIRE(mid);
  return {mid->pos == Point{2, 1}, mid->hp, w.last.timing_sensitive()};
}

}  // namespace

TEST_CASE("e27 Tri-Rocket: the dying-blocker threshold") {
  REQUIRE_GAME_DATA();
  // Spec D.6.1 (continuous time): the middle push lands ~0.55 s after the far
  // Vek died, against a ~0.56 s threshold. Frame-exact, the death animation
  // is slower than 8 x 0.14 s (each animation frame runs whole update
  // steps), so the push is blocked (rocket + bump: HP 1) with a margin of
  // frames at 30, 60, 120 and 144 fps; see the report for 50-60 fps.
  for (double fps : {30.0, 60.0, 120.0, 144.0}) {
    CAPTURE(fps);
    const TriOutcome o = tri_rocket_at(fps);
    CHECK(o.mid_blocked);
    CHECK(o.mid_hp == 1);
    CHECK_FALSE(o.sensitive);
  }
  // Where the push lands within a frame of the 50% mark the outcome is
  // flagged; at 35 fps it even goes through.
  const TriOutcome at34 = tri_rocket_at(34.0);
  CHECK(at34.mid_blocked);
  CHECK(at34.sensitive);
  const TriOutcome at35 = tri_rocket_at(35.0);
  CHECK_FALSE(at35.mid_blocked);
  CHECK(at35.mid_hp == 2);
  CHECK(at35.sensitive);
}

// ---- Stage 4 vectors: projectiles and the queue ------------------------------------------

TEST_CASE("e28 projectile impact on update 16 at distance 3") {
  World w;
  w.add(mech(10, {2, 6}));
  w.add(vek(1, {2, 3}));
  SkillEffect se = effect_from({2, 6});
  se.add_projectile(push_sd({2, 3}, Dir::Up, 1), "effects/shot_mechtank", kNoDelay);
  w.fire(se);
  CHECK(w.frame_of(ResolveEventType::Impact) == 15);
  CHECK(w.pawn(1)->pos == A);
  CHECK(w.pawn(1)->hp == 2);
}

TEST_CASE("e29 impacts land in distance order") {
  World w;
  w.add(mech(10, {3, 3}));
  w.add(vek(1, {3, 1}));
  w.add(vek(2, {3, 6}));
  SkillEffect se = effect_from({3, 3});
  se.add_projectile(push_sd({3, 1}, Dir::Up, 1), "effects/shot_mechtank", kNoDelay);
  se.add_projectile(push_sd({3, 6}, Dir::Down, 1), "effects/shot_mechtank");
  w.fire(se);
  const auto impacts = w.frames(ResolveEventType::Impact);
  REQUIRE(impacts.size() == 2);
  CHECK(impacts[0] == 9);   // distance 2: about 0.18 s
  CHECK(impacts[1] == 15);  // distance 3: about 0.27 s
  CHECK(w.pawn(1)->pos == Point{3, 0});
  CHECK(w.pawn(1)->hp == 2);
  CHECK(w.pawn(2)->pos == Point{3, 7});
  CHECK(w.pawn(2)->hp == 2);
}

namespace {

SkillEffect artemis(Point shooter, Point center) {
  SkillEffect se = effect_from(shooter);
  se.add_artillery(space_damage(center, 1), "effects/shotup_tribomb_missile.png");
  for (int d = 0; d < 4; ++d) {
    const Dir dir = static_cast<Dir>(d);
    se.add_damage(push_sd(step(center, dir), dir));
  }
  return se;
}

}  // namespace

TEST_CASE("e30 Artemis: the side pushes start in the impact frame") {
  World w;
  w.add(mech(10, {2, 6}));
  w.add(vek(1, A, 2, 2));
  w.add(vek(2, {2, 1}));
  w.add(vek(3, R));
  w.add(vek(4, {2, 3}));
  w.add(vek(5, {1, 2}));
  w.fire(artemis({2, 6}, A));
  const int64_t impact = w.frame_of(ResolveEventType::Impact);
  CHECK(impact == 47);
  for (int32_t uid : {2, 3, 4, 5}) CHECK(w.frame_of(ResolveEventType::PushStarted, uid) == impact);
  CHECK(w.pawn(1)->hp == 1);
  CHECK(w.pawn(2)->pos == Point{2, 0});
  CHECK(w.pawn(3)->pos == Point{4, 2});
  CHECK(w.pawn(4)->pos == Point{2, 4});
  CHECK(w.pawn(5)->pos == Point{0, 2});
}

TEST_CASE("e31 Artemis on an ACID centre: the pool waits for the side pushes") {
  World w;
  w.add(mech(10, {2, 6}));
  Pawn v = vek(1, A, 1, 2);
  v.acid = true;
  w.add(v);
  w.add(vek(2, {2, 1}));
  w.add(vek(3, R));
  w.add(vek(4, {2, 3}));
  w.add(vek(5, {1, 2}));
  w.fire(artemis({2, 6}, A));
  CHECK(w.removed(1));
  CHECK(w.tile(A).acid);
  CHECK(w.pawn(3)->pos == Point{4, 2});
  // The death effect is stacked in the impact frame, behind the PROJ_DELAY
  // remainder, and its pool lands once the pushes are over.
  const auto chunks = w.frames(ResolveEventType::ChunkApplied);
  REQUIRE(chunks.size() == 3);
  CHECK(w.frame_of(ResolveEventType::DeathEffect, 1) == chunks[1]);
  CHECK(chunks[2] >= w.frame_of(ResolveEventType::PushMoved, 2));
}

TEST_CASE("e32 an impact hits whoever is on the tile when it lands") {
  World w;
  w.add(vek(1, {4, 4}));
  SkillEffect se = effect_from({1, 4});
  SpaceDamage push = push_sd({4, 4}, Dir::Right);
  push.delay = 0.2f;
  se.add_damage(push);
  se.add_artillery(space_damage({4, 4}, 2), "effects/shotup_ignite_fireball.png");
  w.fire(se);
  CHECK(w.pawn(1)->pos == Point{5, 4});
  CHECK(w.pawn(1)->hp == 3);
  CHECK(w.frame_of(ResolveEventType::Impact) > w.frame_of(ResolveEventType::PushMoved, 1));
}

namespace {

// Laser_Base:AddLaser from `start` going `dir` (FriendlyDamage, MinDamage 1).
SkillEffect laser(Point shooter, Dir dir, int damage) {
  SkillEffect se = effect_from(shooter);
  Point p = step(shooter, dir);
  while (p.valid()) {
    SpaceDamage d = space_damage(p, damage);
    if (!step(p, dir).valid()) {
      se.add_projectile(shooter, d, "effects/laser1", kFullDelay);
      break;
    }
    se.add_damage(d);
    damage = std::max(damage - 1, 1);
    p = step(p, dir);
  }
  return se;
}

}  // namespace

TEST_CASE("e33 lasers hit at once and keep the board busy 0.5 s") {
  World w;
  w.add(mech(10, {0, 4}));
  w.add(vek(1, {1, 4}));
  w.add(vek(2, {2, 4}));
  w.add(vek(3, {3, 4}));
  SkillEffect se = laser({0, 4}, Dir::Right, 3);
  // A FULL_DELAY marker after the beam.
  se.add_delay(kFullDelay);
  se.add_damage(space_damage({6, 6}, 1));
  w.fire(se);
  CHECK(w.frame_of(ResolveEventType::DeathProcessed, 1) == 0);
  CHECK(w.removed(1));
  CHECK(w.pawn(2)->hp == 1);
  CHECK(w.pawn(3)->hp == 2);
  const auto chunks = w.frames(ResolveEventType::ChunkApplied);
  REQUIRE(chunks.size() == 2);
  CHECK(chunks[1] == FrameClock::steady(60.0).tracker_updates(0.5f) - 1);
}

TEST_CASE("e34 a slow projectile lands after the push moved its target") {
  World w;
  w.add(vek(1, {2, 0}));
  w.add(mech(10, {2, 7}));
  SkillEffect se = effect_from({2, 7});
  se.add_damage(push_sd({2, 0}, Dir::Right));
  se.add_projectile(space_damage({2, 0}, 1), "effects/shot_mechtank", kNoDelay);
  w.fire(se);
  CHECK(w.pawn(1)->pos == Point{3, 0});
  CHECK(w.pawn(1)->hp == 3);
  CHECK(w.frame_of(ResolveEventType::Impact) == 36);
}

TEST_CASE("e35 melee: the hit lands after the lunge, the next chunk waits for the push") {
  World w;
  w.add(mech(10, {2, 3}));
  w.add(vek(1, A));
  SkillEffect se = effect_from({2, 3});
  se.add_melee({2, 3}, push_sd(A, Dir::Up, 2));
  se.add_damage(space_damage({2, 1}, 1));
  w.fire(se);
  CHECK(w.frame_of(ResolveEventType::LungeHit, 10) == 9);  // 0.15 s
  // The hit lands in the mech's pawn update; the Vek comes later in the list,
  // so its push ticks in that same frame.
  CHECK(w.frame_of(ResolveEventType::PushMoved, 1) == 9 + 23);
  CHECK(w.removed(1));
}

TEST_CASE("e36 melee with nobody at the path start applies at once") {
  World w;
  w.add(vek(1, A));
  SkillEffect se = effect_from({2, 3});
  se.add_melee({2, 3}, space_damage(A, 2));
  w.fire(se);
  CHECK(w.pawn(1)->hp == 1);
  CHECK(w.frames(ResolveEventType::LungeHit).empty());
}

TEST_CASE("e37 queued melee with a positive delay (Hornet)") {
  World w;
  w.board.player_phase = false;
  Pawn h = vek(1, {2, 3});
  h.flying = true;
  w.add(h);
  w.add(mech(10, A));
  w.add(vek(2, {2, 1}));
  SkillEffect se = effect_from({2, 3});
  se.owner = 1;
  se.add_queued_melee({2, 3}, space_damage(A, 1), 0.25f);
  se.q_effect.push_back(space_damage({2, 1}, 1));
  WeaponInfo wi;
  wi.queued = true;
  w.fire(se, wi);
  const int64_t lunge = w.frame_of(ResolveEventType::LungeHit, 1);
  CHECK(lunge == 9);
  const auto chunks = w.frames(ResolveEventType::ChunkApplied);
  REQUIRE(chunks.size() == 2);
  CHECK(chunks[1] == FrameClock::steady(60.0).delay_updates(0.25f) - 1);
  CHECK(chunks[1] > lunge);
  CHECK(w.pawn(10)->hp == 2);
  CHECK(w.pawn(2)->hp == 2);
}

TEST_CASE("e38 PROJ_DELAY doesn't wait for pushes: the hit finds the pawn still there") {
  World w;
  w.add(vek(1, A));
  SkillEffect se;
  SpaceDamage push = push_sd(A, Dir::Right);
  push.delay = kProjDelay;
  se.add_damage(push);
  se.add_damage(space_damage(A, 1));
  w.fire(se);
  CHECK(w.pawn(1)->pos == R);
  CHECK(w.pawn(1)->hp == 2);
}

TEST_CASE("e39 FULL_DELAY (or +0.5) waits: the hit lands on the empty tile") {
  for (float delay : {kFullDelay, 0.5f}) {
    CAPTURE(delay);
    World w;
    w.add(vek(1, A));
    SkillEffect se;
    SpaceDamage push = push_sd(A, Dir::Right);
    push.delay = delay;
    se.add_damage(push);
    se.add_damage(space_damage(A, 1));
    w.fire(se);
    CHECK(w.pawn(1)->pos == R);
    CHECK(w.pawn(1)->hp == 3);
  }
}

TEST_CASE("e40 a positive delay lets the death effect run first (Fast Decay)") {
  World w;
  w.board.passives = kPassiveFastDecay;
  const Point p{3, 3};
  w.add(vek(1, p, 1, 3));
  SkillEffect se;
  SpaceDamage hit = space_damage(p, 1);
  hit.delay = 0.3f;
  se.add_damage(hit);
  SpaceDamage fire = space_damage(p);
  fire.fire = StatusChange::Apply;
  se.add_damage(fire);
  w.fire(se);
  CHECK(w.tile(p).terrain == Terrain::Road);
  CHECK(w.tile(p).fire == FireState::BurningForest);
}

TEST_CASE("e41 a FULL_DELAY remainder runs before the death effect (Fast Decay)") {
  // The fire lands first, then the death effect turns the tile into an
  // unlit forest. The spec stops there, but the fire also set the body on
  // fire (iFire ignites every occupant, dead or alive), and a burning
  // occupant relights a forest in the next tile pass: the end state is the
  // same burning road as e40.
  World w;
  w.board.passives = kPassiveFastDecay;
  const Point p{3, 3};
  w.add(vek(1, p, 1, 3));
  SkillEffect se;
  SpaceDamage hit = space_damage(p, 1);
  hit.delay = kFullDelay;
  se.add_damage(hit);
  SpaceDamage fire = space_damage(p);
  fire.fire = StatusChange::Apply;
  se.add_damage(fire);
  w.fire(se);
  std::vector<int> terrains;
  for (const RulesEvent& e : w.events) {
    if (e.type == RulesEventType::TerrainChanged) terrains.push_back(e.amount);
  }
  CHECK(terrains == std::vector<int>{static_cast<int>(Terrain::Forest), static_cast<int>(Terrain::Road)});
  const auto chunks = w.frames(ResolveEventType::ChunkApplied);
  REQUIRE(chunks.size() == 3);
  CHECK(chunks[1] == 0);  // the fire, in the first queue pass
  CHECK(chunks[2] == 1);  // the death effect, next frame
  CHECK(w.tile(p).terrain == Terrain::Road);
  CHECK(w.tile(p).fire == FireState::BurningForest);
}

TEST_CASE("e42 Vek Hormones: enemy shots hurt Vek more") {
  World w;
  w.board.passives = kPassiveFriendlyFire;
  w.add(vek(1, {5, 5}));  // shooter
  w.add(vek(2, A));
  w.add(mech(10, {3, 3}));
  SkillEffect se = effect_from({5, 5});
  se.owner = 1;
  se.add_damage(space_damage(A, 1));
  se.add_damage(space_damage({3, 3}, 1));
  w.fire(se);
  CHECK(w.pawn(2)->hp == 1);
  CHECK(w.pawn(10)->hp == 2);
}

TEST_CASE("e43 Hormones AB, then Boost") {
  World w;
  w.board.passives = kPassiveFriendlyFireAB;
  Pawn s = vek(1, {5, 5});
  s.boosted = true;
  w.add(s);
  w.add(vek(2, A, 6, 6));
  SkillEffect se = effect_from({5, 5});
  se.owner = 1;
  se.add_damage(space_damage(A, 1));
  w.fire(se);
  CHECK(w.pawn(2)->hp == 1);
  CHECK_FALSE(w.pawn(1)->boosted);  // spent by firing
}

TEST_CASE("e44 Boost on each kind of damage") {
  Board b;
  Pawn m = mech(10, A);
  m.boosted = true;
  b.add_pawn(m);
  std::vector<SpaceDamage> list{space_damage({1, 1}, 2), space_damage({1, 1}, -1),
                                space_damage({1, 1}, kDamageDeath), space_damage({1, 1}, kDamageZero)};
  check_alterations(b, list, b.find_pawn(10), /*move_skill=*/false);
  CHECK(list[0].damage == 3);
  CHECK(list[1].damage == -2);
  CHECK(list[2].damage == kDamageDeath);
  CHECK(list[3].damage == kDamageZero);
  // The Move skill is never boosted.
  std::vector<SpaceDamage> move{space_damage({1, 1}, 2)};
  check_alterations(b, move, b.find_pawn(10), /*move_skill=*/true);
  CHECK(move[0].damage == 2);
}

TEST_CASE("e45 queued shots get Hormones from the board at fire time") {
  World w;
  w.board.passives = kPassiveFriendlyFire;
  w.board.player_phase = false;
  w.add(vek(1, {4, 4}));      // Firefly
  w.add(mech(10, {4, 1}));
  w.add(vek(2, {4, 2}));      // pushed into the line before it fires
  // GetSkillEffect recomputed at fire time: the shot now stops at B.
  SkillEffect se = effect_from({4, 4});
  se.owner = 1;
  se.add_queued_projectile(space_damage({4, 2}, 1), "effects/shot_firefly");
  WeaponInfo wi;
  wi.queued = true;
  w.fire(se, wi);
  CHECK(w.pawn(2)->hp == 1);
  CHECK(w.pawn(10)->hp == 3);
}

TEST_CASE("e46 DIR_FLIP mirrors the queued attack") {
  World w;
  Pawn f = vek(1, {3, 3});
  f.queued = QueuedShot{0, {3, 3}, {3, 1}};
  w.add(f);
  SkillEffect se;
  se.add_damage(push_sd({3, 3}, Dir::Flip));
  w.fire(se);
  CHECK(w.pawn(1)->queued.target == Point{3, 5});
  CHECK(w.pawn(1)->pos == Point{3, 3});
}

TEST_CASE("e47 a push carries the queued attack, then the flip mirrors it") {
  World w;
  Pawn f = vek(1, {3, 3});
  f.queued = QueuedShot{0, {3, 3}, {3, 1}};
  w.add(f);
  SkillEffect push;
  push.add_damage(push_sd({3, 3}, Dir::Right));
  SkillEffect flip;
  flip.add_damage(push_sd({4, 3}, Dir::Flip));
  resolve_effects(w.board, {{push, {}}, {flip, {}}}, w.ctx);
  CHECK(w.pawn(1)->queued.origin == Point{4, 3});
  CHECK(w.pawn(1)->queued.target == Point{4, 5});
}

TEST_CASE("e48 IgnoreFlip pawns keep their attack") {
  World w;
  Pawn f = vek(1, {3, 3});
  f.queued = QueuedShot{0, {3, 3}, {3, 1}};
  f.ignore_flip = true;
  w.add(f);
  SkillEffect se;
  se.add_damage(push_sd({3, 3}, Dir::Flip));
  w.fire(se);
  CHECK(w.pawn(1)->queued.target == Point{3, 1});
}

TEST_CASE("e49 pushed onto smoke: the queued attack is cancelled in the finish frame") {
  World w;
  Pawn a = vek(1, A);
  a.queued = QueuedShot{0, A, {2, 4}};
  w.add(a);
  w.tile(R).smoke = true;
  SkillEffect se;
  se.add_damage(push_sd(A, Dir::Right));
  w.fire(se);
  CHECK(w.pawn(1)->pos == R);
  CHECK_FALSE(w.pawn(1)->queued.active());
}

namespace {

// Spec vector 50's board: Blast Psion at (6,6), V on forest (3,3), mountain
// (3,2), armored+ACID mech (4,3), Vek W (2,3).
World blast_board(bool kill_psion) {
  World w;
  Pawn psion = vek(9, {6, 6}, kill_psion ? 1 : 2, 2);
  psion.type = intern("Test_Jelly");
  psion.flying = true;
  psion.leader = Leader::Explode;
  w.add(psion);
  w.board.psion = Leader::Explode;
  w.tile({3, 3}).terrain = Terrain::Forest;
  w.add(vek(1, {3, 3}, 1, 3));
  make_mountain(w.tile({3, 2}));
  Pawn m = mech(10, {4, 3});
  m.armor = true;
  m.acid = true;
  w.add(m);
  w.add(vek(2, {2, 3}));
  SkillEffect se;
  se.add_damage(push_sd({3, 3}, Dir::Up));
  if (kill_psion) se.add_damage(space_damage({6, 6}, 1));
  w.fire(se);
  return w;
}

}  // namespace

TEST_CASE("e50 Blast Psion corpse explosion: explosion mode") {
  World w = blast_board(false);
  CHECK(w.frame_of(ResolveEventType::CorpseExploded, 1) >= 0);
  CHECK(w.tile({3, 2}).terrain == Terrain::Rubble);  // bump, then the blast
  CHECK(w.pawn(10)->hp == 2);  // no armor, no ACID doubling
  CHECK(w.pawn(2)->hp == 2);
  CHECK(w.tile({3, 3}).terrain == Terrain::Road);  // the blast ignites the forest
  CHECK(w.tile({3, 3}).on_fire());
}

TEST_CASE("e51 killing the Blast Psion in the same effect: no explosion") {
  World w = blast_board(true);
  CHECK(w.frames(ResolveEventType::CorpseExploded).empty());
  CHECK(w.tile({3, 2}).terrain == Terrain::Mountain);
  CHECK(w.tile({3, 2}).hp == 1);
  CHECK(w.pawn(10)->hp == 3);
  CHECK(w.pawn(2)->hp == 3);
  CHECK(w.board.psion == Leader::None);
  // The body keeps waiting for an explosion that never comes.
  REQUIRE(w.pawn(1) != nullptr);
  CHECK_FALSE(w.pawn(1)->alive());
}

TEST_CASE("e52 two exploding corpses go off in list order") {
  World w;
  Pawn psion = vek(9, {6, 6});
  psion.leader = Leader::Explode;
  psion.flying = true;
  w.add(psion);
  w.board.psion = Leader::Explode;
  w.add(vek(1, {3, 3}, 1, 3));
  w.add(vek(2, {4, 3}, 1, 3));
  w.add(vek(3, {5, 3}, 3, 3));
  SkillEffect se;
  se.add_damage(space_damage({3, 3}, 1));
  se.add_damage(space_damage({4, 3}, 1));
  w.fire(se);
  const int64_t first = w.frame_of(ResolveEventType::CorpseExploded, 1);
  const int64_t second = w.frame_of(ResolveEventType::CorpseExploded, 2);
  REQUIRE(first >= 0);
  REQUIRE(second >= 0);
  CHECK(first == second);  // both stacked in the same quiescent frame, V1 first
  std::vector<int32_t> order;
  for (const ResolveEvent& e : w.log) {
    if (e.type == ResolveEventType::CorpseExploded) order.push_back(e.uid);
  }
  CHECK(order == std::vector<int32_t>{1, 2});
  CHECK(w.pawn(3)->hp == 2);  // only V2's blast reaches it
}

TEST_CASE("e53 an ACID Vek leaves a pool where it dies") {
  World w;
  Pawn v = vek(1, {3, 3}, 1, 3);
  v.acid = true;
  w.add(v);
  SkillEffect se;
  se.add_damage(space_damage({3, 3}, 1));
  w.fire(se);
  CHECK(w.tile({3, 3}).acid);
}

TEST_CASE("EVENT_ACID_DESTROYED: the dying pawn's ACID as its death is processed") {
  World w;
  w.add(vek(1, A, 1, 3));      // gets ACID and dies in the same effect
  w.add(vek(2, R, 1, 3));      // no ACID
  Pawn old = vek(3, {5, 5}, 1, 3);
  old.acid = true;             // ACID since an earlier turn
  w.add(old);
  Pawn m = mech(4, {6, 6}, 1);
  m.acid = true;               // mechs never count
  w.add(m);
  Pawn egg = vek(5, {1, 5}, 1, 1);
  egg.minor = true;            // minor pawns do (the event comes first)
  egg.acid = true;
  w.add(egg);
  const Board start = w.board;
  TurnContext tc;
  tc.mission.mission_id = "Mission_AcidTank";
  auto acid_kills = [&] { return evaluate_objectives(start, w.board, &tc, nullptr).acid_kills; };

  SkillEffect same;
  SpaceDamage acid = space_damage(A);
  acid.acid = StatusChange::Apply;
  same.add_damage(acid);
  same.add_damage(space_damage(A, 1));
  w.fire(same);
  CHECK(w.removed(1));  // gone from the board, ACID and all
  CHECK(w.board.acid_destroyed == 1);
  CHECK(acid_kills() == 1);

  SkillEffect plain;
  plain.add_damage(space_damage(R, 1));
  w.fire(plain);
  CHECK(w.removed(2));
  CHECK(acid_kills() == 1);

  SkillEffect rest;
  rest.add_damage(space_damage({5, 5}, 1));
  rest.add_damage(space_damage({6, 6}, 1));
  rest.add_damage(space_damage({1, 5}, 1));
  w.fire(rest);
  CHECK_FALSE(w.pawn(4)->alive());
  CHECK(w.removed(3));
  CHECK(w.removed(5));
  CHECK(acid_kills() == 3);  // vek 1, the old ACID Vek, the minor one
}

TEST_CASE("e54 mech corpses block pushes and stay") {
  World w;
  Pawn c = mech(10, R);
  c.hp = 0;
  w.add(c);
  w.add(vek(1, A));
  SkillEffect se;
  se.add_damage(push_sd(A, Dir::Right));
  w.fire(se);
  CHECK(w.pawn(1)->hp == 2);
  CHECK(w.pawn(1)->pos == A);
  REQUIRE(w.pawn(10));
  CHECK(w.pawn(10)->pos == R);
  CHECK(w.pawn(10)->hp == 0);
}

TEST_CASE("e55 a second push before the first lands replaces it") {
  World w;
  w.add(vek(1, A));
  SkillEffect se;
  se.add_damage(push_sd(A, Dir::Right));
  se.add_damage(push_sd(A, Dir::Up));
  w.fire(se);
  CHECK(w.pawn(1)->pos == Point{2, 1});
  CHECK(w.frames(ResolveEventType::PushMoved).size() == 1);
}

TEST_CASE("e56 a second push off the board leaves the first alone") {
  World w;
  w.add(vek(1, {2, 0}));
  SkillEffect se;
  se.add_damage(push_sd({2, 0}, Dir::Right));
  se.add_damage(push_sd({2, 0}, Dir::Up));
  w.fire(se);
  CHECK(w.pawn(1)->pos == Point{3, 0});
}

TEST_CASE("e57 Aerial Bombs: the leaper is on its landing tile from the start") {
  World w;
  w.add(mech(10, {2, 6}));
  w.add(vek(1, {2, 5}));
  w.add(vek(2, {2, 4}));
  Point seen = kInvalidPoint;
  w.ctx.run_script = [&](Resolver& r, const std::string&, Point) { seen = r.board().find_pawn(10)->pos; };
  SkillEffect se = effect_from({2, 6});
  se.add_leap({{2, 6}, {2, 3}}, kNoDelay);
  se.add_script("check");
  se.add_delay(0.25f);
  se.add_damage(space_damage({2, 5}, 1));
  se.add_delay(0.2f);
  se.add_damage(space_damage({2, 4}, 1));
  w.fire(se);
  CHECK(seen == Point{2, 3});
  CHECK(w.pawn(10)->pos == Point{2, 3});
  CHECK(w.pawn(1)->hp == 2);
  CHECK(w.pawn(2)->hp == 2);
}

TEST_CASE("e58 a charge relocates at once; the push comes after the delays") {
  World w;
  w.add(mech(10, {2, 6}));
  w.add(vek(1, {2, 2}));
  SkillEffect se = effect_from({2, 6});
  se.add_charge({{2, 6}, {2, 3}}, kNoDelay);
  se.add_delay(0.06f);
  se.add_damage(push_sd({2, 2}, Dir::Up));
  w.fire(se);
  CHECK(w.pawn(10)->pos == Point{2, 3});
  CHECK(w.pawn(1)->pos == Point{2, 1});
}

TEST_CASE("e59 a grapple can't pull a stable unit") {
  World w;
  w.add(mech(10, {2, 6}));
  Pawn s = vek(1, {2, 2});
  s.pushable = false;
  w.add(s);
  SkillEffect se = effect_from({2, 6});
  se.add_charge({{2, 2}, {2, 5}}, kNoDelay);
  w.fire(se);
  CHECK(w.pawn(1)->pos == Point{2, 2});
}

TEST_CASE("e60 Swap: teleports ignore Pushable") {
  World w;
  w.add(mech(10, A));
  Pawn s = vek(1, {4, 4});
  s.pushable = false;
  w.add(s);
  SkillEffect se = effect_from(A);
  se.add_teleport(A, {4, 4});
  se.add_teleport({4, 4}, A);
  w.fire(se);
  CHECK(w.pawn(10)->pos == Point{4, 4});
  CHECK(w.pawn(1)->pos == A);
}

TEST_CASE("e61 enemy phase: FULL_DELAY waits for the whole death animation") {
  World w;
  w.board.player_phase = false;
  w.add(vek(1, R, 1, 3));
  w.add(vek(2, A));
  SkillEffect se;
  SpaceDamage hit = space_damage(R, 1);
  hit.delay = kFullDelay;
  se.q_effect.push_back(hit);
  se.q_effect.push_back(push_sd(A, Dir::Right));
  WeaponInfo wi;
  wi.queued = true;
  w.fire(se, wi);
  CHECK(w.pawn(2)->pos == R);
  CHECK(w.pawn(2)->hp == 3);
  // The second chunk waited for the 72-update death animation.
  const auto chunks = w.frames(ResolveEventType::ChunkApplied);
  REQUIRE(chunks.size() == 2);
  CHECK(chunks[1] == 72);
}

TEST_CASE("e62 artillery shells land together whatever their distance") {
  World w;
  w.add(mech(10, {0, 0}));
  SkillEffect se = effect_from({0, 0});
  se.add_artillery(space_damage({0, 1}, 1), "effects/shotup_tank", kNoDelay);
  se.add_artillery(space_damage({0, 6}, 1), "effects/shotup_tank", kNoDelay);
  w.fire(se);
  const auto impacts = w.frames(ResolveEventType::Impact);
  REQUIRE(impacts.size() == 2);
  CHECK(impacts[0] == impacts[1]);
}

TEST_CASE("e63 Push Beam: far to near, 0.1 s apart") {
  World w;
  w.add(mech(10, A));
  w.add(vek(1, R));
  w.add(vek(2, {4, 2}));
  make_mountain(w.tile({5, 2}));
  // Science_PushBeam: the beam to the mountain, then the pawns far to near.
  SkillEffect se = effect_from(A);
  se.add_projectile(space_damage({5, 2}), "effects/laser_push");
  se.add_delay(0.1f);
  se.add_damage(push_sd({4, 2}, Dir::Right));
  se.add_delay(0.1f);
  se.add_damage(push_sd(R, Dir::Right));
  w.fire(se);
  CHECK(w.tile({5, 2}).hp == 1);
  CHECK(w.pawn(2)->pos == Point{4, 2});
  CHECK(w.pawn(2)->hp == 1);
  CHECK(w.pawn(1)->pos == R);
  CHECK(w.pawn(1)->hp == 2);
}

// ---- Beyond the vectors ----------------------------------------------------------------

TEST_CASE("resolve_effects keeps one clock across effects") {
  World w;
  w.add(vek(1, A));
  SkillEffect a;
  a.add_damage(push_sd(A, Dir::Right));
  SkillEffect b;
  b.add_damage(push_sd(R, Dir::Right));
  const ResolveResult r = resolve_effects(w.board, {{a, {}}, {b, {}}}, w.ctx);
  CHECK(r.quiescent);
  CHECK(w.pawn(1)->pos == Point{4, 2});
  const auto moves = w.frames(ResolveEventType::PushMoved, 1);
  REQUIRE(moves.size() == 2);
  // The second effect fires once the board went idle and then had a frame
  // in which nothing changed.
  CHECK(moves[1] == moves[0] + 25);
}

TEST_CASE("a frame-rate change shifts timings but not a robust outcome") {
  for (double fps : {30.0, 60.0, 144.0}) {
    CAPTURE(fps);
    World w(fps);
    w.add(vek(1, A));
    w.add(vek(2, R));
    SkillEffect se;
    se.add_damage(push_sd(A, Dir::Right));
    w.fire(se);
    CHECK(w.pawn(1)->hp == 2);
    CHECK_FALSE(w.last.timing_sensitive());
  }
}

TEST_CASE("prepare_effect fills animations, sources and teams") {
  SkillEffect se;
  se.add_damage(space_damage(A, 1));
  SpaceDamage own = space_damage(R, 1);
  own.animation = intern("ExploAir2");
  se.add_damage(own);
  se.add_projectile(space_damage({5, 2}, 1), "effects/shot_mechtank");
  prepare_effect(se, {0, 2}, {5, 2}, Team::Player, intern("ExploAir1"));
  CHECK(se.effect[0].animation == intern("ExploAir1"));
  CHECK(se.effect[1].animation == intern("ExploAir2"));
  CHECK(se.effect[2].projectile_source == Point{0, 2});
  CHECK(se.effect[2].owner_team == Team::Player);
  CHECK(se.team == Team::Player);
}

TEST_CASE("an empty sAnimation never keeps the tile busy; ANIM_NO_DELAY neither") {
  REQUIRE_GAME_DATA();
  World w;
  w.add(vek(1, R, 1, 3));
  w.add(vek(2, A));
  SkillEffect se;
  SpaceDamage hit = space_damage(R, 1);
  hit.animation = intern("ExploAir1");
  hit.anim_flags = kAnimNoDelay;
  hit.delay = kFullDelay;
  se.add_damage(hit);
  se.add_damage(push_sd(A, Dir::Right));
  w.fire(se);
  CHECK(w.pawn(2)->pos == A);  // as e26: the push follows at once and is blocked
}

TEST_CASE("Board:AddEffect from a script appends a FULL_DELAY effect") {
  World w;
  w.add(vek(1, A));
  bool added = false;
  w.ctx.run_script = [&](Resolver& r, const std::string&, Point) {
    if (added) return;
    added = true;
    SkillEffect extra;
    extra.add_damage(space_damage(A, 1));
    r.add_effect(extra);
  };
  SkillEffect se;
  se.add_script("Board:AddEffect(...)");
  se.add_damage(push_sd(A, Dir::Right));
  w.fire(se);
  // The appended hit waited for the push: it found an empty tile.
  CHECK(w.pawn(1)->pos == R);
  CHECK(w.pawn(1)->hp == 3);
}

TEST_CASE("deferred chasm: the tile stays busy until it opens") {
  World w;
  w.add(vek(1, R));
  SkillEffect se;
  SpaceDamage hole = space_damage(R);
  hole.terrain = static_cast<int>(Terrain::Hole);
  hole.delay = kFullDelay;
  se.add_damage(hole);
  se.add_damage(space_damage({6, 6}, 1));
  w.fire(se);
  CHECK(w.tile(R).terrain == Terrain::Hole);
  CHECK(w.removed(1));
  // The chasm opens when its eased 0.5 s bounce ends; its pawn is killed
  // and falls (busy 0.4 s), and only then does FULL_DELAY let go.
  const FrameClock c = FrameClock::steady(60.0);
  const int64_t opened = c.eased_tracker_updates(0.5f, 0.15f, 0.15f) - 1;
  CHECK(w.frame_of(ResolveEventType::DeathProcessed, 1) == opened);
  const auto chunks = w.frames(ResolveEventType::ChunkApplied);
  REQUIRE(chunks.size() == 2);
  // (The fall starts in the tile pass, so its tracker ticks that same frame.)
  CHECK(chunks[1] == opened + c.tracker_updates(0.4f) - 1);
}

TEST_CASE("walks step tile by tile; the final tile's dangers apply") {
  World w;
  w.add(mech(10, {1, 1}));
  w.tile({1, 3}).terrain = Terrain::Water;
  SkillEffect se = effect_from({1, 1});
  se.add_move({{1, 1}, {1, 2}, {1, 3}});
  w.fire(se);
  REQUIRE(w.pawn(10));
  CHECK(w.pawn(10)->pos == Point{1, 3});
  CHECK_FALSE(w.pawn(10)->alive());  // drowned (a mech corpse in water)
}

TEST_CASE("the tile occupant order follows arrival") {
  Board b;
  Pawn body = vek(1, R, 0, 3);
  b.add_pawn(body);
  b.add_pawn(mech(10, A));  // player units come first in the list
  Pawn& m = *b.find_pawn(10);
  set_space(b, m, R);
  // The body arrived first: it is the first occupant, the mech the last.
  const std::vector<Pawn*> occ = b.pawns_at(R);
  REQUIRE(occ.size() == 2);
  CHECK(occ[0]->uid == 1);
  CHECK(occ[1]->uid == 10);
  CHECK(b.pawn_at(R)->uid == 1);
}

TEST_CASE("timing: a delay ending with a push on its tiles is flagged") {
  World w;
  w.add(vek(1, A));
  SkillEffect se;
  se.add_damage(push_sd(A, Dir::Right));
  se.add_delay(0.4f);
  se.add_damage(space_damage(R, 1));
  w.fire(se);
  // At 60 fps the push lands first and the hit finds the Vek; a frame later
  // or earlier it could miss.
  CHECK(w.pawn(1)->pos == R);
  CHECK(w.pawn(1)->hp == 2);
  REQUIRE(w.last.timing.size() == 1);
  CHECK(w.last.timing[0].kind == TimingKind::DelayVsPush);
  CHECK(w.last.timing[0].point == R);
  CHECK(w.frames(ResolveEventType::TimingSensitive).size() == 1);
}

TEST_CASE("timing: an impact landing with a push on its tiles is flagged") {
  World w;
  w.add(mech(10, {0, 2}));
  w.add(vek(1, A));
  SkillEffect se = effect_from({0, 2});
  se.add_artillery(space_damage(R, 1), "effects/shotup_tank", kNoDelay);
  se.add_delay(0.4f);
  se.add_damage(push_sd(A, Dir::Right));
  w.fire(se);
  CHECK(w.frame_of(ResolveEventType::Impact) == w.frame_of(ResolveEventType::PushMoved, 1));
  CHECK(w.pawn(1)->pos == R);
  CHECK(w.pawn(1)->hp == 3);  // the shell landed (P1) before the push did (P3)
  REQUIRE(w.last.timing.size() == 1);
  CHECK(w.last.timing[0].kind == TimingKind::ImpactVsPush);
}

// ---- Found by replaying recorded games (itb_inspect --replay) -----------------------

TEST_CASE("an idle board that still changed runs another frame: a walker picks up a pod") {
  World w;
  w.add(mech(1, A));
  w.tile({2, 4}).pod = PodState::Present;
  SkillEffect se = effect_from(A);
  se.add_move({A, {2, 3}, {2, 4}}, kFullDelay);
  w.fire(se);
  CHECK(w.pawn(1)->pos == Point{2, 4});
  // The walk ends (P3) with the board idle; the pod goes on the next frame's
  // tile rules (P2), which the game always runs.
  CHECK(w.tile({2, 4}).pod == PodState::Collected);
}

TEST_CASE("the Soldier psion's death takes its +1 HP back (and can kill)") {
  // Pawn::SetMutation via Board::UpdateLeaders: ComputeHealthTotal drops by 1.
  World w;
  Pawn psion = vek(1, A, 1, 1);
  psion.leader = Leader::Health;
  w.add(psion);
  w.add(vek(2, {5, 5}, 3, 3));  // 2 HP + the psion's 1
  w.add(vek(3, {6, 6}, 1, 3));  // only alive thanks to the bonus
  w.board.psion = Leader::Health;
  SkillEffect se = effect_from(R);
  se.add_damage(space_damage(A, 1));
  w.fire(se);
  CHECK(w.board.psion == Leader::None);
  CHECK(w.pawn(2)->hp == 2);
  CHECK(w.pawn(2)->max_hp == 2);
  CHECK((!w.pawn(3) || !w.pawn(3)->alive()));
}

TEST_CASE("the board psion is the last living leader in list order") {
  World w;
  Pawn soldier = vek(1, A, 1, 1);
  soldier.leader = Leader::Health;
  w.add(soldier);
  Pawn shell = vek(2, R, 1, 1);
  shell.leader = Leader::Armor;
  w.add(shell);
  w.add(vek(3, {5, 5}, 2, 2));
  w.board.psion = Leader::Armor;
  SkillEffect se = effect_from({0, 0});
  se.add_damage(space_damage(R, 1));  // the Shell psion (last) dies
  w.fire(se);
  CHECK(w.board.psion == Leader::Health);
  // The Soldier psion's mutation now reaches the Vek: +1 maximum and current HP.
  CHECK(w.pawn(3)->hp == 3);
  CHECK(w.pawn(3)->max_hp == 3);
}

TEST_CASE("psion mutations skip Minor pawns") {
  World w;
  Pawn egg = vek(1, A);
  egg.minor = true;
  w.add(egg);
  w.add(vek(2, R));
  w.board.psion = Leader::Armor;
  CHECK_FALSE(mutation_affects(w.board, *w.pawn(1), Leader::Armor));
  CHECK(mutation_affects(w.board, *w.pawn(2), Leader::Armor));
}

TEST_CASE("a pawn arriving on a teleporter pad swaps with the partner pad") {
  // Board::OnLoop -> Board::Teleport once no effect is active.
  World w;
  w.board.teleporters = {{2, 4}, {6, 6}};
  w.board.teleporter_occupants = {-1, -1};
  w.tile({2, 4}).teleporter = w.tile({6, 6}).teleporter = true;
  w.add(mech(1, A));
  w.add(vek(2, {6, 6}));
  SkillEffect se = effect_from(A);
  se.add_move({A, {2, 3}, {2, 4}}, kFullDelay);
  w.fire(se);
  CHECK(w.pawn(1)->pos == Point{6, 6});
  CHECK(w.pawn(2)->pos == Point{2, 4});
  // Both now stand where they arrived: nobody bounces back.
  CHECK(w.board.teleporter_occupants == std::vector<int32_t>{2, 1});
}

// ---- Burrower dives (stage 2 H4, Pawn::Burrow(-1, -1)) -----------------------------
// A hurt burrower dives: it stays on its tile while the dive animation plays
// and leaves the board (SetSpace(-1, -1)) when it ends, within the action.

namespace {

Pawn burrower(int32_t uid, Point pos) {
  Pawn p = vek(uid, pos);
  p.burrows = true;
  p.pushable = false;
  return p;
}

}  // namespace

TEST_CASE("a burrower hurt by a weapon dives off the board before the action settles") {
  World w;
  w.add(mech(10, {0, 4}));
  Pawn b = burrower(1, {2, 4});
  b.queued = QueuedShot{0, {2, 4}, {2, 5}};
  w.add(b);
  w.fire(laser({0, 4}, Dir::Right, 3));  // {1, 4} 3, {2, 4} 2
  const Pawn* p = w.pawn(1);
  REQUIRE(p != nullptr);
  CHECK(p->hp == 1);
  CHECK(p->alive());
  CHECK_FALSE(p->pos.valid());
  CHECK(p->movement.prev_pos == Point{2, 4});
  CHECK_FALSE(p->queued.active());
  CHECK_FALSE(p->fire);
  CHECK_FALSE(w.removed(1));
  CHECK(board_pawn(w.board, {2, 4}) == nullptr);
  // Underground when the dive animation ends.
  CHECK(w.frame_of(ResolveEventType::PawnUnderground, 1) ==
        FrameClock::steady(60.0).tracker_updates(Durations{}.burrow_dive) - 1);
  CHECK(w.count(RulesEventType::PawnBurrowed) == 1);
  CHECK(w.last.quiescent);
}

TEST_CASE("a diving burrower is still on its tile for the rest of the chunk") {
  World w;
  w.add(mech(10, A));
  w.add(burrower(1, R));
  SkillEffect se = effect_from(A);
  se.add_damage(space_damage(R, 1));
  se.add_damage(space_damage(R, 1));  // same chunk: the dive has just started
  w.fire(se);
  CHECK(w.pawn(1)->hp == 1);
  CHECK_FALSE(w.pawn(1)->pos.valid());
  CHECK(w.count(RulesEventType::PawnBurrowed) == 2);
  CHECK(w.frames(ResolveEventType::PawnUnderground, 1).size() == 1);
}

TEST_CASE("a FULL_DELAY chunk after the dive finds the tile empty") {
  World w;
  w.add(mech(10, A));
  w.add(burrower(1, R));
  SkillEffect se = effect_from(A);
  se.add_damage(space_damage(R, 1));
  se.add_delay(kFullDelay);  // waits for the dive (the pawn is busy)
  se.add_damage(space_damage(R, 1));
  w.fire(se);
  CHECK(w.pawn(1)->hp == 2);
  CHECK_FALSE(w.pawn(1)->pos.valid());
}

TEST_CASE("a burrower hit by a weapon on a cracked tile does not dive") {
  World w;
  w.add(mech(10, A));
  w.tile(R).cracked = true;
  w.add(burrower(1, R));
  SkillEffect se = effect_from(A);
  se.add_damage(space_damage(R, 1));
  w.fire(se);
  CHECK(w.count(RulesEventType::PawnBurrowed) == 0);
  CHECK(w.frames(ResolveEventType::PawnUnderground, 1).empty());
  // The tile collapses under it: it falls instead.
  CHECK(w.tile(R).is_chasm());
  const Pawn* fell = w.pawn(1);
  CHECK((fell == nullptr || !fell->alive()));
}

TEST_CASE("a bump always makes a burrower dive, on a cracked tile too") {
  World w;
  w.add(mech(10, A));
  w.tile(R).cracked = true;
  w.add(burrower(1, R));
  w.add(vek(2, {4, 2}));
  SkillEffect se = effect_from(A);
  se.add_damage(push_sd({4, 2}, Dir::Left));  // vek 2 bumps into the burrower
  w.fire(se);
  CHECK(w.pawn(2)->hp == 2);
  CHECK(w.pawn(2)->pos == Point{4, 2});
  CHECK(w.pawn(1)->hp == 2);
  CHECK(w.pawn(1)->alive());
  CHECK_FALSE(w.pawn(1)->pos.valid());
  CHECK(w.pawn(1)->movement.prev_pos == R);
}
