// The next-turn threat metric (threat.hpp) and its score tier
// (ScoreKey::Threat, SolveOptions::threat_tier).

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <vector>

#include "itb/engine.hpp"
#include "itb/game_data.hpp"
#include "itb/score.hpp"
#include "itb/solver.hpp"
#include "itb/threat.hpp"

using namespace itb;

namespace {

void building(Board& b, Point p, int hp = 1) {
  Tile& t = b.tile(p);
  t.terrain = Terrain::Building;
  t.hp = t.max_hp = static_cast<int8_t>(hp);
  t.populated = true;
}

void mountain(Board& b, Point p) {
  Tile& t = b.tile(p);
  t.terrain = Terrain::Mountain;
  t.hp = t.max_hp = 2;
}

// An enemy with one weapon and a move speed, without the game's data.
int32_t enemy(Board& b, const char* weapon, Point p, int move = 0, int hp = 2) {
  Pawn q;
  int32_t uid = 0;
  for (const Pawn& o : b.pawns()) uid = std::max(uid, o.uid + 1);
  q.uid = uid;
  q.type = intern("TestVek");
  q.team = Team::Enemy;
  q.pos = p;
  q.hp = q.max_hp = static_cast<int8_t>(hp);
  q.move = static_cast<int8_t>(move);
  if (weapon) q.weapons[0] = intern(weapon);
  b.add_pawn(q);
  return uid;
}

int32_t mech(Board& b, Point p) {
  Pawn q;
  int32_t uid = 0;
  for (const Pawn& o : b.pawns()) uid = std::max(uid, o.uid + 1);
  q.uid = uid;
  q.type = intern("PunchMech");
  q.team = Team::Player;
  q.mech = true;
  q.corpse = true;
  q.pos = p;
  q.hp = q.max_hp = 3;
  q.move = 3;
  b.add_pawn(q);
  return uid;
}

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

}  // namespace

TEST_CASE("threat: reach classes from weapon names") {
  CHECK(threat_class(intern("ScorpionAtk1")).reach == ThreatReach::Footprint);
  CHECK(threat_class(intern("ScorpionAtk_Acid")).reach == ThreatReach::Footprint);
  CHECK(threat_class(intern("FireflyAtk2")).reach == ThreatReach::Projectile);
  CHECK(threat_class(intern("SnowtankAtk1_Player")).reach == ThreatReach::Projectile);
  CHECK(threat_class(intern("CentipedeAtkB")).reach == ThreatReach::ProjectileSplash);
  CHECK(threat_class(intern("SnowlaserAtk2")).reach == ThreatReach::Laser);
  CHECK(threat_class(intern("StarfishAtkB1")).reach == ThreatReach::Footprint);
  for (const char* none : {"SpiderAtk1", "BlobberAtk2", "ShamanAtkB", "WebeggHatch1", "SpiderlingHatch1",
                           "SnowmineAtk1", "Jelly_Health_Tooltip"}) {
    INFO(none);
    CHECK(threat_class(intern(none)).reach == ThreatReach::None);
  }
  CHECK(threat_class(kNoSymbol).reach == ThreatReach::None);
  // Unknown names: a wide footprint (every tile in line).
  const ThreatClass unknown = threat_class(intern("SomeModWeapon"));
  REQUIRE(unknown.reach == ThreatReach::Footprint);
  CHECK(((*unknown.hit)[Point(0, 0).index()] & (uint64_t{1} << Point(0, 7).index())) != 0);
}

TEST_CASE("threat: melee reach includes the move") {
  Board b;
  building(b, {3, 0});
  const int32_t near = enemy(b, "ScorpionAtk1", {3, 3}, 2);  // can step to (3,1)
  enemy(b, "ScorpionAtk1", {6, 6}, 3);                        // too far
  CHECK(threatening_enemies(b) == std::vector<int32_t>{near});
  CHECK(count_threats(b) == 1);

  // Frozen: no move, no attack. A frozen building is no target.
  Board frozen = b;
  frozen.find_pawn(near)->frozen = true;
  CHECK(count_threats(frozen) == 0);
  Board ice = b;
  ice.tile({3, 0}).frozen = true;
  CHECK(count_threats(ice) == 0);

  // A smoked tile is no place to attack from; an immobile Vek in smoke is no threat.
  Board smoke;
  building(smoke, {3, 0});
  const int32_t still = enemy(smoke, "ScorpionAtk1", {3, 1}, 0);
  CHECK(count_threats(smoke) == 1);
  smoke.tile({3, 1}).smoke = true;
  CHECK(count_threats(smoke) == 0);
  smoke.find_pawn(still)->ignore_smoke = true;
  CHECK(count_threats(smoke) == 1);

  // No weapon (a psion) or a weapon that never damages (an egg layer).
  Board none;
  building(none, {3, 0});
  enemy(none, nullptr, {3, 1});
  enemy(none, "SpiderAtk1", {3, 2});
  CHECK(count_threats(none) == 0);
  // No building: nothing to threaten.
  Board empty;
  enemy(empty, "ScorpionAtk1", {3, 1}, 3);
  CHECK(count_threats(empty) == 0);
}

