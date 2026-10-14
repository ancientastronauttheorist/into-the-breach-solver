// Deployable units: the support weapons that drop a player-controlled tank
// (weapons_deploy.lua: DeploySkill_Tank / _AcidTank / _ShieldTank / _PullTank
// / _IceTank and their _A / _B / _AB upgrades). Rules, from the game's Lua and
// the build:
//
// - The weapon is `Deployable` (weapons_base.lua): a LineArtillery target area
//   (2..8 tiles in a line) with OnlyEmpty (no tile blocked for ground units),
//   Limited = 1 (one use per battle); its effect is one artillery shot whose
//   SpaceDamage carries sPawn = self.Deployed. The upgrades are pawn types
//   (Deploy_TankA = +2 Health, Deploy_TankB = the 2-damage shot, ...), so the
//   spawned unit's type alone carries them.
// - Board::DamageSpace (B6) creates the pawn with its type's default team
//   (TEAM_PLAYER) and Board::AddPawn sets it active (Pawn::SetActive(true),
//   which only a Move-only pawn with no move speed refuses): it may move and
//   fire in the turn it lands, like a mech, and every later turn.
// - It is not a mech (Pawn::SetMech is for squad units only): its death is no
//   mech loss and it has no pilot or Repair.
// - Vek score it like a mech (Skill:ScoreList: any pawn of an enemy team that
//   is neither dead nor a TempUnit is ScoreEnemy).

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "itb/ai_phase.hpp"
#include "itb/engine.hpp"
#include "itb/game_data.hpp"
#include "itb/objectives.hpp"
#include "itb/recording.hpp"
#include "itb/score.hpp"
#include "itb/solver.hpp"

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
    pawn.active = true;
  }
  b.add_pawn(pawn);
  if (b.next_uid <= uid) b.next_uid = uid + 1;
  return uid;
}

// A mech carrying `weapon` in slot 0 with its one use (Limited = 1).
int32_t deployer(Board& b, const char* weapon, Point p) {
  const int32_t m = place(b, "ArtiMech", p, true);
  Pawn& mech = *b.find_pawn(m);
  mech.weapons = {};
  mech.weapons[0] = intern(weapon);
  mech.uses[0] = 1;
  return m;
}

const Pawn* at(const Board& b, Point p) {
  for (const Pawn& q : b.pawns()) {
    if (q.pos == p && q.alive()) return &q;
  }
  return nullptr;
}

TurnContext context() {
  TurnContext ctx;
  ctx.mission.mission_id = "";
  return ctx;
}

}  // namespace

TEST_CASE("deployables: each weapon drops its upgraded tank, a player unit that is not a mech") {
  NEED_ENGINE();
  struct Case {
    const char* weapon;
    const char* type;
    int hp;
    const char* shot;
    bool flying;
  };
  // weapons_deploy.lua. The Ice Tank's A and B upgrades point at each other's
  // pawns (both 2 HP); the type is what counts.
  const Case cases[] = {
      {"DeploySkill_Tank", "Deploy_Tank", 1, "Deploy_TankShot", false},
      {"DeploySkill_Tank_A", "Deploy_TankA", 3, "Deploy_TankShot", false},
      {"DeploySkill_Tank_B", "Deploy_TankB", 1, "Deploy_TankShot2", false},
      {"DeploySkill_Tank_AB", "Deploy_TankAB", 3, "Deploy_TankShot2", false},
      {"DeploySkill_AcidTank", "Deploy_AcidTank", 1, "Deploy_AcidTankShot", false},
      {"DeploySkill_AcidTank_AB", "Deploy_AcidTankAB", 3, "Deploy_AcidTankShot2", false},
      {"DeploySkill_ShieldTank", "Deploy_ShieldTank", 1, "Deploy_ShieldTankShot", false},
      {"DeploySkill_ShieldTank_B", "Deploy_ShieldTankB", 1, "Deploy_ShieldTankShot2", false},
      {"DeploySkill_PullTank", "Deploy_PullTank", 1, "Deploy_PullTankShot", false},
      {"DeploySkill_PullTank_B", "Deploy_PullTankB", 1, "Deploy_PullTankShot", true},
      {"DeploySkill_PullTank_AB", "Deploy_PullTankAB", 3, "Deploy_PullTankShot", true},
      {"DeploySkill_IceTank_A", "Deploy_IceTankB", 2, "Deploy_IceTankShot", false},
      {"DeploySkill_IceTank_AB", "Deploy_IceTankAB", 3, "Deploy_IceTankShot", false},
  };
  for (const Case& c : cases) {
    INFO(c.weapon);
    Board b;
    const int32_t m = deployer(b, c.weapon, {3, 6});
    const int32_t next = b.next_uid;
    const ActionResult r = E.fire_weapon(b, m, 0, {3, 3});
    REQUIRE_MESSAGE(r.ok(), to_string(r.status));
    CHECK(r.lua_errors.empty());
    const Pawn* t = at(b, {3, 3});
    REQUIRE(t != nullptr);
    CHECK(t->uid == next);
    CHECK(symbol_name(t->type) == c.type);
    CHECK(t->team == Team::Player);
    CHECK(t->controlled());
    CHECK_FALSE(t->mech);
    CHECK(t->hp == c.hp);
    CHECK(t->max_hp == c.hp);
    CHECK(t->flying == c.flying);
    CHECK(t->active);  // Board::AddPawn: it may act this turn
    CHECK(symbol_name(t->weapons[0]) == c.shot);
    CHECK(t->weapons[1] == kNoSymbol);
    // One use per battle.
    CHECK(b.find_pawn(m)->uses[0] == 0);
    CHECK_FALSE(b.find_pawn(m)->active);
  }
}

