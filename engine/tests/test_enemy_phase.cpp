// Stage 7: the enemy phase (Engine::end_turn). Test vectors V1-V50 are the
// stage 7 spec's section 5 table (setup -> board at the end of the enemy's
// spawns); the rest cover every native environment on recorded boards and the
// chance nodes. Skipped (with a warning) without a game install.

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "itb/engine.hpp"
#include "itb/game_data.hpp"
#include "itb/objectives.hpp"
#include "itb/recording.hpp"
#include "itb/score.hpp"
#include "itb/tile_rules.hpp"

using namespace itb;

namespace {

Engine* engine() {
  static Engine* e = [] {
    const auto root = GameData::default_game_root();
    if (root.empty() || !std::filesystem::exists(root / "scripts" / "scripts.lua")) {
      return static_cast<Engine*>(nullptr);
    }
    return Engine::create(root).release();
  }();
  return e;
}

#define NEED_ENGINE()                                                                     \
  if (!engine()) {                                                                        \
    WARN_MESSAGE(false, "no game install found; set ITB_GAME_DIR to run engine tests"); \
    return;                                                                               \
  }                                                                                       \
  Engine& E = *engine()

void set_building(Board& b, Point p, int hp = 1) {
  Tile& t = b.tile(p);
  t.terrain = Terrain::Building;
  t.hp = t.max_hp = static_cast<int8_t>(hp);
  t.populated = true;
}

void set_mountain(Board& b, Point p) {
  Tile& t = b.tile(p);
  t.terrain = Terrain::Mountain;
  t.hp = t.max_hp = 2;
}

// A new pawn of this type (mechs: the squad flags). Returns its uid.
int32_t place(Board& b, const char* type, Point p, bool mech = false) {
  const PawnDef* def = engine()->data().pawn(type);
  REQUIRE_MESSAGE(def != nullptr, type);
  int32_t uid = 0;
  for (const Pawn& q : b.pawns()) uid = std::max(uid, q.uid + 1);
  Pawn pawn = engine()->data().make_pawn(*def, uid, p);
  if (mech) {
    pawn.mech = true;
    pawn.corpse = true;
    pawn.team = Team::Player;
  }
  b.add_pawn(pawn);
  return uid;
}

int32_t mech(Board& b, Point p) { return place(b, "PunchMech", p, true); }

Pawn& P(Board& b, int32_t uid) {
  Pawn* p = b.find_pawn(uid);
  REQUIRE(p != nullptr);
  return *p;
}

void queue(Board& b, int32_t uid, Point target) {
  Pawn& p = P(b, uid);
  p.queued = QueuedShot{0, p.pos, target};
}

void hp(Board& b, int32_t uid, int now, int max = -1) {
  Pawn& p = P(b, uid);
  p.hp = static_cast<int8_t>(now);
  if (max >= 0) p.max_hp = static_cast<int8_t>(max);
}

bool dead(const Board& b, int32_t uid) {
  const Pawn* p = b.find_pawn(uid);
  return !p || !p->alive() || p->fallen;
}

int hp_of(const Board& b, int32_t uid) {
  const Pawn* p = b.find_pawn(uid);
  return p ? p->hp : -1;
}

TurnContext context(const char* mission = "") {
  TurnContext ctx;
  ctx.mission.mission_id = mission;
  return ctx;
}

void danger(TurnContext& ctx, std::initializer_list<Point> tiles) {
  for (Point p : tiles) ctx.mission.danger.push_back(DangerTile{p, 1, true, -1});
}

std::set<int32_t> fired(const PhaseResult& r) {
  std::set<int32_t> out;
  for (const PhaseEvent& e : r.events) {
    if (e.type == PhaseEventType::ShotFired) out.insert(e.uid);
  }
  return out;
}

// Board::UpdateLeaders at load: the last living leader.
void set_psion(Board& b) {
  for (const Pawn& p : b.pawns()) {
    if (p.alive() && p.leader != Leader::None) b.psion = p.leader;
  }
}

}  // namespace

// ---- 1.3 webs and 1.4 status ticks -----------------------------------------------

TEST_CASE("V1 webs are released before the Vek attack") {
  NEED_ENGINE();
  Board b;
  const int32_t m = mech(b, {3, 4});
  const int32_t s = place(b, "Scorpion1", {3, 3});
  add_web(b, {3, 3}, Dir::Down);
  REQUIRE(P(b, m).webbed);
  queue(b, s, {3, 4});
  const PhaseResult r = E.end_turn(b, context());
  CHECK(hp_of(b, m) == 2);
  CHECK_FALSE(P(b, m).webbed);
  CHECK(b.tile({3, 3}).web_out == 0);
  CHECK(b.tile({3, 4}).web_in == 0);
  CHECK(r.count(PhaseEventType::WebsCleared) == 1);
  CHECK(r.exact);
}

TEST_CASE("V2 a Vek burning to death never fires") {
  NEED_ENGINE();
  Board b;
  const int32_t f = place(b, "Firefly1", {2, 5});
  hp(b, f, 1);
  P(b, f).fire = true;
  queue(b, f, {2, 4});
  set_building(b, {2, 1});
  const PhaseResult r = E.end_turn(b, context());
  CHECK(dead(b, f));
  CHECK(b.tile({2, 1}).hp == 1);
  CHECK(b.grid_power == 7);
  CHECK(fired(r).empty());
  CHECK(r.count(PhaseEventType::ShotCancelled) == 1);
}

TEST_CASE("an ACID Vek burning to death in the enemy phase is an acid kill") {
  // EVENT_ACID_DESTROYED is raised by the death itself, whatever the phase;
  // Mission_AcidTank's UpdateMission adds it up every frame.
  NEED_ENGINE();
  Board b;
  const int32_t f = place(b, "Firefly1", {2, 5});
  hp(b, f, 1);
  P(b, f).fire = true;
  P(b, f).acid = true;
  const int32_t g = place(b, "Firefly1", {5, 5});  // no ACID
  hp(b, g, 1);
  P(b, g).fire = true;
  const Board before = b;
  const TurnContext ctx = context("Mission_AcidTank");
  const PhaseResult r = E.end_turn(b, ctx);
  CHECK(dead(b, f));
  CHECK(dead(b, g));
  CHECK(b.acid_destroyed == 1);
  const ObjectiveReport obj = evaluate_objectives(before, b, &ctx, &r);
  CHECK(obj.acid_kills == 1);
  CHECK(obj.enemy_kills == 2);
}

TEST_CASE("V3 the fire tick ignores shields and armor") {
  NEED_ENGINE();
  Board b;
  const int32_t m = mech(b, {3, 4});
  hp(b, m, 2);
  P(b, m).fire = true;
  P(b, m).shield = true;
  P(b, m).armor = true;
  E.end_turn(b, context());
  CHECK(hp_of(b, m) == 1);
  CHECK(P(b, m).shield);
  CHECK(P(b, m).fire);
}

TEST_CASE("V4 Cauterize turns the fire tick into a heal") {
  NEED_ENGINE();
  Board b;
  const int32_t m = mech(b, {3, 4});
  hp(b, m, 2);
  P(b, m).fire = true;
  P(b, m).pilot_abilities |= kPilotPainImmunity;
  E.end_turn(b, context());
  CHECK(hp_of(b, m) == 3);
  CHECK(P(b, m).fire);
}

TEST_CASE("V5 the fire psion stops burn damage") {
  NEED_ENGINE();
  Board b;
  place(b, "Jelly_Fire1", {0, 0});
  const int32_t v = place(b, "Scorpion1", {4, 4});
  hp(b, v, 2);
  P(b, v).fire = true;
  set_psion(b);
  E.end_turn(b, context());
  CHECK(hp_of(b, v) == 2);
}

TEST_CASE("V6 Storm Generator hits every enemy on smoke, raw unless shielded") {
  NEED_ENGINE();
  Board b;
  const int32_t rocket = mech(b, {0, 0});
  P(b, rocket).weapons[1] = intern("Passive_Electric");
  b.passives |= kPassiveElectricSmoke;
  const int32_t armored = place(b, "Scorpion1", {1, 1});
  P(b, armored).armor = true;
  const int32_t shielded = place(b, "Scorpion1", {3, 1});
  P(b, shielded).shield = true;
  const int32_t bot = place(b, "Snowtank1", {5, 1});
  hp(b, bot, 2, 2);
  const int32_t acid = place(b, "Scorpion1", {1, 5});
  P(b, acid).acid = true;
  for (Point p : {Point{1, 1}, Point{3, 1}, Point{5, 1}, Point{1, 5}}) b.tile(p).smoke = true;
  E.end_turn(b, context());
  CHECK(hp_of(b, armored) == 2);
  CHECK(hp_of(b, shielded) == 3);
  CHECK_FALSE(P(b, shielded).shield);
  CHECK(hp_of(b, bot) == 1);
  CHECK(hp_of(b, acid) == 2);
}

TEST_CASE("V7 the upgraded Storm Generator deals 2") {
  NEED_ENGINE();
  Board b;
  mech(b, {0, 0});
  b.passives |= kPassiveElectricSmoke | kPassiveElectricSmokeA;
  const int32_t v = place(b, "Scorpion1", {4, 4});
  hp(b, v, 2);
  b.tile({4, 4}).smoke = true;
  E.end_turn(b, context());
  CHECK(dead(b, v));
}

