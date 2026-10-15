// The enemy AI's planning (ai_phase.hpp) and the deployment search
// (deploy.hpp). Engine tests are skipped (with a warning) without a game
// install.

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "itb/ai_phase.hpp"
#include "itb/deploy.hpp"
#include "itb/engine.hpp"
#include "itb/game_data.hpp"
#include "itb/recording.hpp"

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

void set_building(Board& b, Point p) {
  Tile& t = b.tile(p);
  t.terrain = Terrain::Building;
  t.hp = t.max_hp = 1;
  t.populated = true;
}

Recording fixture(const char* name) {
  std::string error;
  auto rec = load_recording(std::string(ITB_FIXTURE_DIR) + "/" + name, &engine()->data(), &error);
  REQUIRE_MESSAGE(rec.has_value(), error);
  return *rec;
}

bool adjacent(Point a, Point b) { return std::abs(a.x - b.x) + std::abs(a.y - b.y) == 1; }

}  // namespace

TEST_CASE("AiPlan order: a good position beats a bad one, then target score, then position") {
  const AiPlan bad_pos{{3, 3}, {3, 4}, 10, -2};
  const AiPlan good_pos{{3, 3}, {3, 4}, 5, 1};
  const AiPlan zero_pos{{3, 3}, {3, 4}, 5, 0};
  CHECK(ai_plan_at_least(good_pos, bad_pos));
  CHECK_FALSE(ai_plan_at_least(bad_pos, good_pos));
  // A zero position score is no "positive" position: target score decides.
  CHECK(ai_plan_at_least(bad_pos, zero_pos));
  CHECK(ai_plan_at_least(AiPlan{{1, 1}, {1, 2}, 6, 0}, AiPlan{{1, 1}, {1, 2}, 5, 5}));
  CHECK(ai_plan_at_least(AiPlan{{1, 1}, {1, 2}, 5, 5}, AiPlan{{2, 2}, {2, 3}, 5, 5}));  // equal: >=
  CHECK_FALSE(ai_plan_at_least(AiPlan{{1, 1}, {1, 2}, 5, 4}, AiPlan{{2, 2}, {2, 3}, 5, 5}));
}

TEST_CASE("deployment ranking: per tier the worst outcome, then the mean") {
  auto score = [](int grid, int mechs) {
    Score s;
    s[ScoreKey::GridLost] = grid;
    s[ScoreKey::MechsLost] = mechs;
    return s;
  };
  // a: never loses grid but sometimes a mech; b: loses grid once.
  const DeploySummary a = summarize({{score(0, -1), 1}, {score(0, 0), 3}});
  const DeploySummary b = summarize({{score(-1, 0), 1}, {score(0, 0), 3}});
  CHECK(a.outcomes == 2);
  CHECK(a.seeds == 4);
  CHECK(a.worst == score(0, -1));
  CHECK(a.tier_min[ScoreKey::MechsLost] == -1);
  CHECK(a.mean[static_cast<size_t>(ScoreKey::MechsLost)] == doctest::Approx(-0.25));
  CHECK(deploy_better(a, b));
  CHECK_FALSE(deploy_better(b, a));
  // Same worst grid: the mean decides.
  const DeploySummary c = summarize({{score(-1, 0), 3}, {score(0, 0), 1}});
  CHECK(deploy_better(b, c));
  CHECK_FALSE(deploy_better(a, a));
  // Sample seeds are valid generator states and differ.
  CHECK(deploy_sample_seed(1, 0) != deploy_sample_seed(1, 1));
  CHECK(deploy_sample_seed(1, 0) != deploy_sample_seed(2, 0));
  CHECK(deploy_sample_seed(7, 3) > 0u);
  CHECK(deploy_sample_seed(7, 3) < 0x7fffffffu);
}

