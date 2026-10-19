// Next-turn lookahead (lookahead.hpp, SolveOptions::lookahead) and the
// pilot floor of the mech exchange (ScoreOptions::mech_grid_pilot).

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "itb/engine.hpp"
#include "itb/game_data.hpp"
#include "itb/lookahead.hpp"
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

void building(Board& b, Point p, int hp = 1) {
  Tile& t = b.tile(p);
  t.terrain = Terrain::Building;
  t.hp = t.max_hp = static_cast<int8_t>(hp);
  t.populated = true;
}

int32_t add(Board& b, const char* type, Point p, bool mech = false) {
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
  b.next_uid = uid + 1;
  return uid;
}

// The lookahead of ending the turn on `b` with no action.
NextTurnLoss look(Engine& E, const Board& b, LookaheadOptions o = {}) {
  TurnContext ctx;
  Board after = b;
  const PhaseResult pr = E.end_turn(after, ctx);
  return lookahead_loss(E, b, after, &pr, ctx, o);
}

}  // namespace

TEST_CASE("lookahead: a Vek next to a building, answered or not") {
  NEED_ENGINE();
  // A Scorpion beside a building plans to hit it next turn. A Punch Mech
  // across the board cannot reach it next turn; one beside it can.
  Board b;
  b.turn = 1;
  b.total_turns = 4;
  building(b, {3, 0});
  add(b, "Scorpion1", {3, 1});
  Board far = b;
  add(far, "PunchMech", {7, 7}, true);
  Board near = b;
  add(near, "PunchMech", {5, 1}, true);

  LookaheadOptions o;
  o.samples = 2;
  const NextTurnLoss lf = look(E, far, o);
  CHECK(lf.grid >= 1);
  CHECK(lf.building_hp >= 1);
  CHECK(next_turn_value(lf) < 0);
  const NextTurnLoss ln = look(E, near, o);
  CHECK(ln.grid == 0);
  CHECK(ln.building_hp == 0);
  // Idle: nobody answers.
  o.reply = LookaheadOptions::Reply::Idle;
  CHECK(look(E, near, o).grid >= 1);
  // No next turn on the mission's last turn.
  Board last = far;
  last.turn = 4;
  const NextTurnLoss ll = look(E, last, LookaheadOptions{});
  CHECK(ll.mission_over);
  CHECK(next_turn_value(ll) == 0);
}

TEST_CASE("lookahead: packing and floor") {
  NextTurnLoss l;
  CHECK(next_turn_value(l) == 0);
  l.building_hp = 2;
  const int32_t b2 = next_turn_value(l);
  l = {};
  l.mechs = 1;
  const int32_t m1 = next_turn_value(l);
  l = {};
  l.grid = 1;
  const int32_t g1 = next_turn_value(l);
  CHECK(g1 < m1);  // grid first, then mechs, then building HP
  CHECK(m1 < b2);
  l.grid = 99;
  l.mechs = 99;
  l.building_hp = 999;
  CHECK(next_turn_value(l) == kNextTurnFloor);
}