TEST_CASE("V8 the Psion Tyrant hits every player-team pawn") {
  NEED_ENGINE();
  Board b;
  place(b, "Jelly_Lava1", {0, 0});
  const int32_t plain = mech(b, {1, 2});
  const int32_t armored = mech(b, {2, 2});
  P(b, armored).armor = true;
  const int32_t shielded = mech(b, {3, 2});
  P(b, shielded).shield = true;
  const int32_t frozen = mech(b, {5, 2});
  P(b, frozen).frozen = true;
  const int32_t acid = mech(b, {6, 2});
  P(b, acid).armor = true;
  P(b, acid).acid = true;
  const int32_t train = place(b, "Train_Pawn", {4, 6});
  set_psion(b);
  REQUIRE(b.psion == Leader::Tentacle);
  const PhaseResult r = E.end_turn(b, context("Mission_Train"));
  CHECK(hp_of(b, plain) == 2);
  CHECK(hp_of(b, armored) == 3);
  CHECK(hp_of(b, shielded) == 3);
  CHECK_FALSE(P(b, shielded).shield);
  CHECK(hp_of(b, frozen) == 3);
  CHECK_FALSE(P(b, frozen).frozen);
  CHECK(hp_of(b, acid) == 2);
  CHECK(b.find_pawn(train) == nullptr);
  bool wreck = false;
  for (const Pawn& p : b.pawns()) wreck = wreck || (symbol_name(p.type) == "Train_Damaged" && p.pos == Point{4, 6});
  CHECK(wreck);
  CHECK(r.count(PhaseEventType::MissionHook) == 1);
}

TEST_CASE("V9 a psion that burns to death takes its tick with it") {
  NEED_ENGINE();
  Board b;
  const int32_t psion = place(b, "Jelly_Lava1", {0, 0});
  hp(b, psion, 1);
  P(b, psion).fire = true;
  const int32_t m = mech(b, {3, 3});
  set_psion(b);
  E.end_turn(b, context());
  CHECK(dead(b, psion));
  CHECK(b.psion == Leader::None);
  CHECK(hp_of(b, m) == 3);
}

TEST_CASE("V10 burning then regenerating under the Blood psion") {
  NEED_ENGINE();
  Board b;
  place(b, "Jelly_Regen1", {0, 0});
  const int32_t v = place(b, "Scorpion1", {2, 2});
  hp(b, v, 1);
  P(b, v).fire = true;
  const int32_t w = place(b, "Scorpion1", {5, 5});
  hp(b, w, 2);
  P(b, w).fire = true;
  set_psion(b);
  E.end_turn(b, context());
  CHECK(dead(b, v));
  CHECK(hp_of(b, w) == 2);
}

TEST_CASE("V10b the Blood psion heals a burrower underground") {
  // Live 2026-10-10: a Burrower2 that dove (off the board, still on the
  // board's pawn list) came back up healed.
  NEED_ENGINE();
  Board b;
  place(b, "Jelly_Regen1", {0, 0});
  const int32_t w = place(b, "Burrower2", {4, 4});
  hp(b, w, 3);
  P(b, w).movement.prev_pos = P(b, w).pos;
  P(b, w).pos = kInvalidPoint;
  set_psion(b);
  E.end_turn(b, context());
  CHECK(hp_of(b, w) == 4);
}

TEST_CASE("V11 no regeneration once the Blood psion burned") {
  NEED_ENGINE();
  Board b;
  const int32_t psion = place(b, "Jelly_Regen1", {0, 0});
  hp(b, psion, 1);
  P(b, psion).fire = true;
  const int32_t w = place(b, "Scorpion1", {5, 5});
  hp(b, w, 2);
  set_psion(b);
  E.end_turn(b, context());
  CHECK(dead(b, psion));
  CHECK(hp_of(b, w) == 2);
}

TEST_CASE("V12 the Regen pilot heals after the fire tick") {
  NEED_ENGINE();
  Board b;
  const int32_t m = mech(b, {1, 1});
  hp(b, m, 2);
  P(b, m).pilot_abilities |= kPilotRegen;
  const int32_t burning = mech(b, {5, 5});
  hp(b, burning, 1);
  P(b, burning).fire = true;
  P(b, burning).pilot_abilities |= kPilotRegen;
  E.end_turn(b, context());
  CHECK(hp_of(b, m) == 3);
  CHECK(hp_of(b, burning) == 0);
}

TEST_CASE("V13 a Vek burning under the Blast psion explodes before the attacks") {
  NEED_ENGINE();
  Board b;
  place(b, "Jelly_Explode1", {0, 0});
  const int32_t v = place(b, "Scarab1", {3, 3});
  hp(b, v, 1);
  P(b, v).fire = true;
  P(b, v).armor = true;
  const int32_t m = mech(b, {3, 4});
  P(b, m).acid = true;
  set_psion(b);
  E.end_turn(b, context());
  CHECK(dead(b, v));
  CHECK(hp_of(b, m) == 2);  // explosion mode: no ACID doubling
}

TEST_CASE("V14 the Soldier psion dies to lightning and takes its +1 with it") {
  NEED_ENGINE();
  Board b;
  const int32_t psion = place(b, "Jelly_Health1", {5, 5});
  const int32_t v = place(b, "Scorpion1", {1, 6});
  hp(b, v, 1, 4);
  set_psion(b);
  TurnContext ctx = context("Mission_Lightning");
  danger(ctx, {{5, 5}});
  const PhaseResult r = E.end_turn(b, ctx);
  CHECK(dead(b, psion));
  CHECK(dead(b, v));
  CHECK(r.count(PhaseEventType::EnvStep) == 1);
}

TEST_CASE("V15 a burning Burrower dives and loses its attack") {
  NEED_ENGINE();
  Board b;
  const int32_t m = mech(b, {3, 4});
  const int32_t bur = place(b, "Burrower1", {3, 3});
  P(b, bur).fire = true;
  queue(b, bur, {3, 4});
  const PhaseResult r = E.end_turn(b, context());
  CHECK(hp_of(b, m) == 3);
  CHECK(hp_of(b, bur) == 2);
  CHECK_FALSE(P(b, bur).pos.valid());
  CHECK(r.count(PhaseEventType::Burrowed) == 1);
  CHECK(fired(r).empty());
}

// ---- 1.6 queued shooters ---------------------------------------------------------

TEST_CASE("V16 an earlier shooter kills a later one before it fires") {
  NEED_ENGINE();
  Board b;
  const int32_t f = place(b, "Firefly1", {1, 3});
  queue(b, f, {2, 3});
  const int32_t h = place(b, "Hornet1", {3, 3});
  hp(b, h, 1);
  queue(b, h, {3, 4});
  const int32_t m = mech(b, {3, 4});
  const PhaseResult r = E.end_turn(b, context());
  CHECK(dead(b, h));
  CHECK(hp_of(b, m) == 3);
  CHECK(fired(r) == std::set<int32_t>{f});
}

TEST_CASE("V17 list order decides: the Hornet strikes first") {
  NEED_ENGINE();
  Board b;
  const int32_t h = place(b, "Hornet1", {3, 3});
  hp(b, h, 1);
  queue(b, h, {3, 4});
  const int32_t f = place(b, "Firefly1", {1, 3});
  queue(b, f, {2, 3});
  const int32_t m = mech(b, {3, 4});
  E.end_turn(b, context());
  CHECK(hp_of(b, m) == 2);
  CHECK(dead(b, h));
}

TEST_CASE("V18 a pushed Firefly re-aims from its new tile") {
  NEED_ENGINE();
  Board b;
  const int32_t bouncer = place(b, "Bouncer1", {2, 3});
  queue(b, bouncer, {3, 3});
  const int32_t f = place(b, "Firefly1", {3, 3});
  queue(b, f, {3, 2});
  set_building(b, {3, 1});
  const int32_t m = mech(b, {4, 1});
  E.end_turn(b, context());
  CHECK(P(b, bouncer).pos == Point{1, 3});
  CHECK(P(b, f).pos == Point{4, 3});
  CHECK(hp_of(b, f) == 2);
  CHECK(hp_of(b, m) == 2);
  CHECK(b.tile({3, 1}).hp == 1);
}

TEST_CASE("V19 a pushed Scarab's shell lands one tile over") {
  NEED_ENGINE();
  Board b;
  const int32_t bouncer = place(b, "Bouncer1", {0, 5});
  queue(b, bouncer, {1, 5});
  const int32_t c = place(b, "Scarab1", {1, 5});
  queue(b, c, {1, 2});
  set_mountain(b, {2, 2});
  set_mountain(b, {1, 2});
  E.end_turn(b, context());
  CHECK(P(b, c).pos == Point{2, 5});
  CHECK(b.tile({2, 2}).hp == 1);
  CHECK(b.tile({1, 2}).hp == 2);
}

TEST_CASE("V20 a shot pushed off the board is cleared") {
  NEED_ENGINE();
  Board b;
  const int32_t bouncer = place(b, "Bouncer1", {3, 4});
  queue(b, bouncer, {3, 3});
  const int32_t c = place(b, "Scarab1", {3, 3});
  queue(b, c, {3, 0});
  set_building(b, {3, 0});
  const PhaseResult r = E.end_turn(b, context());
  CHECK(P(b, c).pos == Point{3, 2});
  CHECK(b.tile({3, 0}).hp == 1);
  CHECK(fired(r).count(c) == 0);
}

TEST_CASE("V21 a Blast psion killed by an earlier shooter: no later explosion") {
  NEED_ENGINE();
  Board b;
  const int32_t psion = place(b, "Jelly_Explode1", {1, 1});
  hp(b, psion, 1);
  const int32_t f1 = place(b, "Firefly1", {1, 3});
  queue(b, f1, {1, 2});
  const int32_t f2 = place(b, "Firefly1", {6, 5});
  queue(b, f2, {5, 5});
  const int32_t v = place(b, "Scorpion1", {4, 5});
  hp(b, v, 1);
  const int32_t m = mech(b, {4, 6});
  set_psion(b);
  E.end_turn(b, context());
  CHECK(dead(b, psion));
  CHECK(dead(b, v));
  CHECK(hp_of(b, m) == 3);
}

// ---- 2.2 environments ---------------------------------------------------------------