TEST_CASE("AI phase: a lone Scorpion walks up to a building and queues its attack") {
  NEED_ENGINE();
  Board b;
  b.turn = 1;
  set_building(b, Point{1, 4});
  const int32_t vek = place(b, "Scorpion1", Point{5, 4});
  AiPhaseOptions o;
  o.seed = 12345;
  o.keep_plans = true;
  const AiPhaseResult r = E.ai_phase(b, o);
  REQUIRE(r.pawns.size() == 1);
  const AiPawnResult& pr = r.pawns.front();
  CHECK(pr.uid == vek);
  CHECK(pr.plan.target == Point{1, 4});
  CHECK(pr.plan.target_score == 5);  // ScoreBuilding
  CHECK(pr.queued);
  const Pawn& p = *b.find_pawn(vek);
  CHECK(adjacent(p.pos, Point{1, 4}));
  CHECK(p.pos == pr.plan.loc);
  CHECK(p.queued.active());
  CHECK(p.queued.target == Point{1, 4});
  CHECK_FALSE(p.active);
  // Its own tile was scored too (always a candidate).
  CHECK(std::any_of(pr.plans.begin(), pr.plans.end(), [](const AiPlan& q) { return q.loc == Point{5, 4}; }));
  // The same seed gives the same phase; the seed moves on.
  Board b2;
  b2.turn = 1;
  set_building(b2, Point{1, 4});
  place(b2, "Scorpion1", Point{5, 4});
  const AiPhaseResult r2 = E.ai_phase(b2, o);
  CHECK(b2 == b);
  CHECK(r2.seed == r.seed);
  CHECK(r.seed != o.seed);
}

TEST_CASE("AI phase: a Scorpion's web holds a mech but not a Disable_Immunity pilot (Camila Vera)") {
  NEED_ENGINE();
  // ScorpionAtk1 webs its target as it queues (AddGrapple in the instant
  // part). BoardSpace::OnLoop's web check breaks the web on a pawn with the
  // Disable_Immunity skill (IsGrappleImmunity), so her mech is free to move
  // in the next player turn; the queued attack itself still lands later.
  for (const bool immune : {false, true}) {
    Board b;
    b.turn = 1;
    const int32_t m = place(b, "PunchMech", Point{3, 4}, true);
    const int32_t vek = place(b, "Scorpion1", Point{3, 3});
    if (immune) b.find_pawn(m)->pilot_abilities = engine()->data().pilot_ability("Pilot_Soldier");
    AiPhaseOptions o;
    o.seed = 4242;
    const AiPhaseResult r = E.ai_phase(b, o);
    CHECK(r.lua_errors.empty());
    const Pawn& s = *b.find_pawn(vek);
    INFO("immune " << immune);
    REQUIRE(s.queued.active());
    REQUIRE(s.queued.target == Point{3, 4});
    const Pawn& mech = *b.find_pawn(m);
    CHECK(mech.webbed == !immune);
    CHECK((b.tile(Point{3, 4}).web_in > 0) == !immune);
  }
}

TEST_CASE("AI phase: frozen pawns and the player's units do not plan") {
  NEED_ENGINE();
  Board b;
  b.turn = 1;
  set_building(b, Point{1, 4});
  const int32_t m = place(b, "PunchMech", Point{6, 6}, true);
  const int32_t vek = place(b, "Scorpion1", Point{5, 4});
  b.find_pawn(vek)->frozen = true;
  AiPhaseOptions o;
  o.seed = 7;
  const AiPhaseResult r = E.ai_phase(b, o);
  CHECK(r.pawns.empty());
  CHECK(b.find_pawn(vek)->pos == Point{5, 4});
  CHECK_FALSE(b.find_pawn(vek)->queued.active());
  CHECK(b.find_pawn(m)->pos == Point{6, 6});
  CHECK(r.seed == 7u);  // no pawn planned: the seed is untouched
}