TEST_CASE("deployables: the drop tile is 2+ tiles away in a line and must be empty") {
  NEED_ENGINE();
  Board b;
  const int32_t m = deployer(b, "DeploySkill_Tank", {3, 6});
  place(b, "Scorpion1", {3, 2});
  set_mountain(b, {1, 6});
  set_building(b, {5, 6});
  b.tile({3, 4}).terrain = Terrain::Water;
  b.tile({6, 6}).terrain = Terrain::Hole;
  const std::vector<Point> area = E.lua().target_area(b, *b.find_pawn(m), "DeploySkill_Tank", {3, 6});
  auto in = [&](Point p) { return std::find(area.begin(), area.end(), p) != area.end(); };
  CHECK_FALSE(in({3, 5}));  // adjacent
  CHECK(in({3, 3}));
  CHECK_FALSE(in({3, 2}));  // a Vek
  CHECK(in({3, 1}));        // past it: artillery
  CHECK_FALSE(in({1, 6}));  // mountain
  CHECK_FALSE(in({5, 6}));  // building
  CHECK_FALSE(in({4, 5}));  // not in line
  // Water and chasms block ground pathing: no drop there.
  CHECK_FALSE(in({3, 4}));
  CHECK_FALSE(in({6, 6}));
  CHECK(E.fire_weapon(b, m, 0, {3, 2}).status == ActionStatus::NotInArea);
  // Without its use left it cannot fire.
  b.find_pawn(m)->uses[0] = 0;
  CHECK(E.fire_weapon(b, m, 0, {3, 3}).status == ActionStatus::NoUses);
}

TEST_CASE("deployables: a tank dropped this turn moves and fires, then is done") {
  NEED_ENGINE();
  Board b;
  const int32_t m = deployer(b, "DeploySkill_Tank_B", {1, 6});
  const int32_t vek = place(b, "Scorpion1", {5, 2});
  REQUIRE(E.fire_weapon(b, m, 0, {1, 3}).ok());
  const int32_t tank = at(b, {1, 3})->uid;
  // Move 3 (Deploy_Tank MoveSpeed), then the 2-damage pushing shot.
  REQUIRE(E.move(b, tank, {1, 2}).ok());
  const int hp = b.find_pawn(vek)->hp;
  const ActionResult r = E.fire_weapon(b, tank, 0, {4, 2});
  REQUIRE_MESSAGE(r.ok(), to_string(r.status));
  const Pawn& v = *b.find_pawn(vek);
  CHECK(v.hp == hp - 2);
  CHECK(v.pos == Point{6, 2});  // pushed away from the tank
  const Pawn& t = *b.find_pawn(tank);
  CHECK_FALSE(t.active);
  CHECK(E.move(b, tank, {1, 1}).status == ActionStatus::CannotAct);
  CHECK(E.fire_weapon(b, tank, 0, {4, 2}).status == ActionStatus::CannotAct);
}