TEST_CASE("V22 Terratide smoke cancels shots on its row") {
  NEED_ENGINE();
  Board b;
  const int32_t f = place(b, "Firefly1", {4, 5});
  queue(b, f, {4, 4});
  set_building(b, {4, 3});
  TurnContext ctx = context("Mission_Terratide");
  ctx.mission.tides_index = 2;
  const PhaseResult r = E.end_turn(b, ctx);
  CHECK(b.tile({4, 5}).smoke);
  CHECK(b.tile({0, 5}).smoke);
  CHECK_FALSE(b.tile({0, 4}).smoke);
  CHECK(b.tile({4, 3}).hp == 1);
  CHECK(fired(r).empty());
  CHECK(r.environment == "Env_Terratide");
}

TEST_CASE("V23 the ice storm freezes shooters and their targets") {
  NEED_ENGINE();
  Board b;
  const int32_t h = place(b, "Hornet1", {2, 2});
  queue(b, h, {1, 2});
  const int32_t m = mech(b, {1, 2});
  const int32_t m2 = mech(b, {3, 3});
  const int32_t f = place(b, "Firefly1", {6, 3});
  queue(b, f, {5, 3});
  TurnContext ctx = context("Mission_SnowStorm");
  for (int x = 2; x <= 4; ++x) {
    for (int y = 1; y <= 3; ++y) ctx.mission.freeze.push_back({x, y});
  }
  E.end_turn(b, ctx);
  CHECK(P(b, h).frozen);
  CHECK(hp_of(b, m) == 3);
  CHECK(hp_of(b, m2) == 3);
  CHECK_FALSE(P(b, m2).frozen);
}

TEST_CASE("V24 the ice storm turns water to ice and freezes buildings") {
  NEED_ENGINE();
  Board b;
  b.tile({4, 3}).terrain = Terrain::Water;
  set_building(b, {2, 1});
  TurnContext ctx = context("Mission_SnowStorm");
  ctx.mission.freeze = {{4, 3}, {2, 1}};
  E.end_turn(b, ctx);
  CHECK(b.tile({4, 3}).terrain == Terrain::Ice);
  CHECK(b.tile({4, 3}).hp == 2);
  CHECK(b.tile({2, 1}).frozen);
}

TEST_CASE("V25 the tide floods its row behind building shadows") {
  NEED_ENGINE();
  Board b;
  const int32_t v = place(b, "Scorpion1", {2, 3});
  queue(b, v, {2, 4});
  const int32_t m = mech(b, {4, 3});
  P(b, m).massive = true;
  const int32_t flyer = place(b, "Hornet1", {5, 3});
  set_mountain(b, {6, 3});
  set_building(b, {1, 2});
  const int32_t target = mech(b, {2, 4});
  TurnContext ctx = context("Mission_Tides");
  danger(ctx, {{2, 3}, {4, 3}, {5, 3}, {6, 3}});
  ctx.mission.tides_index = 3;
  const PhaseResult r = E.end_turn(b, ctx);
  CHECK(dead(b, v));
  CHECK_FALSE(dead(b, m));
  CHECK_FALSE(dead(b, flyer));
  CHECK(b.tile({6, 3}).terrain == Terrain::Water);
  CHECK(b.tile({1, 3}).terrain == Terrain::Road);
  CHECK(b.tile({2, 3}).terrain == Terrain::Water);
  CHECK(hp_of(b, target) == 3);
  CHECK(fired(r).empty());
}

TEST_CASE("V26 cataclysm opens a column, buildings excepted") {
  NEED_ENGINE();
  Board b;
  const int32_t v = place(b, "Scorpion1", {6, 2});
  const int32_t flyer = place(b, "Hornet1", {6, 4});
  const int32_t frozen = place(b, "Hornet1", {6, 5});
  P(b, frozen).frozen = true;
  set_building(b, {6, 7});
  TurnContext ctx = context("Mission_Cataclysm");
  for (int y = 0; y < 7; ++y) danger(ctx, {{6, y}});
  E.end_turn(b, ctx);
  for (int y = 0; y < 7; ++y) CHECK(b.tile({6, y}).terrain == Terrain::Hole);
  CHECK(b.tile({6, 7}).terrain == Terrain::Building);
  CHECK(dead(b, v));
  CHECK(dead(b, frozen));
  CHECK_FALSE(dead(b, flyer));
}

TEST_CASE("V27 seismic cracks its path; a ground Vek falls, a flyer stays") {
  NEED_ENGINE();
  Board b;
  const int32_t v = place(b, "Scorpion1", {3, 2});
  const int32_t target = mech(b, {4, 2});
  queue(b, v, {4, 2});
  const int32_t flyer = place(b, "Hornet1", {3, 3});
  TurnContext ctx = context("Mission_Crack");
  danger(ctx, {{2, 2}, {3, 2}, {3, 3}});
  ctx.mission.ordered_locations = {{2, 2}, {3, 2}, {3, 3}};
  E.end_turn(b, ctx);
  for (Point p : {Point{2, 2}, Point{3, 2}, Point{3, 3}}) CHECK(b.tile(p).terrain == Terrain::Hole);
  CHECK(dead(b, v));
  CHECK_FALSE(dead(b, flyer));
  CHECK(hp_of(b, target) == 3);
}

TEST_CASE("V28 the air strike kills through shields and flight") {
  NEED_ENGINE();
  Board b;
  const int32_t v = place(b, "Scorpion1", {4, 5});
  const int32_t m = mech(b, {4, 4});
  P(b, m).shield = true;
  const int32_t flyer = place(b, "Hornet1", {5, 5});
  set_mountain(b, {3, 5});
  TurnContext ctx = context("Mission_Airstrike");
  danger(ctx, {{4, 4}, {3, 5}, {4, 5}, {5, 5}, {4, 6}});
  E.end_turn(b, ctx);
  CHECK(dead(b, v));
  CHECK(dead(b, m));
  CHECK(dead(b, flyer));
  CHECK(b.tile({3, 5}).terrain == Terrain::Rubble);
}

TEST_CASE("V29 lightning strike order is a chance node with two outcomes") {
  NEED_ENGINE();
  auto run = [&](int branch, PhaseResult* out) {
    Board b;
    place(b, "Jelly_Explode1", {1, 1});
    place(b, "Scorpion1", {5, 2});
    const int32_t m = mech(b, {5, 3});
    set_psion(b);
    TurnContext ctx = context("Mission_Lightning");
    danger(ctx, {{1, 1}, {5, 2}, {1, 5}, {6, 6}});
    ctx.choose = [branch](const ChanceRecord&) { return branch; };
    *out = E.end_turn(b, ctx);
    return hp_of(b, m);
  };
  PhaseResult a, c;
  CHECK(run(0, &a) == 3);  // psion first: no explosion
  CHECK(run(1, &c) == 2);  // Vek first: it explodes next to the mech
  auto order = [](const PhaseResult& r) {
    for (const ChanceRecord& ch : r.chances) {
      if (ch.kind == ChanceKind::EnvOrder) return ch;
    }
    return ChanceRecord{ChanceKind::GridDefense};
  };
  CHECK(order(a).kind == ChanceKind::EnvOrder);
  CHECK(order(a).options == 2);  // 2 occupied strikes -> 2! orders
  CHECK(order(c).outcome == 1);
}

TEST_CASE("V30 wind pushes columns as a convoy from the downwind edge") {
  NEED_ENGINE();
  Board b;
  const int32_t a = place(b, "Scorpion1", {1, 5});
  const int32_t c = place(b, "Scorpion1", {1, 6});
  const int32_t m = mech(b, {4, 7});
  const int32_t d = place(b, "Scorpion1", {4, 6});
  TurnContext ctx = context("Mission_Wind");
  for (int y = 0; y < 8; ++y) danger(ctx, {{1, y}, {4, y}});
  ctx.mission.wind_dir = Dir::Down;
  const PhaseResult r = E.end_turn(b, ctx);
  CHECK(P(b, c).pos == Point{1, 7});
  CHECK(P(b, a).pos == Point{1, 6});
  CHECK(P(b, m).pos == Point{4, 7});
  CHECK(hp_of(b, d) == 2);
  CHECK(hp_of(b, m) == 2);
  CHECK(r.exact);
}

TEST_CASE("V31 wind: the second push lands on a still-occupied tile") {
  NEED_ENGINE();
  Board b;
  set_mountain(b, {1, 7});
  const int32_t c = place(b, "Scarab1", {1, 6});
  const int32_t a = place(b, "Scarab1", {1, 5});
  TurnContext ctx = context("Mission_Wind");
  for (int y = 0; y < 8; ++y) danger(ctx, {{1, y}, {3, y}});
  ctx.mission.wind_dir = Dir::Down;
  E.end_turn(b, ctx);
  CHECK(b.tile({1, 7}).hp == 1);
  CHECK(dead(b, c));
  CHECK(hp_of(b, a) == 1);
  CHECK(P(b, a).pos == Point{1, 5});
}

TEST_CASE("V32 conveyors push downstream first into acid water") {
  NEED_ENGINE();
  Board b;
  b.tile({5, 0}).terrain = Terrain::Water;
  b.tile({5, 0}).acid = true;
  b.tile({5, 1}).conveyor = 0;
  b.tile({5, 2}).conveyor = 0;
  const int32_t a = place(b, "Scorpion1", {5, 1});
  const int32_t c = place(b, "Scorpion1", {5, 2});
  const PhaseResult r = E.end_turn(b, context("Mission_Belt"));
  CHECK(dead(b, a));
  CHECK(b.tile({5, 0}).acid);
  CHECK(P(b, c).pos == Point{5, 1});
  CHECK(r.exact);
}