TEST_CASE("AI phase: the draw hook picks among tied plans") {
  NEED_ENGINE();
  auto board = [] {
    Board b;
    b.turn = 1;
    set_building(b, Point{1, 4});
    set_building(b, Point{6, 1});
    place(b, "Scorpion1", Point{4, 3});
    return b;
  };
  std::vector<Point> ends;
  for (int pick : {0, 1000}) {
    Board b = board();
    AiPhaseOptions o;
    o.draw = [pick](AiDraw kind, int options) { return kind == AiDraw::Deviate ? 0 : std::min(pick, options - 1); };
    const AiPhaseResult r = E.ai_phase(b, o);
    REQUIRE(r.pawns.size() == 1);
    CHECK(r.pawns.front().best_ties > 1);
    CHECK_FALSE(r.pawns.front().deviated);
    ends.push_back(r.pawns.front().plan.loc);
  }
  CHECK(ends[0] != ends[1]);
}

TEST_CASE("AI phase: the recorded seed reproduces the game (live run, Mission_Volatile turn 1)") {
  NEED_ENGINE();
  // The board the player left at the end of turn 1, the enemy phase, then
  // the AI with the save's aiSeed and targetHistory: every Vek of the next
  // turn's board (live 20261012_231559 m00 turn 2) on its tile, aiming at its
  // target.
  const Recording rec = fixture("live_volatile_t1_end_turn.json");
  Board b = rec.board;
  const TurnContext ctx = turn_context(rec, Visibility::Full);
  E.end_turn(b, ctx);
  AiPhaseOptions o;
  o.seed = 323946513;
  o.last_target = {{5278, {3, 3}}, {5279, {3, 1}}, {5280, {3, 3}}, {5281, {3, 3}}, {5282, {2, 2}}};
  o.actions = action_options(ctx);
  const AiPhaseResult r = E.ai_phase(b, o);
  struct Want {
    const char* type;
    Point pos, target;
  };
  const Want want[] = {{"GlowingScorpion", {5, 3}, {5, 2}},
                       {"Leaper2", {2, 1}, {2, 0}},
                       {"Scarab2", {6, 4}, {2, 4}},
                       {"Mosquito1", {4, 4}, {3, 4}}};
  for (const Want& w : want) {
    const Pawn* p = nullptr;
    for (const Pawn& q : b.pawns()) {
      if (q.alive() && symbol_name(q.type) == w.type) p = &q;
    }
    REQUIRE_MESSAGE(p != nullptr, w.type);
    CHECK_MESSAGE(p->pos == w.pos, w.type);
    CHECK_MESSAGE(p->queued.target == w.target, w.type);
  }
  // The whole phase drew exactly what the game did: the seed it leaves is
  // the one the game saved at the start of turn 2.
  CHECK(r.seed == 4479278u);
}

TEST_CASE("AI phase: a Mosquito's planning smoke puts out the burning mech it targets (live Mission_Power)") {
  NEED_ENGINE();
  // Live 20261014_045942 m78 turn 2 -> 3 (Detritus, Unfair): the TeleMech
  // burns at G4 (bridge (4,1)). The enemy phase's fire tick and a Centipede's
  // acid leave it at 1 HP, burning, with ACID: that is where end_turn stops.
  // Then the AI plans: the Mosquito2 walks onto F4 (picking up the acid pool
  // the Centipede left) and queues its whip on the mech, and the instant part
  // of that skill smokes G4. A burning pawn on a smoked tile is put out (fire
  // and smoke never share a tile), so the next player turn shows the mech at
  // 1 HP with ACID and no fire under smoke.
  const Recording rec = fixture("live_power_t2_end_turn.json");
  Board b = rec.board;
  const TurnContext ctx = turn_context(rec, Visibility::Full);
  constexpr int32_t kMech = 2, kMosquito = 15879;
  constexpr Point kMechTile{4, 1};
  REQUIRE(b.find_pawn(kMech) != nullptr);
  REQUIRE(b.find_pawn(kMech)->fire);
  const PhaseResult phase = E.end_turn(b, ctx);
  REQUIRE_FALSE(phase.mission_ended);
  {
    const Pawn& m = *b.find_pawn(kMech);
    CHECK(m.pos == kMechTile);
    CHECK(m.hp == 1);
    CHECK(m.acid);
    CHECK(m.fire);  // the enemy phase itself does not smoke it
    CHECK_FALSE(b.tile(kMechTile).smoke);
  }
  AiPhaseOptions o;
  o.seed = 1821990806;  // the save's aiSeed at the start of turn 2
  o.last_target = {{15874, {1, 5}}, {15875, {5, 2}}, {15876, {3, 1}}, {15877, {1, 6}},
                   {15878, {6, 3}}, {15879, {3, 4}}, {15891, {3, 4}}, {15905, {1, 5}}};
  o.actions = action_options(ctx);
  const AiPhaseResult r = E.ai_phase(b, o);
  CHECK(r.lua_errors.empty());
  const Pawn* mosquito = b.find_pawn(kMosquito);
  REQUIRE(mosquito != nullptr);
  CHECK(mosquito->pos == Point{4, 2});
  CHECK(mosquito->queued.target == kMechTile);
  CHECK(mosquito->acid);  // the Centipede's pool at F4
  const Pawn& m = *b.find_pawn(kMech);
  CHECK(b.tile(kMechTile).smoke);
  CHECK_FALSE(b.tile(kMechTile).on_fire());
  CHECK_FALSE(m.fire);
  CHECK(m.acid);
  CHECK(m.hp == 1);
}