TEST_CASE("deployables: the tanks' own shots (ACID, shield, pull, freeze)") {
  NEED_ENGINE();
  SUBCASE("A.C.I.D. Tank: ACID on the first thing in line; the B shot also pushes") {
    for (const char* type : {"Deploy_AcidTank", "Deploy_AcidTankB"}) {
      INFO(type);
      Board b;
      const int32_t t = place(b, type, {1, 2});
      b.find_pawn(t)->active = true;
      const int32_t vek = place(b, "Scorpion1", {4, 2});
      const ActionResult r = E.fire_weapon(b, t, 0, {4, 2});
      REQUIRE_MESSAGE(r.ok(), to_string(r.status));
      const Pawn& v = *b.find_pawn(vek);
      CHECK(v.acid);
      CHECK(v.hp == v.max_hp);  // no damage
      CHECK(v.pos == (std::string(type) == "Deploy_AcidTankB" ? Point{5, 2} : Point{4, 2}));
    }
  }
  SUBCASE("Shield-Tank: shields the adjacent tile; the B shot is a projectile") {
    Board b;
    const int32_t t = place(b, "Deploy_ShieldTank", {2, 2});
    b.find_pawn(t)->active = true;
    set_building(b, {2, 1});
    REQUIRE(E.fire_weapon(b, t, 0, {2, 1}).ok());
    CHECK(b.tile({2, 1}).shield);
    Board b2;
    const int32_t t2 = place(b2, "Deploy_ShieldTankB", {2, 6});
    b2.find_pawn(t2)->active = true;
    const int32_t mech = place(b2, "PunchMech", {2, 2}, true);
    REQUIRE(E.fire_weapon(b2, t2, 0, {2, 2}).ok());
    CHECK(b2.find_pawn(mech)->shield);
  }
  SUBCASE("Pull-Tank: pulls the first thing in line one tile") {
    Board b;
    const int32_t t = place(b, "Deploy_PullTank", {1, 2});
    b.find_pawn(t)->active = true;
    const int32_t vek = place(b, "Scorpion1", {5, 2});
    REQUIRE(E.fire_weapon(b, t, 0, {5, 2}).ok());
    CHECK(b.find_pawn(vek)->pos == Point{4, 2});
  }
  SUBCASE("Ice Tank: freezes the first thing in line") {
    Board b;
    const int32_t t = place(b, "Deploy_IceTank", {1, 2});
    b.find_pawn(t)->active = true;
    const int32_t vek = place(b, "Scorpion1", {5, 2});
    REQUIRE(E.fire_weapon(b, t, 0, {5, 2}).ok());
    CHECK(b.find_pawn(vek)->frozen);
  }
}

TEST_CASE("deployables: a deployed tank acts again on later turns") {
  NEED_ENGINE();
  Board b;
  b.turn = 1;
  const int32_t m = deployer(b, "DeploySkill_Tank", {1, 6});
  REQUIRE(E.fire_weapon(b, m, 0, {1, 3}).ok());
  const int32_t tank = at(b, {1, 3})->uid;
  REQUIRE(E.move(b, tank, {2, 3}).ok());
  const TurnContext ctx = context();
  E.end_turn(b, ctx);
  // The next player turn starts (BoardPlayer::ActivatePawns): every player
  // unit is active again. The engine starts a turn from the loaded board;
  // a recorded tank's `active` comes from the bridge (see the loader test).
  const Pawn* t = b.find_pawn(tank);
  REQUIRE(t != nullptr);
  CHECK(t->alive());
  CHECK_FALSE(t->active);  // Pawn::EndTurn
}

TEST_CASE("deployables: a deployed tank's death is no mech loss") {
  NEED_ENGINE();
  Board before;
  place(before, "PunchMech", {6, 6}, true);
  const int32_t tank = place(before, "Deploy_TankA", {2, 2});
  Board after = before;
  after.find_pawn(tank)->hp = 0;
  const Score s = score_turn(before, after, nullptr, nullptr);
  CHECK(s[ScoreKey::MechsLost] == 0);
  CHECK(s[ScoreKey::MechHpLost] == 0);
  CHECK(s[ScoreKey::VekKilled] == 0);
  // It is not an objective unit either.
  CHECK(s[ScoreKey::ObjectivesFailed] == 0);
  // The position tier keeps it: +2 alive, +1 per HP, so a dead tank never
  // beats a living one, even next to a Vek (-1 there).
  CHECK(is_deployed_unit(*before.find_pawn(tank)));
  const PositionTerms alive = position_terms(before);
  CHECK(alive.deployed == 1);
  CHECK(alive.deployed_hp == 3);
  Board threatened = before;
  place(threatened, "Scorpion1", {2, 3});
  const Score near = score_turn(before, threatened, nullptr, nullptr);
  CHECK(near[ScoreKey::Position] > s[ScoreKey::Position]);
  // Neither the Bombling's bomb (a TempUnit) nor a mech is a deployed unit.
  Board other;
  const int32_t bomb = place(other, "DeployUnit_Bomby", {1, 1});
  const int32_t mech = place(other, "PunchMech", {1, 2}, true);
  CHECK_FALSE(is_deployed_unit(*other.find_pawn(bomb)));
  CHECK_FALSE(is_deployed_unit(*other.find_pawn(mech)));
  const int32_t spider = place(other, "DeployUnit_Aracnoid", {1, 3});
  CHECK(is_deployed_unit(*other.find_pawn(spider)));
}