TEST_CASE("V33 volcano rocks and lava") {
  NEED_ENGINE();
  SUBCASE("rocks") {
    Board b;
    const int32_t v = place(b, "Scorpion1", {2, 3});
    TurnContext ctx = context("Mission_Final");
    ctx.mission.volcano = FinalEnvState{true, 1, 2, false, {{2, 3}}};
    E.end_turn(b, ctx);
    CHECK(dead(b, v));
    CHECK(b.tile({2, 3}).on_fire());
  }
  SUBCASE("lava") {
    Board b;
    const int32_t v = place(b, "Scorpion1", {3, 3});
    const int32_t m = mech(b, {4, 3});
    P(b, m).massive = true;
    TurnContext ctx = context("Mission_Final");
    ctx.mission.volcano = FinalEnvState{true, 2, 1, false, {{3, 3}, {4, 3}}};
    E.end_turn(b, ctx);
    CHECK(dead(b, v));
    CHECK_FALSE(dead(b, m));
    CHECK(b.tile({3, 3}).lava);
    CHECK(b.tile({4, 3}).lava);
  }
}

TEST_CASE("V34 final cave rocks and tentacles") {
  NEED_ENGINE();
  SUBCASE("rocks") {
    Board b;
    b.tile({4, 4}).terrain = Terrain::Hole;
    const int32_t flyer = place(b, "Hornet1", {4, 4});
    TurnContext ctx = context("Mission_Final_Cave");
    ctx.mission.final_cave = FinalEnvState{true, 1, 1, false, {{4, 4}}};
    E.end_turn(b, ctx);
    CHECK(dead(b, flyer));
    CHECK(b.tile({4, 4}).terrain == Terrain::Road);
  }
  SUBCASE("tentacles") {
    Board b;
    const int32_t m = mech(b, {2, 2});
    TurnContext ctx = context("Mission_Final_Cave");
    ctx.mission.final_cave = FinalEnvState{true, 2, 2, false, {{2, 2}}};
    E.end_turn(b, ctx);
    CHECK(dead(b, m));
    CHECK(b.tile({2, 2}).lava);
  }
}

// ---- 2.3 mission hooks ----------------------------------------------------------------

TEST_CASE("V35 acid storm: a popped shield lets ACID in before the next shot") {
  NEED_ENGINE();
  Board b;
  place(b, "Storm_Generator", {0, 7});
  const int32_t h = place(b, "Hornet1", {3, 3});
  queue(b, h, {3, 4});
  const int32_t f = place(b, "Firefly1", {6, 4});
  queue(b, f, {5, 4});
  const int32_t m = mech(b, {3, 4});
  P(b, m).shield = true;
  E.end_turn(b, context("Mission_AcidStorm"));
  CHECK(P(b, m).acid);
  CHECK(hp_of(b, m) == 1);
}

TEST_CASE("V36 shields mission: the generator's death drops every shield") {
  NEED_ENGINE();
  Board b;
  place(b, "Shield_Building", {1, 3});
  const int32_t f = place(b, "Firefly1", {1, 5});
  queue(b, f, {1, 4});
  const int32_t h = place(b, "Hornet1", {5, 3});
  queue(b, h, {5, 2});
  set_building(b, {5, 2});
  b.tile({5, 2}).shield = true;
  const PhaseResult r = E.end_turn(b, context("Mission_Shields"));
  CHECK(b.tile({5, 2}).hp == 0);
  CHECK(b.grid_power == 6);
  CHECK(r.count(PhaseEventType::MissionHook) == 1);
}

TEST_CASE("V37 the dam floods before the next Vek fires") {
  NEED_ENGINE();
  Board b;
  const int32_t dam = place(b, "Dam_Pawn", {3, 0});
  hp(b, dam, 1);
  const int32_t h = place(b, "Hornet1", {2, 0});
  queue(b, h, {3, 0});
  const int32_t c = place(b, "Scarab1", {3, 2});
  queue(b, c, {3, 5});
  set_building(b, {3, 5});
  const PhaseResult r = E.end_turn(b, context("Mission_Dam"));
  CHECK(dead(b, c));
  CHECK(b.tile({3, 2}).terrain == Terrain::Water);
  CHECK(b.tile({4, 7}).terrain == Terrain::Water);
  CHECK(b.tile({3, 5}).hp == 1);
  CHECK(fired(r).count(c) == 0);
}

TEST_CASE("V38 the train fires after every Vek and stops on a blocker") {
  NEED_ENGINE();
  Board b;
  const int32_t bouncer = place(b, "Bouncer1", {2, 5});
  queue(b, bouncer, {3, 5});
  const int32_t v = place(b, "Scorpion1", {3, 5});
  const int32_t train = place(b, "Train_Pawn", {4, 6});
  const PhaseResult r = E.end_turn(b, context("Mission_Train"));
  CHECK(dead(b, v));
  CHECK(b.find_pawn(train) == nullptr);
  bool wreck = false;
  for (const Pawn& p : b.pawns()) wreck = wreck || (symbol_name(p.type) == "Train_Damaged" && p.pos == Point{4, 6});
  CHECK(wreck);
  CHECK(fired(r).count(train) == 1);
}

TEST_CASE("a queued shot with no weapon behind it is used up, not retried") {
  NEED_ENGINE();
  Board b;
  const int32_t v = place(b, "Scorpion1", {3, 3});
  P(b, v).weapons = {};
  P(b, v).queued = QueuedShot{0, {3, 3}, {3, 4}};
  const PhaseResult r = E.end_turn(b, context());
  int fizzles = 0;
  for (const PhaseEvent& e : r.events) fizzles += e.type == PhaseEventType::ShotFizzled ? 1 : 0;
  CHECK(fizzles == 1);
  CHECK_FALSE(P(b, v).queued.active());
}

TEST_CASE("a train the bridge exported without a queued shot stays put") {
  NEED_ENGINE();
  Board b;
  const int32_t train = place(b, "Train_Pawn", {4, 6});
  TurnContext ctx = context("Mission_Train");
  ctx.mission.all_queued_known = true;  // attack_order_all: the train has no shot
  const PhaseResult r = E.end_turn(b, ctx);
  CHECK(fired(r).count(train) == 0);
  REQUIRE(b.find_pawn(train) != nullptr);
  CHECK(b.find_pawn(train)->pos == Point{4, 6});
  // Old recordings: the move is inferred.
  Board old;
  const int32_t moving = place(old, "Train_Pawn", {4, 6});
  const PhaseResult r2 = E.end_turn(old, context("Mission_Train"));
  CHECK(fired(r2).count(moving) == 1);
}

TEST_CASE("V39 a satellite launch kills its neighbours and flies away") {
  NEED_ENGINE();
  Board b;
  const int32_t rocket = place(b, "SatelliteRocket", {3, 5});
  const int32_t flyer = place(b, "Hornet1", {3, 4});
  const int32_t v = place(b, "Scorpion1", {2, 5});
  TurnContext ctx = context("Mission_Satellite");
  ctx.mission.launching = {rocket};
  const PhaseResult r = E.end_turn(b, ctx);
  CHECK(dead(b, flyer));
  CHECK(dead(b, v));
  CHECK(b.find_pawn(rocket) == nullptr);
  CHECK(fired(r).count(rocket) == 1);
}

TEST_CASE("V40 hacking: the facility dies and the bot changes sides") {
  NEED_ENGINE();
  Board b;
  const int32_t hack = place(b, "Hacked_Building", {1, 3});
  const int32_t f = place(b, "Firefly1", {1, 5});
  queue(b, f, {1, 4});
  const int32_t bot = place(b, "Snowtank1", {5, 5});
  P(b, bot).shield = true;
  queue(b, bot, {5, 4});
  set_building(b, {5, 2});
  TurnContext ctx = context("Mission_Hacking");
  ctx.mission.hacking_bot = bot;
  ctx.mission.hacking_building = hack;
  E.end_turn(b, ctx);
  CHECK(dead(b, hack));
  CHECK(b.find_pawn(bot) == nullptr);
  const Pawn* tank = nullptr;
  for (const Pawn& p : b.pawns()) {
    if (symbol_name(p.type) == "Snowtank1_Player") tank = &p;
  }
  REQUIRE(tank != nullptr);
  CHECK(tank->pos == Point{5, 5});
  CHECK(tank->shield);
  CHECK(tank->team == Team::Player);
  CHECK(b.tile({5, 2}).hp == 1);
}

// ---- Mission hooks during the player's turn ------------------------------------------
//
// BoardPlayer::OnLoop (0x008c9760) calls Mission:BaseUpdate on every frame of
// every state but the finished one, the player's turn included, so player
// actions run the same per-frame hooks (ActionOptions::mission).

namespace {

Recording live_fixture(const char* name) {
  std::string error;
  auto rec = load_recording(std::string(ITB_FIXTURE_DIR) + "/" + name, &engine()->data(), &error);
  REQUIRE_MESSAGE(rec.has_value(), error);
  for (const auto& [uid, pilot] : rec->pilots) {
    if (Pawn* p = rec->board.find_pawn(uid)) p->pilot_abilities |= engine()->pilot_ability(pilot);
  }
  return *rec;
}

}  // namespace