TEST_CASE("threat: footprints") {
  // Burrower: the target tile and its two side tiles, so a diagonal building.
  Board burrow;
  building(burrow, {4, 4});
  enemy(burrow, "Burrower_Atk", {3, 3});
  CHECK(count_threats(burrow) == 1);
  Board scorp = burrow;
  scorp.pawns().clear();
  enemy(scorp, "ScorpionAtk1", {3, 3});
  CHECK(count_threats(scorp) == 0);
  // Starfish: diagonals only.
  Board star;
  building(star, {4, 4});
  enemy(star, "StarfishAtk1", {3, 3});
  CHECK(count_threats(star) == 1);
  Board star2;
  building(star2, {4, 3});
  enemy(star2, "StarfishAtk1", {3, 3});
  CHECK(count_threats(star2) == 0);
  // Artillery: range 2..5 in line, over anything in between; not adjacent.
  Board art;
  building(art, {0, 3});
  const int32_t a = enemy(art, "ScarabAtk1", {1, 3});
  CHECK(count_threats(art) == 0);
  art.find_pawn(a)->pos = {2, 3};
  mountain(art, {1, 3});
  CHECK(count_threats(art) == 1);
  art.find_pawn(a)->pos = {6, 3};
  CHECK(count_threats(art) == 0);  // range 6
  // Crab: the tile behind the target too (range 2..5, +1).
  Board crab = art;
  crab.pawns().clear();
  enemy(crab, "CrabAtk1", {6, 3});
  CHECK(count_threats(crab) == 1);
  // Hornet beta reaches the tile behind its target.
  Board hornet;
  building(hornet, {3, 0});
  enemy(hornet, "HornetAtk1", {3, 2});
  CHECK(count_threats(hornet) == 0);
  hornet.pawns().clear();
  enemy(hornet, "HornetAtk2", {3, 2});
  CHECK(count_threats(hornet) == 1);
}

TEST_CASE("threat: projectiles stop at the first obstacle that stays") {
  Board b;
  building(b, {0, 3});
  const int32_t fly = enemy(b, "FireflyAtk1", {5, 3});
  CHECK(count_threats(b) == 1);
  // A mech in the line stays there through the AI phase: it takes the shot.
  Board blocked = b;
  mech(blocked, {2, 3});
  CHECK(count_threats(blocked) == 0);
  // A Vek that will move does not count as cover; one that cannot move does.
  Board moving = b;
  enemy(moving, "ScorpionAtk1", {2, 3}, 3);
  CHECK(count_threats(moving) == 2);  // the Scorpion reaches the building too
  Board rooted = b;
  enemy(rooted, nullptr, {2, 3}, 0);
  CHECK(count_threats(rooted) == 0);
  // A mountain stops it; the shooter can move around it.
  Board hill = b;
  mountain(hill, {2, 3});
  CHECK(count_threats(hill) == 0);
  hill.find_pawn(fly)->move = 6;
  CHECK(count_threats(hill) == 1);  // around the hill to (1,3) or into column 0
  // Splash: a shot ending beside a building hits it.
  Board splash;
  building(splash, {0, 4});
  mountain(splash, {0, 3});
  enemy(splash, "CentipedeAtk1", {5, 3});
  CHECK(count_threats(splash) == 1);
  Board plain = splash;
  plain.pawns().clear();
  enemy(plain, "FireflyAtk1", {5, 3});
  CHECK(count_threats(plain) == 0);
  // Laser: through pawns, stopped by mountains.
  Board laser;
  building(laser, {0, 3});
  mech(laser, {2, 3});
  enemy(laser, "SnowlaserAtk1", {5, 3});
  CHECK(count_threats(laser) == 1);
  mountain(laser, {3, 3});
  CHECK(count_threats(laser) == 0);
}