TEST_CASE("deployables: the solver keeps a deployed tank out of a Vek's way") {
  NEED_ENGINE();
  Board b;
  b.turn = 2;
  const int32_t tank = place(b, "Deploy_Tank", {3, 3});
  b.find_pawn(tank)->active = true;
  const int32_t vek = place(b, "Scorpion1", {3, 5});
  b.find_pawn(vek)->queued = QueuedShot{0, {3, 4}, {3, 3}};
  b.find_pawn(vek)->pos = {3, 4};
  const TurnContext ctx = context();
  const auto idle = evaluate_plan(E, b, ctx, {});
  REQUIRE(idle);
  SolveOptions o;
  o.time_limit_s = 60;
  const SolveResult r = solve_turn(E, b, ctx, o);
  CHECK(r.proven_optimal);
  CHECK(r.best.worst_case[ScoreKey::Position] > (*idle)[ScoreKey::Position]);
  Board end = b;
  const TurnResult played = E.play_turn(end, r.best.actions, ctx);
  CHECK(played.refused < 0);
  const Pawn* t = end.find_pawn(tank);
  REQUIRE(t != nullptr);
  CHECK(t->alive());
}

TEST_CASE("deployables: Vek target a deployed tank like a mech") {
  NEED_ENGINE();
  Board b;
  b.turn = 1;
  const int32_t tank = place(b, "Deploy_Tank", {3, 3});
  b.find_pawn(tank)->active = false;
  const int32_t vek = place(b, "Scorpion1", {6, 3});
  AiPhaseOptions o;
  o.seed = 12345;
  const AiPhaseResult r = E.ai_phase(b, o);
  REQUIRE(r.pawns.size() == 1);
  const AiPawnResult& pr = r.pawns.front();
  CHECK(pr.uid == vek);
  CHECK(pr.plan.target == Point{3, 3});
  CHECK(pr.plan.target_score > 0);
  CHECK(b.find_pawn(vek)->queued.target == Point{3, 3});
}

TEST_CASE("deployables: the solver drops a tank and fires it in the same turn") {
  NEED_ENGINE();
  // A Hornet (2 HP) queued on a building, out of the mech's reach: only the
  // tank it drops can kill it (Deploy_TankShot2: 2 damage).
  Board b;
  b.turn = 1;
  const int32_t m = deployer(b, "DeploySkill_Tank_B", {0, 7});
  b.find_pawn(m)->move = 0;
  b.find_pawn(m)->movement.pilot_bonus = 0;
  const int32_t h = place(b, "Hornet1", {5, 3});
  b.find_pawn(h)->queued = QueuedShot{0, {5, 3}, {5, 2}};
  set_building(b, {5, 2});
  const TurnContext ctx = context();
  const auto idle = evaluate_plan(E, b, ctx, {});
  REQUIRE(idle);
  CHECK((*idle)[ScoreKey::GridLost] == -1);
  SolveOptions o;
  o.time_limit_s = 60;
  const SolveResult r = solve_turn(E, b, ctx, o);
  CHECK(r.proven_optimal);
  CHECK(r.best.worst_case[ScoreKey::GridLost] == 0);
  CHECK(r.best.worst_case[ScoreKey::VekKilled] == 1);
  REQUIRE(r.best.actions.size() >= 2);
  CHECK(r.best.actions[0].uid == m);
  CHECK(r.best.actions[0].weapon == "DeploySkill_Tank_B");
  const int32_t tank = b.next_uid;  // the engine numbers it next
  const auto fired = std::find_if(r.best.actions.begin(), r.best.actions.end(), [&](const PlayerAction& a) {
    return a.uid == tank && a.kind == PlayerAction::Kind::Weapon;
  });
  REQUIRE(fired != r.best.actions.end());
  CHECK(fired->weapon == "Deploy_TankShot2");
  const auto again = evaluate_plan(E, b, ctx, r.best.actions);
  REQUIRE(again);
  CHECK(*again == r.best.worst_case);
}