TEST_CASE("player turn: the acid storm re-applies ACID right after a repair (live 2026-10-10)") {
  NEED_ENGINE();
  // Mission_AcidStorm turn 1: JetMech#0 (moved, full HP, ACID) repairs.
  // Skill_Repair clears ACID, the next frame's UpdateMission sets it again
  // while the Storm Generator lives: the game's step 4 still shows ACID.
  const Recording before = live_fixture("live_acidstorm_t1_step3.json");
  const Recording after = live_fixture("live_acidstorm_t1_step4.json");
  REQUIRE(before.mission.mission_id == "Mission_AcidStorm");
  const TurnContext ctx = turn_context(before, Visibility::Player);
  const int32_t jet = 0;
  REQUIRE(before.board.find_pawn(jet) != nullptr);
  REQUIRE(before.board.find_pawn(jet)->acid);
  REQUIRE(after.board.find_pawn(jet)->acid);

  Board b = before.board;
  const ActionResult r = E.repair(b, jet, kInvalidPoint, "Skill_Repair", action_options(ctx));
  REQUIRE(r.ok());
  CHECK(P(b, jet).acid);
  CHECK(P(b, jet).hp == after.board.find_pawn(jet)->hp);
  CHECK_FALSE(P(b, jet).active);
  for (const Pawn& p : after.board.pawns()) {
    const Pawn* q = b.find_pawn(p.uid);
    REQUIRE(q != nullptr);
    CHECK(q->acid == p.acid);
    CHECK(q->hp == p.hp);
  }

  // Without the mission's hooks (a bare rules call) the repair clears it.
  Board bare = before.board;
  REQUIRE(E.repair(bare, jet).ok());
  CHECK_FALSE(P(bare, jet).acid);

  // With the generator dead the storm is over: the repair clears ACID.
  Board calm = before.board;
  int32_t gen = -1;
  for (const Pawn& p : calm.pawns()) {
    if (symbol_name(p.type) == "Storm_Generator") gen = p.uid;
  }
  REQUIRE(gen >= 0);
  calm.remove_pawn(gen);
  REQUIRE(E.repair(calm, jet, kInvalidPoint, "Skill_Repair", action_options(ctx)).ok());
  CHECK_FALSE(P(calm, jet).acid);
}

TEST_CASE("conveyors come from the live belts, not the save scan (live 2026-10-11 m34)") {
  NEED_ENGINE();
  // Mission_BeltRandom. The bridge's per-tile `conveyor` field scans the whole
  // save, which holds the other regions' maps too: it reported phantom belts
  // along row 5 and the B4/C4 belts as pushing left. The Env_Belt instance
  // (Belts/BeltsDir) and the conveyor<dir>.png custom tiles agree: six belts,
  // C4 and B4 pushing down.
  const Recording t1 = live_fixture("live_belt_t1_end_turn.json");
  REQUIRE(t1.mission.mission_id == "Mission_BeltRandom");
  REQUIRE(t1.mission.belts_known);
  REQUIRE(t1.mission.belts.size() == 6);
  CHECK(t1.mission.belts.back() == std::pair<Point, Dir>{Point{4, 5}, Dir::Down});
  int conveyors = 0;
  for (int i = 0; i < kTileCount; ++i) conveyors += t1.board.tile(Point::from_index(i)).conveyor >= 0;
  CHECK(conveyors == 6);
  CHECK(t1.board.tile({4, 5}).conveyor == static_cast<int>(Dir::Down));
  CHECK(t1.board.tile({4, 6}).conveyor == static_cast<int>(Dir::Down));
  CHECK(t1.board.tile({3, 5}).conveyor == -1);  // phantom

  // Turn 1: the B4 belt bumps the Armor psion into the A4 mountain, then the
  // C4 belt bumps the Tumblebug into it: the psion dies (armor gone), the
  // Tumblebug stays on C4 and its rock throw kills the bomb rock on C5, whose
  // explosion hits the Tumblebug and the DoubleTank the Burnbug pulled to D5.
  {
    Board b = t1.board;
    const PhaseResult r = E.end_turn(b, turn_context(t1, Visibility::Full));
    CHECK(r.exact);  // the recorded list fixes the push order
    CHECK(hp_of(b, 1) == 2);          // DoubletankMech: game 2
    CHECK(P(b, 1).pos == Point{3, 4});
    CHECK(hp_of(b, 1376) == 3);       // Dung2: game 3
    CHECK(P(b, 1376).pos == Point{4, 5});
    CHECK(dead(b, 1377));             // Jelly_Armor1: game dead
    CHECK(dead(b, 1398));             // the bomb rock
  }
  // Turn 2: the rock on B5 dies under the throw; the explosion hits the
  // Tumblebug and rolls Grid Defense for the A5 building (the game resisted).
  const Recording t2 = live_fixture("live_belt_t2_end_turn.json");
  {
    Board b = t2.board;
    const PhaseResult r = E.end_turn(b, turn_context(t2, Visibility::Full));
    CHECK(hp_of(b, 2) == 1);     // NapalmMech: game 1
    CHECK(hp_of(b, 1376) == 2);  // Dung2: game 2
    CHECK(dead(b, 1439));        // BombRock: game dead
    bool rolled = false;
    for (const ChanceRecord& c : r.chances) rolled |= c.kind == ChanceKind::GridDefense && c.point == Point{3, 7};
    CHECK(rolled);
  }
}

TEST_CASE("player turn: under the acid storm a popped shield lets ACID in before the next shot") {
  NEED_ENGINE();
  Board b;
  place(b, "Storm_Generator", {0, 0});
  const int32_t v = place(b, "Scorpion1", {3, 3});
  hp(b, v, 3, 3);
  P(b, v).shield = true;
  const int32_t m = place(b, "ArtiMech", {3, 6}, true);
  const TurnContext ctx = context("Mission_AcidStorm");
  ActionOptions o = action_options(ctx);
  o.check_legal = false;  // the same mech fires twice
  // First shot: the shield absorbs it; the hook sets ACID on the next frame.
  REQUIRE(E.fire_weapon(b, m, "Ranged_Artillerymech", {3, 3}, std::nullopt, o).ok());
  CHECK_FALSE(P(b, v).shield);
  CHECK(P(b, v).acid);
  CHECK(hp_of(b, v) == 3);
  // Second shot: 1 damage, doubled by ACID.
  REQUIRE(E.fire_weapon(b, m, "Ranged_Artillerymech", {3, 3}, std::nullopt, o).ok());
  CHECK(hp_of(b, v) == 1);
}

TEST_CASE("player turn: the dam floods as soon as the player destroys it") {
  NEED_ENGINE();
  Board b;
  const int32_t dam = place(b, "Dam_Pawn", {3, 0});
  hp(b, dam, 1);
  const int32_t c = place(b, "Scarab1", {3, 3});
  const int32_t m = place(b, "PunchMech", {2, 0}, true);
  const TurnContext ctx = context("Mission_Dam");
  const ActionResult r = E.fire_weapon(b, m, "Prime_Punchmech", {3, 0}, std::nullopt, action_options(ctx));
  REQUIRE(r.ok());
  CHECK(dead(b, dam));
  CHECK(b.tile({3, 3}).terrain == Terrain::Water);
  CHECK(b.tile({4, 7}).terrain == Terrain::Water);
  CHECK(dead(b, c));
  REQUIRE(r.mission_events.size() == 1);
  CHECK(r.mission_events[0].type == PhaseEventType::MissionHook);
  // The enemy phase does not flood a second time.
  const PhaseResult pr = E.end_turn(b, ctx);
  CHECK(pr.count(PhaseEventType::MissionHook) == 0);
}

TEST_CASE("player turn: the shield generator's death drops the shields at once") {
  NEED_ENGINE();
  Board b;
  const int32_t gen = place(b, "Shield_Building", {3, 3});
  const int32_t v = place(b, "Scorpion1", {5, 5});
  P(b, v).shield = true;
  set_building(b, {6, 6});
  b.tile({6, 6}).shield = true;
  const int32_t m = place(b, "PunchMech", {2, 3}, true);
  const TurnContext ctx = context("Mission_Shields");
  REQUIRE(E.fire_weapon(b, m, "Prime_Punchmech", {3, 3}, std::nullopt, action_options(ctx)).ok());
  CHECK(dead(b, gen));
  CHECK_FALSE(P(b, v).shield);
  CHECK_FALSE(b.tile({6, 6}).shield);
  // A new shield given later in the turn stays (the drop happens once).
  P(b, v).shield = true;
  const int32_t m2 = place(b, "PunchMech", {0, 7}, true);
  REQUIRE(E.move(b, m2, {1, 7}, action_options(ctx)).ok());
  CHECK(P(b, v).shield);
}

TEST_CASE("player turn: the native environments with a per-frame hook are the ones flagged") {
  for (const char* id : {"Mission_Airstrike", "Mission_Lightning", "Mission_Crack", "Mission_Cataclysm",
                         "Mission_Tides", "Mission_Terratide", "Mission_SnowStorm", "Mission_Wind", "Mission_Belt",
                         "Mission_BeltRandom", "Mission_Final", "Mission_Final_Cave", "Mission_Dam", "Mission_Train",
                         "Mission_Armored_Train", "Mission_Satellite", "Mission_Volatile", "Mission_AcidStorm",
                         "Mission_Shields", "Mission_Hacking", "Mission_Reactivation", "Mission_Survive", ""}) {
    MissionData m;
    m.mission_id = id;
    CAPTURE(id);
    CHECK(make_native_environment(m)->has_update() == native_environment_has_update(m));
  }
}

// ---- 3 spawning ------------------------------------------------------------------------

TEST_CASE("V41 a spawn blocked by a mech: 1 damage, the spawn waits") {
  NEED_ENGINE();
  Board b;
  const int32_t m = mech(b, {5, 5});
  b.spawn_points = {{5, 5}};
  TurnContext ctx = context();
  ctx.spawn_types = {"Scorpion1"};
  const PhaseResult r = E.end_turn(b, ctx);
  CHECK(hp_of(b, m) == 2);
  CHECK(b.spawn_points == std::vector<Point>{{5, 5}});
  CHECK(b.pawns().size() == 1);
  CHECK(r.count(PhaseEventType::SpawnBlocked) == 1);
}

TEST_CASE("V42 shields and ice absorb the spawn bump") {
  NEED_ENGINE();
  Board b;
  const int32_t s = mech(b, {5, 5});
  P(b, s).shield = true;
  const int32_t f = mech(b, {2, 2});
  P(b, f).frozen = true;
  b.spawn_points = {{5, 5}, {2, 2}};
  E.end_turn(b, context());
  CHECK(hp_of(b, s) == 3);
  CHECK_FALSE(P(b, s).shield);
  CHECK(hp_of(b, f) == 3);
  CHECK_FALSE(P(b, f).frozen);
}