TEST_CASE("deployment: outcome boards and a small search on a recorded drop zone") {
  NEED_ENGINE();
  const Recording rec = fixture("live_volatile_t0_deployed.json");
  const TurnContext ctx = turn_context(rec, Visibility::Player);
  const std::vector<int32_t> mechs = deploy_mechs(rec.board);
  REQUIRE(mechs == std::vector<int32_t>{0, 1, 2});
  // The drop zone of that deployment (bridge drop_zone).
  std::vector<Point> zone;
  for (int x = 1; x <= 3; ++x) {
    for (int y : {1, 2, 5, 6}) zone.push_back(Point{x, y});
  }
  zone.push_back(Point{1, 3});
  zone.push_back(Point{1, 4});
  const std::vector<Point> free = deploy_tiles(rec.board, zone, mechs);
  CHECK(free.size() == zone.size());
  const std::vector<Point> actual{{2, 2}, {2, 5}, {3, 1}};
  const std::vector<Point> other{{3, 2}, {3, 6}, {2, 1}};
  Score cost;
  const Board t1 = deploy_outcome(E, rec.board, ctx, mechs, other, deploy_sample_seed(1, 0), &cost);
  CHECK(t1.turn == rec.board.turn + 1);
  CHECK(t1.player_phase);
  for (size_t i = 0; i < mechs.size(); ++i) {
    const Pawn* p = t1.find_pawn(mechs[i]);
    REQUIRE(p != nullptr);
    CHECK(p->pos == other[i]);
    CHECK(p->active);
  }
  int queued = 0;
  for (const Pawn& p : t1.pawns()) queued += p.team == Team::Enemy && p.queued.active();
  CHECK(queued >= 3);
  CHECK(cost[ScoreKey::GridLost] == 0);
  // Deterministic for a seed.
  CHECK(deploy_outcome(E, rec.board, ctx, mechs, other, deploy_sample_seed(1, 0)) == t1);

  DeployOptions o;
  o.time_limit_s = 0;
  o.samples = 2;
  o.solve_time_s = 0;
  o.solve_nodes = 20;
  o.placements = {actual, other};
  const DeployResult r = deploy_search(E, rec.board, ctx, zone, o);
  REQUIRE_MESSAGE(r.ok, r.error);
  CHECK(r.mechs == mechs);
  REQUIRE(r.finalists.size() == 2);
  REQUIRE(r.best.has_value());
  CHECK((r.best->tiles == actual || r.best->tiles == other));
  for (const DeployCandidate& c : r.finalists) {
    REQUIRE(c.value.has_value());
    CHECK(c.value->seeds == 2);
    CHECK(c.value->outcomes >= 1);
    CHECK(c.value->outcomes <= 2);
    CHECK(c.screen.seeds >= 1);
  }
  CHECK_FALSE(deploy_better(r.finalists[1].value.value(), r.finalists[0].value.value()));
  CHECK(r.solves >= 2);

  // The turn-1 solves can rank with the live options (threat tier, mech
  // exchange, next-turn lookahead): the values then carry those tiers; the
  // phase's own cost adds no threat (that is turn 1's end board's).
  DeployOptions ranked = o;
  ranked.solve.threat_tier = true;
  ranked.solve.mech_grid = 2;
  ranked.solve.mech_grid_turns = true;
  ranked.solve.lookahead = true;
  ranked.solve.lookahead_options.samples = 2;
  const DeployResult rr = deploy_search(E, rec.board, ctx, zone, ranked);
  REQUIRE_MESSAGE(rr.ok, rr.error);
  REQUIRE(rr.finalists.size() == 2);
  for (const DeployCandidate& c : rr.finalists) {
    REQUIRE(c.value.has_value());
    CHECK(c.value->worst[ScoreKey::NextTurn] <= 0);
    CHECK(c.value->worst[ScoreKey::NextTurn] >= kNextTurnFloor);
    CHECK(c.value->worst[ScoreKey::Threat] <= 0);
  }
  for (const DeployCandidate& c : r.finalists) CHECK(c.value->worst[ScoreKey::NextTurn] == 0);

  // A placement with the wrong tile count is refused.
  DeployOptions bad = o;
  bad.placements = {{{2, 2}}};
  const DeployResult rb = deploy_search(E, rec.board, ctx, zone, bad);
  CHECK_FALSE(rb.ok);
}