TEST_CASE("threat: score tier and tier views") {
  Board before;
  building(before, {3, 0});
  enemy(before, "ScorpionAtk1", {3, 1});
  enemy(before, "FireflyAtk1", {0, 0});
  const Board after = before;
  const Score off = score_turn(before, after);
  CHECK(off[ScoreKey::Threat] == 0);
  ScoreOptions on;
  on.threat_tier = true;
  const Score s = score_turn(before, after, nullptr, nullptr, on);
  CHECK(s[ScoreKey::Threat] == -2);
  // Every other tier is the same.
  Score t = s;
  t[ScoreKey::Threat] = 0;
  CHECK(t == off);
  // Printing leaves the unused tier out.
  CHECK(off.describe().find("threat") == std::string::npos);
  CHECK(s.describe(true).find("threat=-2") != std::string::npos);
  CHECK(off.describe() == "grid=0 bldg_hp=0 mechs=0 obj_fail=0 mech_hp=0 obj=0 kills=0 vek_hp=0 position=-1");
  // Tier counts.
  CHECK(used_tier_count(false) == kScoreKeys - 1);
  CHECK(used_tier_count(true) == kScoreKeys);
  CHECK(components_for_tiers(4, false) == 4);
  CHECK(components_for_tiers(5, false) == 6);  // grid .. mech HP, Threat (0) inside
  CHECK(components_for_tiers(5, true) == 5);   // grid .. threat
  CHECK(components_for_tiers(9, false) == kScoreKeys);
  CHECK(components_for_tiers(0, true) == 0);
}

TEST_CASE("threat: the tier changes the plan; off, nothing changes") {
  if (!engine()) {
    WARN_MESSAGE(false, "no game install found; set ITB_GAME_DIR to run engine tests");
    return;
  }
  Engine& E = *engine();
  // A Punch Mech can kill either Vek. Off, the solver takes the one with
  // more HP (Vek HP removed); with the threat tier it kills the one standing
  // next to the building, the other cannot reach any building next turn.
  Board b;
  building(b, {3, 0});
  const PawnDef* def = E.data().pawn("PunchMech");
  REQUIRE(def != nullptr);
  Pawn m = E.data().make_pawn(*def, 0, {4, 4});
  m.mech = true;
  m.corpse = true;
  m.team = Team::Player;
  m.active = true;
  b.add_pawn(m);
  const PawnDef* vek = E.data().pawn("Scorpion1");
  REQUIRE(vek != nullptr);
  Pawn far = E.data().make_pawn(*vek, 1, {6, 6});
  far.hp = 2;
  far.move = 0;
  b.add_pawn(far);
  Pawn near = E.data().make_pawn(*vek, 2, {3, 1});
  near.hp = 1;
  b.add_pawn(near);
  TurnContext ctx;
  SolveOptions o;
  o.time_limit_s = 60;
  const SolveResult off = solve_turn(E, b, ctx, o);
  REQUIRE(off.proven_optimal);
  CHECK(off.best.worst_case[ScoreKey::Threat] == 0);
  CHECK(off.proven_components == used_tier_count(false));
  CHECK(off.best.worst_case[ScoreKey::VekKilled] == 1);
  CHECK(off.best.worst_case[ScoreKey::VekHpRemoved] == 2);
  o.threat_tier = true;
  const SolveResult on = solve_turn(E, b, ctx, o);
  REQUIRE(on.proven_optimal);
  CHECK(on.proven_components == used_tier_count(true));
  CHECK(on.best.worst_case[ScoreKey::Threat] == 0);
  CHECK(on.best.worst_case[ScoreKey::VekKilled] == 1);
  CHECK(on.best.worst_case[ScoreKey::VekHpRemoved] == 1);
  // The off plan, scored with the tier, leaves the threat standing.
  const auto again = evaluate_plan(E, b, ctx, off.best.actions, o);
  REQUIRE(again);
  CHECK((*again)[ScoreKey::Threat] == -1);
  // The adaptive budget counts tiers in use: 5 with the tier = through threat.
  SolveOptions a = o;
  a.time_limit_s = 1e-9;
  a.max_time_s = 60;
  a.min_proven_tiers = 5;
  const SolveResult r = solve_turn(E, b, ctx, a);
  CHECK(r.proven_components >= 5);
  CHECK(r.best.worst_case[ScoreKey::Threat] == 0);
}