TEST_CASE("V43 Force Amp adds 1 to spawn bumps on Vek, not bots") {
  NEED_ENGINE();
  Board b;
  b.passives |= kPassiveForceAmp;
  const int32_t v = place(b, "Scorpion1", {5, 5});
  const int32_t bot = place(b, "Snowtank1", {2, 2});
  hp(b, bot, 3, 3);
  b.spawn_points = {{5, 5}, {2, 2}};
  E.end_turn(b, context());
  CHECK(hp_of(b, v) == 1);
  CHECK(hp_of(b, bot) == 2);
}

TEST_CASE("V44 Stabilizers spare mechs only") {
  NEED_ENGINE();
  Board b;
  b.passives |= kPassiveBurrows;
  const int32_t m = mech(b, {5, 5});
  const int32_t tank = place(b, "Archive_Tank", {2, 2});
  b.spawn_points = {{5, 5}, {2, 2}};
  E.end_turn(b, context());
  CHECK(hp_of(b, m) == 3);
  CHECK(dead(b, tank));
}

TEST_CASE("V45 the spawn bump ignores armor and ACID") {
  NEED_ENGINE();
  Board b;
  const int32_t v = place(b, "Scorpion1", {5, 5});
  P(b, v).armor = true;
  P(b, v).acid = true;
  b.spawn_points = {{5, 5}};
  E.end_turn(b, context());
  CHECK(hp_of(b, v) == 2);
}

TEST_CASE("V46 a spawn under the flood is dropped") {
  NEED_ENGINE();
  Board b;
  const int32_t dam = place(b, "Dam_Pawn", {3, 0});
  hp(b, dam, 1);
  const int32_t h = place(b, "Hornet1", {2, 0});
  queue(b, h, {3, 0});
  const int32_t m = mech(b, {4, 5});
  b.spawn_points = {{4, 5}};
  const PhaseResult r = E.end_turn(b, context("Mission_Dam"));
  CHECK(hp_of(b, m) == 3);  // massive: stands in the flood, never bumped
  CHECK(b.spawn_points.empty());
  CHECK(r.count(PhaseEventType::SpawnDropped) == 1);
  CHECK(r.count(PhaseEventType::SpawnBlocked) == 0);
}

TEST_CASE("V47 emerging Vek take the tile's fire and mines") {
  NEED_ENGINE();
  Board b;
  b.tile({2, 2}).fire = FireState::Burning;
  b.tile({5, 5}).item = intern("Item_Mine");
  b.spawn_points = {{2, 2}, {5, 5}};
  TurnContext ctx = context();
  ctx.spawn_types = {"Scorpion1", "Scorpion1"};
  ctx.spawn_order_known = true;
  const PhaseResult r = E.end_turn(b, ctx);
  CHECK(r.count(PhaseEventType::SpawnEmerged) == 2);
  const Pawn* burning = nullptr;
  for (const Pawn& p : b.pawns()) {
    if (p.pos == Point{2, 2}) burning = &p;
  }
  REQUIRE(burning != nullptr);
  CHECK(burning->fire);
  CHECK(b.tile({5, 5}).item == kNoSymbol);
  bool mined_alive = false;
  for (const Pawn& p : b.pawns()) mined_alive = mined_alive || (p.pos == Point{5, 5} && p.alive());
  CHECK_FALSE(mined_alive);
  CHECK(b.spawn_points.empty());
}

TEST_CASE("V48 a blocker's explosion resolves before the next spawn") {
  NEED_ENGINE();
  Board b;
  const int32_t boom = place(b, "Snowtank1_Boom", {3, 3});
  b.spawn_points = {{3, 3}, {3, 4}};
  TurnContext ctx = context();
  ctx.spawn_types = {"Scorpion1", "Scorpion1"};
  ctx.spawn_order_known = true;
  E.end_turn(b, ctx);
  CHECK(dead(b, boom));
  const Pawn* emerged = nullptr;
  for (const Pawn& p : b.pawns()) {
    if (p.pos == Point{3, 4}) emerged = &p;
  }
  REQUIRE(emerged != nullptr);
  CHECK(emerged->hp == 3);
  CHECK(b.spawn_points == std::vector<Point>{{3, 3}});
}

TEST_CASE("V49 the final turn: Vek still attack, then the mission ends") {
  NEED_ENGINE();
  Board b;
  b.turn = 4;
  b.total_turns = 4;
  const int32_t h = place(b, "Hornet1", {3, 3});
  queue(b, h, {3, 4});
  const int32_t m = mech(b, {3, 4});
  const int32_t m2 = mech(b, {6, 6});
  b.spawn_points = {{6, 6}};
  const PhaseResult r = E.end_turn(b, context());
  CHECK(hp_of(b, m) == 2);
  CHECK(hp_of(b, m2) == 3);
  CHECK(r.mission_ended);
  CHECK(r.count(PhaseEventType::SpawnBlocked) == 0);
}

TEST_CASE("V50 stale Soldier-psion bonus survives a Tyrant psion; new Vek get none") {
  NEED_ENGINE();
  Board b;
  place(b, "Jelly_Health1", {0, 0});
  const int32_t v = place(b, "Scorpion1", {2, 2});
  hp(b, v, 4, 4);
  P(b, v).health_bonus = true;
  place(b, "Jelly_Lava1", {7, 0});
  const int32_t m = mech(b, {5, 5});
  set_psion(b);
  REQUIRE(b.psion == Leader::Tentacle);
  b.spawn_points = {{6, 6}};
  TurnContext ctx = context();
  ctx.spawn_types = {"Scorpion1"};
  E.end_turn(b, ctx);
  CHECK(hp_of(b, m) == 2);
  CHECK(hp_of(b, v) == 4);
  const Pawn* fresh = nullptr;
  for (const Pawn& p : b.pawns()) {
    if (p.pos == Point{6, 6}) fresh = &p;
  }
  REQUIRE(fresh != nullptr);
  CHECK(fresh->max_hp == 3);
}

// ---- Soldier psion bonus for new pawns, and the driver's bookkeeping -------------------

TEST_CASE("a Vek emerging under the Soldier psion gets its +1") {
  NEED_ENGINE();
  Board b;
  place(b, "Jelly_Health1", {0, 0});
  set_psion(b);
  b.spawn_points = {{6, 6}};
  TurnContext ctx = context();
  ctx.spawn_types = {"Scorpion1"};
  E.end_turn(b, ctx);
  const Pawn* fresh = nullptr;
  for (const Pawn& p : b.pawns()) {
    if (p.pos == Point{6, 6}) fresh = &p;
  }
  REQUIRE(fresh != nullptr);
  CHECK(fresh->max_hp == 4);
  CHECK(fresh->hp == 4);
}

TEST_CASE("a Soldier psion emerging gives the Vek already on the board +1") {
  NEED_ENGINE();
  Board b;
  const int32_t v = place(b, "Scorpion1", {2, 2});
  hp(b, v, 2);
  b.spawn_points = {{6, 6}};
  TurnContext ctx = context();
  ctx.spawn_types = {"Jelly_Health1"};
  E.end_turn(b, ctx);
  CHECK(b.psion == Leader::Health);
  CHECK(hp_of(b, v) == 3);
  CHECK(P(b, v).max_hp == 4);
}

TEST_CASE("recorded AE pilot skills: Thick Skin and Technician") {
  NEED_ENGINE();
  // Pilot::GetAllPilotSkills ids 7 (Thick) and 12 (Regen), active from the
  // pilot's level.
  const std::filesystem::path path =
      std::filesystem::path(ITB_REPO_ROOT) / "recordings/20260506_114649_974/m19_turn_02_solve_input.json";
  if (!std::filesystem::exists(path)) {
    WARN_MESSAGE(false, "recording not found");
    return;
  }
  std::string error;
  auto rec = load_recording(path, &E.data(), &error);
  REQUIRE(rec.has_value());
  const Pawn* rocket = rec->board.find_pawn(1);  // RocketMech: skill1=7, skill2=8, level 2
  REQUIRE(rocket != nullptr);
  CHECK(rocket->has_pilot(kPilotThick));
  CHECK_FALSE(rocket->has_pilot(kPilotRegen));
}

TEST_CASE("End Turn: Networked Shielding is off during the enemy phase") {
  NEED_ENGINE();
  Board b;
  b.passives |= kPassivePlayerTurnShield;
  const int32_t m = mech(b, {3, 4});
  P(b, m).active = true;
  const int32_t h = place(b, "Hornet1", {3, 3});
  queue(b, h, {3, 4});
  E.end_turn(b, context());
  CHECK(hp_of(b, m) == 2);
  CHECK_FALSE(P(b, m).active);
  CHECK_FALSE(b.player_phase);
}

TEST_CASE("unknown spawn types emerge without a pawn and are reported") {
  NEED_ENGINE();
  Board b;
  b.spawn_points = {{6, 6}};
  const PhaseResult r = E.end_turn(b, context());
  CHECK(r.emerged_unknown == std::vector<Point>{{6, 6}});
  CHECK(b.spawn_points.empty());
  CHECK(b.pawns().empty());
}

TEST_CASE("play_turn runs the player's actions, then the enemy phase") {
  NEED_ENGINE();
  Board b;
  const int32_t m = mech(b, {3, 5});
  P(b, m).active = true;
  const int32_t h = place(b, "Hornet1", {3, 3});
  hp(b, h, 1);
  queue(b, h, {3, 4});
  PlayerAction move_and_punch;
  move_and_punch.uid = m;
  move_and_punch.move = {3, 4};
  move_and_punch.kind = PlayerAction::Kind::Weapon;
  move_and_punch.weapon = "Prime_Punchmech";
  move_and_punch.target = {3, 3};
  const TurnResult r = E.play_turn(b, {move_and_punch}, context());
  REQUIRE(r.ok());
  CHECK(r.actions.size() == 2);
  CHECK(dead(b, h));
  CHECK(hp_of(b, m) == 3);
  CHECK(fired(r.enemy).empty());
}