TEST_CASE("lookahead: folds (worst, sum, worst then sum)") {
  using Fold = LookaheadOptions::Fold;
  auto fold = [](const std::vector<int32_t>& v, Fold f) {
    int32_t acc = 0;
    for (int32_t x : v) acc = lookahead_fold(acc, x, f);
    return acc;
  };
  NextTurnLoss g1;
  g1.grid = 1;
  NextTurnLoss b1;
  b1.building_hp = 1;
  const int32_t G = next_turn_value(g1), B = next_turn_value(b1);
  for (Fold f : {Fold::Worst, Fold::Sum, Fold::WorstSum}) {
    CHECK(fold({}, f) == 0);  // the cap
    CHECK(lookahead_fold_of(lookahead_fold_name(f)) == f);
  }
  CHECK(!lookahead_fold_of("mean"));
  for (auto r : {LookaheadOptions::Reply::Idle, LookaheadOptions::Reply::Greedy, LookaheadOptions::Reply::Search}) {
    CHECK(lookahead_reply_of(lookahead_reply_name(r)) == r);
  }
  // Worst then sum: the worst sample first, ties broken by the sum.
  CHECK(fold({G, 0}, Fold::WorstSum) == fold({0, G}, Fold::WorstSum));  // order does not matter
  CHECK(fold({G, G}, Fold::WorstSum) < fold({G, 0}, Fold::WorstSum));   // same worst, larger sum
  CHECK(fold({G, B}, Fold::WorstSum) < fold({G, 0}, Fold::WorstSum));
  CHECK(fold({G, 0}, Fold::WorstSum) < fold({B, B}, Fold::WorstSum));   // a worse worst beats any sum
  CHECK(lookahead_fold_worst(fold({G, B}, Fold::WorstSum), Fold::WorstSum) == G);
  CHECK(lookahead_fold_worst(fold({G, B}, Fold::Worst), Fold::Worst) == G);
  // Monotone (each sample can only lower the value) and above the floor,
  // the extremes included (no overflow).
  std::mt19937 rng(7);
  for (int samples : {1, 2, 8, 16, 32}) {
    LookaheadOptions o;
    o.samples = samples;
    for (Fold f : {Fold::Worst, Fold::Sum, Fold::WorstSum}) {
      o.fold = f;
      const int32_t floor = lookahead_floor(o);
      std::vector<int32_t> all(static_cast<size_t>(samples), kNextTurnFloor);
      CHECK(fold(all, f) == floor);
      for (int trial = 0; trial < 50; ++trial) {
        int32_t acc = 0;
        for (int k = 0; k < samples; ++k) {
          const int32_t v = -static_cast<int32_t>(rng() % static_cast<uint32_t>(-kNextTurnFloor + 1));
          const int32_t next = lookahead_fold(acc, v, f);
          CHECK(next <= acc);
          CHECK(next >= floor);
          acc = next;
        }
      }
    }
  }
  // Up to 8 samples the order is exactly lexicographic (worst, then sum).
  for (int trial = 0; trial < 2000; ++trial) {
    std::vector<int32_t> a, b;
    for (int k = 0; k < 8; ++k) {
      a.push_back(-static_cast<int32_t>(rng() % 4u) * (trial % 2 ? 1 : kNextTurnGrid));
      b.push_back(-static_cast<int32_t>(rng() % 4u) * (trial % 3 ? 1 : kNextTurnGrid));
    }
    const int32_t wa = *std::min_element(a.begin(), a.end()), wb = *std::min_element(b.begin(), b.end());
    int64_t sa = 0, sb = 0;
    for (int k = 0; k < 8; ++k) sa += a[static_cast<size_t>(k)], sb += b[static_cast<size_t>(k)];
    const bool lex = wa != wb ? wa < wb : sa < sb;
    CHECK(lex == (fold(a, Fold::WorstSum) < fold(b, Fold::WorstSum)));
  }
}

TEST_CASE("lookahead: the solver kills the Vek it could not answer next turn; off, nothing changes") {
  NEED_ENGINE();
  // As the threat tier's test: a Punch Mech can kill either Vek. Off, it
  // takes the one with more HP far from any building; with the lookahead it
  // kills the one beside the building, which it could not reach next turn.
  Board b;
  b.turn = 1;
  b.total_turns = 4;
  building(b, {3, 0});
  const int32_t m = add(b, "PunchMech", {4, 4}, true);
  (void)m;
  const int32_t far = add(b, "Scorpion1", {6, 6});
  b.find_pawn(far)->hp = 2;
  b.find_pawn(far)->move = 0;
  const int32_t near = add(b, "Scorpion1", {3, 1});
  b.find_pawn(near)->hp = 1;
  TurnContext ctx;
  SolveOptions o;
  o.time_limit_s = 120;
  const SolveResult off = solve_turn(E, b, ctx, o);
  REQUIRE(off.proven_optimal);
  CHECK(off.best.worst_case[ScoreKey::NextTurn] == 0);
  CHECK(off.proven_components == used_tier_count(0));
  CHECK(off.best.worst_case[ScoreKey::VekHpRemoved] == 2);  // the far one
  CHECK(off.stats.lookahead_evals == 0);

  o.lookahead = true;
  o.lookahead_options.samples = 2;
  const SolveResult on = solve_turn(E, b, ctx, o);
  REQUIRE(on.proven_optimal);
  CHECK(on.proven_components == used_tier_count(kTierNextTurn));
  CHECK(on.best.worst_case[ScoreKey::NextTurn] == 0);
  CHECK(on.best.worst_case[ScoreKey::VekKilled] == 1);
  CHECK(on.best.worst_case[ScoreKey::VekHpRemoved] == 1);  // the near one
  CHECK(on.stats.lookahead_evals > 0);
  for (const std::string& w : on.warnings) CHECK_MESSAGE(w.find("re-running") == std::string::npos, w);
  // The off plan, scored with the lookahead, leaves a loss next turn.
  const std::optional<Score> again = evaluate_plan(E, b, ctx, off.best.actions, o);
  REQUIRE(again);
  CHECK((*again)[ScoreKey::NextTurn] < 0);
  CHECK(*again < on.best.worst_case);
}