// Mission_Final (live run 24, m40): the seven pylon buildings drop at the
// start of the enemy turn that follows the deployment (Mission_Final:
// NextTurn at turn count 0), before the AI plans. The deployed board has
// none of them; the turn-1 boards must, so the AI can aim at them and the
// search values the placement by them.
TEST_CASE("deployment: Mission_Final's pylons drop before the AI plans") {
  NEED_ENGINE();
  const Recording rec = fixture("live_final_t0_deployed.json");
  REQUIRE(rec.mission.mission_id == "Mission_Final");
  const TurnContext ctx = turn_context(rec, Visibility::Player);
  const auto zone_it = rec.mission.zones.find("pylons");
  REQUIRE(zone_it != rec.mission.zones.end());
  const std::vector<Point> pylons = zone_it->second;
  REQUIRE(pylons.size() == 7);
  for (Point p : pylons) CHECK_FALSE(rec.board.tile(p).is_building());

  // The enemy phase alone: the hook raises populated 2-HP buildings once.
  Board phase = rec.board;
  const PhaseResult pr = E.end_turn(phase, ctx);
  CHECK(pr.count(PhaseEventType::MissionHook) == 1);
  CHECK(pr.exact);
  for (Point p : pylons) {
    const Tile& t = phase.tile(p);
    CHECK(t.is_building());
    CHECK(t.hp == 2);
    CHECK(t.max_hp == 2);
    CHECK(t.populated);
  }

  const std::vector<int32_t> mechs = deploy_mechs(rec.board);
  const std::vector<Point> live{{3, 6}, {2, 2}, {3, 4}};  // the live placement
  int aimed = 0;
  for (int s = 0; s < 4; ++s) {
    Score cost;
    const Board t1 = deploy_outcome(E, rec.board, ctx, mechs, live, deploy_sample_seed(1, s), &cost);
    for (Point p : pylons) CHECK(t1.tile(p).hp == 2);
    // Their arrival is no gain for the phase; nothing is lost yet.
    CHECK(cost[ScoreKey::BuildingHpLost] == 0);
    CHECK(cost[ScoreKey::GridLost] == 0);
    for (const Pawn& p : t1.pawns()) {
      if (p.team == Team::Enemy && p.queued.active() &&
          std::find(pylons.begin(), pylons.end(), p.queued.target) != pylons.end()) {
        ++aimed;
      }
    }
    // Turn 1 does not drop them again.
    Board end = t1;
    const PhaseResult pr1 = E.end_turn(end, ctx);
    CHECK(pr1.count(PhaseEventType::MissionHook) == 0);
  }
  MESSAGE("queued shots at pylons over 4 seeds: " << aimed);
  CHECK(aimed > 0);
}