// ---- Chance nodes ----------------------------------------------------------------------

TEST_CASE("chance: Grid Defense rolls in the enemy phase are logged") {
  NEED_ENGINE();
  for (bool resist : {false, true}) {
    Board b;
    const int32_t h = place(b, "Hornet1", {3, 3});
    queue(b, h, {3, 2});
    set_building(b, {3, 2});
    TurnContext ctx = context();
    ctx.grid_resist = [resist](Point, int) { return resist; };
    const PhaseResult r = E.end_turn(b, ctx);
    CHECK(b.grid_power == (resist ? 7 : 6));
    const auto it = std::find_if(r.chances.begin(), r.chances.end(),
                                 [](const ChanceRecord& c) { return c.kind == ChanceKind::GridDefense; });
    REQUIRE(it != r.chances.end());
    CHECK(it->outcome == (resist ? 1 : 0));
  }
}

TEST_CASE("chance: an unrecorded wind direction is a two-way choice") {
  NEED_ENGINE();
  for (int branch : {0, 1}) {
    Board b;
    const int32_t v = place(b, "Scorpion1", {2, 4});
    TurnContext ctx = context("Mission_Wind");
    for (int y = 0; y < 8; ++y) danger(ctx, {{2, y}, {5, y}});
    ctx.choose = [branch](const ChanceRecord&) { return branch; };
    const PhaseResult r = E.end_turn(b, ctx);
    CHECK(P(b, v).pos == (branch == 0 ? Point{2, 5} : Point{2, 3}));
    CHECK_FALSE(r.exact);
    REQUIRE(!r.chances.empty());
    CHECK(r.chances.front().kind == ChanceKind::EnvChoice);
  }
}

TEST_CASE("chance: lightning on empty tiles has a single order") {
  NEED_ENGINE();
  Board b;
  place(b, "Scorpion1", {1, 1});
  TurnContext ctx = context("Mission_Lightning");
  danger(ctx, {{1, 1}, {5, 2}, {1, 5}, {6, 6}});
  const PhaseResult r = E.end_turn(b, ctx);
  CHECK(std::none_of(r.chances.begin(), r.chances.end(),
                     [](const ChanceRecord& c) { return c.kind == ChanceKind::EnvOrder; }));
  CHECK(r.count(PhaseEventType::EnvStep) == 4);
}

TEST_CASE("chance: seismic path direction decided by last turn's chasm") {
  NEED_ENGINE();
  Board b;
  b.tile({1, 3}).terrain = Terrain::Hole;  // last turn's end of the path
  const int32_t a = place(b, "Scorpion1", {2, 3});
  place(b, "Scorpion1", {4, 3});
  TurnContext ctx = context("Mission_Crack");
  danger(ctx, {{2, 3}, {3, 3}, {4, 3}});
  const PhaseResult r = E.end_turn(b, ctx);
  CHECK(r.chances.empty());
  CHECK(dead(b, a));
  for (int x = 2; x <= 4; ++x) CHECK(b.tile({x, 3}).terrain == Terrain::Hole);
}

TEST_CASE("chance: Mission_Reactivation thaws two of three frozen Vek") {
  NEED_ENGINE();
  for (int branch = 0; branch < 3; ++branch) {
    Board b;
    std::vector<int32_t> vek;
    for (int x : {1, 3, 5}) {
      vek.push_back(place(b, "Scorpion1", {x, 1}));
      P(b, vek.back()).frozen = true;
    }
    TurnContext ctx = context("Mission_Reactivation");
    ctx.choose = [branch](const ChanceRecord&) { return branch; };
    const PhaseResult r = E.end_turn(b, ctx);
    int thawed = 0;
    for (int32_t uid : vek) thawed += P(b, uid).frozen ? 0 : 1;
    CHECK(thawed == 2);
    REQUIRE(r.chances.size() == 1);
    CHECK(r.chances[0].kind == ChanceKind::MissionRandom);
    CHECK(r.chances[0].options == 3);
  }
}

TEST_CASE("chance: the final cave's destroyed bomb drops again at a random tile") {
  NEED_ENGINE();
  Board b;
  const int32_t bomb = place(b, "BigBomb", {3, 3});
  hp(b, bomb, 1);
  const int32_t h = place(b, "Hornet1", {3, 4});
  queue(b, h, {3, 3});
  TurnContext ctx = context("Mission_Final_Cave");
  ctx.mission.final_cave = FinalEnvState{true, 1, 1, false, {}};
  const int limit = b.total_turns;
  const PhaseResult r = E.end_turn(b, ctx);
  const auto it = std::find_if(r.chances.begin(), r.chances.end(),
                               [](const ChanceRecord& c) { return c.kind == ChanceKind::MissionRandom; });
  REQUIRE(it != r.chances.end());
  CHECK(it->options > 1);
  CHECK(b.total_turns == limit + 2);
  int bombs = 0;
  for (const Pawn& p : b.pawns()) bombs += symbol_name(p.type) == "BigBomb" && p.alive() ? 1 : 0;
  CHECK(bombs == 1);
}

// ---- Environment coverage on recorded boards ---------------------------------------------

namespace {

std::optional<Recording> recorded(const std::string& rel) {
  const std::filesystem::path path = std::filesystem::path(ITB_REPO_ROOT) / "recordings" / rel;
  if (!std::filesystem::exists(path)) return std::nullopt;
  std::string error;
  return load_recording(path, &engine()->data(), &error);
}

}  // namespace

TEST_CASE("every recorded environment runs natively") {
  NEED_ENGINE();
  struct Case {
    const char* file;
    const char* env;
  };
  const Case cases[] = {
      {"20260518_121221_583/m00_turn_01_solve_input.json", "Env_Airstrike"},
      {"20260508_134925_472/m04_turn_01_solve_input.json", "Env_Cataclysm"},
      {"20260506_114649_974/m02_turn_01_solve_input.json", "Env_Seismic"},
      {"20260517_105759_344/m08_turn_01_solve_input.json", "Env_Tides"},
      {"20260510_000847_773/m01_turn_01_solve_input.json", "Env_Terratide"},
      {"20260512_104120_903/m02_turn_01_solve_input.json", "Env_RandomWind"},
      {"20260430_230740_117/m02_turn_01_solve_input.json", "Env_SnowStorm"},
      {"20260430_230740_117/m04_turn_01_solve_input.json", "Env_Volcano"},
      {"20260430_230740_117/m05_turn_01_solve_input.json", "Env_Final"},
      {"20260512_104120_903/m05_turn_01_solve_input.json", "Env_BeltLine"},
      {"20260506_114649_974/m21_turn_01_solve_input.json", "Env_BeltRandom"},
  };
  int ran = 0;
  for (const Case& c : cases) {
    auto rec = recorded(c.file);
    if (!rec) continue;
    Board b = rec->board;
    TurnContext ctx;
    ctx.mission = rec->mission;
    const PhaseResult r = engine()->end_turn(b, ctx);
    CHECK_MESSAGE(r.environment == c.env, c.file);
    ++ran;
    CHECK_MESSAGE(r.count(PhaseEventType::EnvUnsupported) == 0, c.file);
    CHECK_MESSAGE(r.count(PhaseEventType::EnvStep) >= 1, c.file);
    CHECK_MESSAGE(r.quiescent, c.file);
  }
  if (ran == 0) WARN_MESSAGE(false, "no recorded environment boards found");
}

TEST_CASE("every mission id in the recordings maps to a native environment") {
  NEED_ENGINE();
  const std::filesystem::path root = std::filesystem::path(ITB_REPO_ROOT) / "recordings";
  if (!std::filesystem::exists(root)) {
    WARN_MESSAGE(false, "no recordings directory");
    return;
  }
  std::map<std::string, std::string> seen;
  int boards = 0;
  for (const auto& e : std::filesystem::recursive_directory_iterator(root)) {
    const std::string name = e.path().filename().string();
    if (!e.is_regular_file() || !name.ends_with("_solve_input.json")) continue;
    std::string error;
    auto rec = load_recording(e.path(), &engine()->data(), &error);
    if (!rec) continue;
    ++boards;
    Board b = rec->board;
    TurnContext ctx;
    ctx.mission = rec->mission;
    const PhaseResult r = engine()->end_turn(b, ctx);
    CHECK_MESSAGE(r.count(PhaseEventType::EnvUnsupported) == 0, e.path().string());
    CHECK_MESSAGE(r.quiescent, e.path().string());
    seen[rec->mission_id] = r.environment;
  }
  CHECK(boards > 400);
  CHECK(seen["Mission_SnowStorm"] == "Env_SnowStorm");
  CHECK(seen["Mission_Final"] == "Env_Volcano");
  CHECK(seen["Mission_Final_Cave"] == "Env_Final");
  CHECK(seen["Mission_BeltRandom"] == "Env_BeltRandom");
  CHECK(seen["Mission_AcidStorm"] == "Mission_AcidStorm");
}

// ---- The Psion Abomination's +1 HP -------------------------------------------------
//
// Pawn::ComputeHealthTotal (0x00878f30) adds +1 max HP under mutation 1
// (Soldier psion) or 7 (Psion Abomination). When the Abomination dies,
// UpdateLeaders gives every Vek SetMutation(0), which lowers max and current
// HP by 1 (SetMutation 0x00879350); the regeneration tick earlier in the
// phase still healed while it lived.