TEST_CASE("score: the pilot floor of the mech exchange (mech_grid_pilot)") {
  // Three mechs on the mission's last turn: with mech_grid_turns the rate is
  // 0; with the pilot floor a mech whose pilot has a level keeps a rate of
  // min(mech_grid, level).
  Board before;
  before.grid_power = 5;
  before.turn = 4;
  before.total_turns = 4;
  for (int i = 0; i < 3; ++i) {
    Pawn m;
    m.uid = i;
    m.type = intern("PunchMech");
    m.team = Team::Player;
    m.mech = true;
    m.corpse = true;
    m.pos = {i, 0};
    m.hp = m.max_hp = 3;
    m.pilot_level = static_cast<int8_t>(i);  // levels 0, 1, 2
    before.add_pawn(m);
  }
  ScoreOptions o;
  o.mech_grid = 2;
  o.mech_grid_turns = true;
  CHECK(mech_grid_rate(before, o) == 0);
  for (int i = 0; i < 3; ++i) CHECK(mech_rate(before, *before.find_pawn(i), o) == 0);
  o.mech_grid_pilot = true;
  CHECK(mech_rate(before, *before.find_pawn(0), o) == 0);
  CHECK(mech_rate(before, *before.find_pawn(1), o) == 1);
  CHECK(mech_rate(before, *before.find_pawn(2), o) == 2);
  o.mech_grid = 1;
  CHECK(mech_rate(before, *before.find_pawn(2), o) == 1);  // capped by mech_grid
  o.mech_grid = 2;

  auto lose = [&](int uid) {
    Board b = before;
    b.find_pawn(uid)->hp = 0;
    return score_turn(before, b, nullptr, nullptr, o);
  };
  Board grid1 = before;
  grid1.grid_power = 4;
  Board grid2 = before;
  grid2.grid_power = 3;
  const Score g1 = score_turn(before, grid1, nullptr, nullptr, o);
  const Score g2 = score_turn(before, grid2, nullptr, nullptr, o);
  // Level 0: worth less than one grid point (as before).
  CHECK(lose(0)[ScoreKey::GridLost] == -1);
  CHECK(lose(0) > g1);
  // Level 1: more than one grid point, less than two.
  CHECK(lose(1)[ScoreKey::GridLost] == -3);
  CHECK(lose(1) < g1);
  CHECK(lose(1) > g2);
  // Level 2: more than two.
  CHECK(lose(2)[ScoreKey::GridLost] == -5);
  CHECK(lose(2) < g2);
  // Equal rates: the same slot as the single-rate formula.
  Board mid = before;
  mid.turn = 1;
  Board d = mid;
  d.find_pawn(0)->hp = 0;
  ScoreOptions plain = o;
  plain.mech_grid_pilot = false;
  CHECK(score_turn(mid, d, nullptr, nullptr, o) == score_turn(mid, d, nullptr, nullptr, plain));
  // A revived mech counts at its own rate (+): sound for the bound's sum.
  Board dead = before;
  dead.find_pawn(2)->hp = 0;
  Board revived = dead;
  revived.find_pawn(2)->hp = 1;
  CHECK(score_turn(dead, revived, nullptr, nullptr, o)[ScoreKey::GridLost] == 5);
}