TEST_CASE("live: the Psion Abomination's death takes its +1 HP from every Vek (2026-10-10)") {
  NEED_ENGINE();
  // Mission_JellyBoss turn 2, End Turn: Jelly_Boss#319 3/5 at (4,5); Burnbug1
  // #320 4/4, Scorpion2 #321 5/6 (queued on the boss's tile), Burnbug1 #323
  // 3/4. Enemy phase: regen #321 6/6 and #323 4/4; the Scorpion's sting (3)
  // kills the boss -> every Vek -1 (3/3, 5/5, 3/3); the blocked emerge under
  // #321 deals 1 -> 4/5. The game's turn 3 board: 3/3, 4/5, 3/3.
  const Recording before = live_fixture("live_jellyboss_t2_end_turn.json");
  const Recording after = live_fixture("live_jellyboss_t3_start.json");
  REQUIRE(before.mission.mission_id == "Mission_JellyBoss");
  REQUIRE(before.board.psion == Leader::Boss);
  const int32_t boss = 319;
  REQUIRE(before.board.find_pawn(boss) != nullptr);
  CHECK_FALSE(before.board.find_pawn(boss)->health_bonus);
  for (int32_t uid : {320, 321, 323}) {
    REQUIRE(before.board.find_pawn(uid) != nullptr);
    CHECK(before.board.find_pawn(uid)->health_bonus);
  }

  Board b = before.board;
  E.end_turn(b, turn_context(before, Visibility::Full));
  CHECK(dead(b, boss));
  CHECK(b.psion == Leader::None);
  for (int32_t uid : {320, 321, 323}) {
    CAPTURE(uid);
    const Pawn* game = after.board.find_pawn(uid);
    REQUIRE(game != nullptr);
    CHECK(hp_of(b, uid) == game->hp);
    CHECK(P(b, uid).max_hp == game->max_hp);
    CHECK_FALSE(P(b, uid).health_bonus);
  }
  CHECK(hp_of(b, 320) == 3);
  CHECK(hp_of(b, 321) == 4);
  CHECK(hp_of(b, 323) == 3);
}

TEST_CASE("the Psion Abomination: +1 HP while it lives, gone when it dies, a Soldier psion keeps it") {
  NEED_ENGINE();
  Board b;
  const int32_t boss = place(b, "Jelly_Boss", {5, 5});
  const int32_t v = place(b, "Scorpion1", {1, 6});
  hp(b, v, 1, 4);
  set_psion(b);
  // The regeneration tick runs first (1/4 -> 2/4); lightning then kills the
  // Abomination and its +1 goes: 1/3.
  TurnContext ctx = context("Mission_Lightning");
  danger(ctx, {{5, 5}});
  E.end_turn(b, ctx);
  CHECK(dead(b, boss));
  CHECK(hp_of(b, v) == 1);
  CHECK(P(b, v).max_hp == 3);

  // Killed in the player's turn (no regeneration first): a Vek that only
  // lived on the +1 dies with it.
  Board d;
  const int32_t boss3 = place(d, "Jelly_Boss", {3, 3});
  hp(d, boss3, 1);
  const int32_t x = place(d, "Scorpion1", {1, 6});
  hp(d, x, 1, 4);
  const int32_t m = mech(d, {3, 2});
  set_psion(d);
  REQUIRE(E.fire_weapon(d, m, "Prime_Punchmech", {3, 3}, std::nullopt, action_options(context())).ok());
  CHECK(dead(d, boss3));
  CHECK(dead(d, x));

  // A Soldier psion listed later takes over: the +1 stays.
  Board c;
  const int32_t boss2 = place(c, "Jelly_Boss", {5, 5});
  const int32_t w = place(c, "Scorpion1", {1, 6});
  hp(c, w, 1, 4);
  place(c, "Jelly_Health1", {6, 1});
  set_psion(c);
  REQUIRE(c.psion == Leader::Health);
  E.end_turn(c, ctx);
  CHECK(dead(c, boss2));
  CHECK(hp_of(c, w) == 1);
  CHECK(P(c, w).max_hp == 4);
}

// ---- "Temp Units Die" (Board::KillTempUnits) ----------------------------------------

namespace {

int event_index(const PhaseResult& r, PhaseEventType type) {
  for (size_t i = 0; i < r.events.size(); ++i) {
    if (r.events[i].type == type) return static_cast<int>(i);
  }
  return -1;
}

}  // namespace

TEST_CASE("temp units die after the spawns, as a plain death") {
  NEED_ENGINE();
  REQUIRE(E.data().pawn("DeployUnit_Bomby") != nullptr);
  CHECK(E.data().pawn("DeployUnit_Bomby")->temp_unit);
  CHECK_FALSE(E.data().pawn("PunchMech")->temp_unit);
  Board b;
  const int32_t m = mech(b, {3, 3});
  const int32_t bomb = place(b, "DeployUnit_Bomby", {3, 4});
  const int32_t v = place(b, "Scorpion1", {5, 4});
  const Board before = b;
  const PhaseResult r = E.end_turn(b, context());
  CHECK(dead(b, bomb));
  CHECK(r.count(PhaseEventType::TempUnitDied) == 1);
  // No self-destruct: the neighbours are untouched, and no corpse is left.
  CHECK(hp_of(b, m) == 3);
  CHECK(hp_of(b, v) == 3);
  const Pawn* left = b.find_pawn(bomb);
  CHECK((left == nullptr || !is_corpse(b, *left)));
  CHECK_FALSE(b.tile({3, 4}).acid);
  // Not a mech loss, not a Vek kill.
  const Score s = score_turn(before, b);
  CHECK(s[ScoreKey::MechsLost] == 0);
  CHECK(s[ScoreKey::MechHpLost] == 0);
  CHECK(s[ScoreKey::VekKilled] == 0);
}

TEST_CASE("temp units die after the Vek attacks and the spawn bumps") {
  NEED_ENGINE();
  Board b;
  mech(b, {6, 6});
  // A 2-HP bomb on a spawn tile: the blocked emerge hits it first (2 -> 1),
  // then it dies as a temp unit.
  const int32_t bomb = place(b, "DeployUnit_Bomby", {3, 3});
  hp(b, bomb, 2, 2);
  b.spawn_points = {{3, 3}};
  // A second bomb a Hornet kills with its queued sting: no temp-unit death.
  const int32_t stung = place(b, "DeployUnit_Bomby", {1, 2});
  const int32_t h = place(b, "Hornet1", {1, 1});
  queue(b, h, {1, 2});
  TurnContext ctx = context();
  ctx.spawn_types = {"Scorpion1"};
  const PhaseResult r = E.end_turn(b, ctx);
  CHECK(fired(r).count(h) == 1);
  CHECK(dead(b, bomb));
  CHECK(dead(b, stung));
  CHECK(r.count(PhaseEventType::TempUnitDied) == 1);
  const int blocked = event_index(r, PhaseEventType::SpawnBlocked);
  const int died = event_index(r, PhaseEventType::TempUnitDied);
  REQUIRE(blocked >= 0);
  REQUIRE(died >= 0);
  CHECK(r.events[static_cast<size_t>(blocked)].uid == bomb);
  CHECK(died > blocked);
  CHECK(r.events[static_cast<size_t>(died)].uid == bomb);
  CHECK(b.spawn_points == std::vector<Point>{{3, 3}});
}

TEST_CASE("temp units: any team; an ACID one leaves its pool") {
  NEED_ENGINE();
  Board b;
  mech(b, {6, 6});
  const int32_t bomb = place(b, "DeployUnit_Bomby", {2, 2});
  P(b, bomb).acid = true;
  const int32_t enemy_bomb = place(b, "DeployUnit_Bomby", {4, 4});
  P(b, enemy_bomb).team = Team::Enemy;
  const PhaseResult r = E.end_turn(b, context());
  CHECK(dead(b, bomb));
  CHECK(dead(b, enemy_bomb));
  CHECK(r.count(PhaseEventType::TempUnitDied) == 2);
  CHECK(b.tile({2, 2}).acid);
  CHECK_FALSE(b.tile({4, 4}).acid);
}

TEST_CASE("temp units survive the final enemy phase: the mission ends first") {
  NEED_ENGINE();
  Board b;
  b.turn = 4;
  b.total_turns = 4;
  mech(b, {6, 6});
  const int32_t bomb = place(b, "DeployUnit_Bomby", {2, 2});
  const PhaseResult r = E.end_turn(b, context());
  CHECK(r.mission_ended);
  CHECK_FALSE(dead(b, bomb));
  CHECK(r.count(PhaseEventType::TempUnitDied) == 0);
}

TEST_CASE("live: an unused walking bomb is gone after the enemy phase (2026-10-11 m71)") {
  NEED_ENGINE();
  // Mission_Artillery turn 3, End Turn: the plan left DeployUnit_Bomby #2323
  // at (7,4) next to PulseMech #2 at (7,3). The game's turn 4 board has no
  // bomb and every player unit at its HP (no self-destruct).
  const Recording before = live_fixture("live_bomb_t3_end_turn.json");
  const Recording after = live_fixture("live_bomb_t4_after_enemy.json");
  const int32_t bomb = 2323;
  REQUIRE(before.board.find_pawn(bomb) != nullptr);
  REQUIRE(before.board.find_pawn(bomb)->alive());
  CHECK(after.board.find_pawn(bomb) == nullptr);
  Board b = before.board;
  const PhaseResult r = E.end_turn(b, turn_context(before, Visibility::Full));
  CHECK(r.count(PhaseEventType::TempUnitDied) == 1);
  CHECK(dead(b, bomb));
  for (const Pawn& p : before.board.pawns()) {
    if (p.uid == bomb || !p.alive() || p.team != Team::Player) continue;
    CAPTURE(p.uid);
    const Pawn* game = after.board.find_pawn(p.uid);
    REQUIRE(game != nullptr);
    CHECK(hp_of(b, p.uid) == game->hp);
  }
}
