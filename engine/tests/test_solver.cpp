// Stage 9: the perfect-turn search (solver.hpp) and board hashes
// (board_hash.hpp). Hand-built boards whose best plan is known, chance nodes
// whose worst case is not the default outcome, and a brute-force cross-check:
// on tiny boards a naive exhaustive enumeration (no table, no bounds, no
// ordering, written independently of the solver) must give the same value.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "itb/board_hash.hpp"
#include "itb/engine.hpp"
#include "itb/game_data.hpp"
#include "itb/lookahead.hpp"
#include "itb/lua_host.hpp"
#include "itb/movement.hpp"
#include "itb/recording.hpp"
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
  return uid;
}

Pawn& P(Board& b, int32_t uid) {
  Pawn* p = b.find_pawn(uid);
  REQUIRE(p != nullptr);
  return *p;
}

void queue(Board& b, int32_t uid, Point target) {
  Pawn& p = P(b, uid);
  p.queued = QueuedShot{0, p.pos, target};
}

TurnContext context(const char* mission = "") {
  TurnContext ctx;
  ctx.mission.mission_id = mission;
  return ctx;
}

SolveOptions quick(double seconds = 60) {
  SolveOptions o;
  o.time_limit_s = seconds;
  return o;
}

// ---- The naive reference: every sequence of sub-actions, every chance outcome --
//
// Written without the solver's machinery: plain recursion, chance outcomes by
// re-running each prefix of choices, values by the definition
//   V(B) = max(E(B), max_a min_o V(B·a·o)),  E(B) = min_o score(end_turn(B, o)).

// How the grid survival odds weigh a call (SolveOptions::grid_survival):
// a Grid Defense roll by the resist chance, a mission's random pick and a
// spider egg evenly, anything else at its worst.
enum class Kind { Worst, Resist, Even };

struct Choices {
  std::vector<int> forced;
  std::vector<int> options;  // per call
  std::vector<Kind> kinds;   // per call
  int next(int n, Kind k = Kind::Worst) {
    const size_t i = options.size();
    options.push_back(n);
    kinds.push_back(k);
    const int c = i < forced.size() ? forced[i] : 0;
    return std::min(c, n - 1);
  }
};

// Runs `run` once per chance outcome: the run with `prefix` forced (default
// choices after it), then every other option of each later call.
template <class Run>
void each_outcome(Run&& run, const std::vector<int>& prefix = {}) {
  Choices c;
  c.forced = prefix;
  run(c);
  for (size_t i = prefix.size(); i < c.options.size(); ++i) {
    for (int j = 1; j < c.options[i]; ++j) {
      std::vector<int> p = prefix;
      p.resize(i, 0);
      p.push_back(j);
      each_outcome(run, p);
    }
  }
}

struct Naive {
  Engine& e;
  const Board& root;
  const TurnContext& base;
  ScoreOptions score_options{};
  // With the lookahead: the NextTurn tier of every chance outcome (the
  // definition: lookahead_loss, worst over its samples).
  const LookaheadOptions* lookahead = nullptr;
  // Grid survival (score_options.grid_survival): the Grid Defense chance.
  double resist_p = 0;
  uint64_t evaluations = 0;
  int player_chance = 0;  // grid survival: actions with more than one run folded

  void hooks(Choices& c, ActionOptions& o, TurnContext* tc) {
    auto resist = [&c](Point, int) { return c.next(2, Kind::Resist) == 1; };
    auto egg = [&c](Resolver&, const Pawn&, const std::vector<Point>& t) {
      return c.next(static_cast<int>(t.size()), Kind::Even);
    };
    auto choose = [&c](const ChanceRecord& n) {
      return c.next(std::max(1, n.options), n.kind == ChanceKind::MissionRandom ? Kind::Even : Kind::Worst);
    };
    o.grid_resist = resist;
    o.spider_egg = egg;
    o.mission = &base.mission;  // the mission's per-frame hooks, as in game
    o.choose = choose;
    if (tc) {
      tc->grid_resist = resist;
      tc->spider_egg = egg;
      tc->choose = choose;
    }
  }

  // The odds that the grid outlives the enemy phase from b, by walking the
  // chance tree node by node: the run with `prefix` forced reveals the next
  // call; each of its options is a subtree. `lost`: some leaf loses the grid.
  double odds(const Board& b, const std::vector<int>& prefix, bool& lost) {
    Choices c;
    c.forced = prefix;
    Board after = b;
    TurnContext tc = base;
    ActionOptions unused;
    hooks(c, unused, &tc);
    e.lua().seed(1);
    e.end_turn(after, tc);
    const size_t d = prefix.size();
    if (c.options.size() <= d) {
      if (after.grid_power <= 0) lost = true;
      return after.grid_power > 0 ? 1.0 : 0.0;
    }
    const int n = c.options[d];
    double first = 0, second = 0, sum = 0, worst = 2.0;
    for (int j = 0; j < n; ++j) {
      std::vector<int> p = prefix;
      p.push_back(j);
      const double x = odds(b, p, lost);
      if (j == 0) first = x;
      if (j == 1) second = x;
      sum += x;
      worst = std::min(worst, x);
    }
    if (n == 1) return first;
    if (c.kinds[d] == Kind::Resist && n == 2) return (1 - resist_p) * first + resist_p * second;
    if (c.kinds[d] == Kind::Even) return sum / n;
    return worst;
  }

  // The odds rounded to kSurvivalOne units: exactly 1 only if no leaf loses
  // the grid.
  static int32_t rounded(double o, bool lost) {
    return lost ? static_cast<int32_t>(std::min<long long>(std::llround(std::clamp(o, 0.0, 1.0) * kSurvivalOne),
                                                           kSurvivalOne - 1))
                : kSurvivalOne;
  }

  Score end_value(const Board& b) {
    Score worst, alive;
    worst.v.fill(INT32_MAX);
    alive.v.fill(INT32_MAX);
    each_outcome([&](Choices& c) {
      Board after = b;
      TurnContext tc = base;
      ActionOptions unused;
      hooks(c, unused, &tc);
      e.lua().seed(1);
      const PhaseResult pr = e.end_turn(after, tc);
      ++evaluations;
      Score s = score_turn(root, after, &tc, &pr, score_options);
      if (lookahead) s[ScoreKey::NextTurn] = lookahead_value(e, root, after, &pr, tc, *lookahead);
      worst = std::min(worst, s);
      if (s[ScoreKey::GridSurvival] == kSurvivalOne) alive = std::min(alive, s);
    });
    if (score_options.grid_survival) {
      // The definition: the odds over the whole tree; the rest from the
      // worst outcome that kept the grid when the odds are above 0, else
      // from the worst outcome.
      bool lost = false;
      const int32_t o = rounded(odds(b, {}, lost), lost);
      worst = o > 0 ? alive : worst;
      worst[ScoreKey::GridSurvival] = o;
    }
    return worst;
  }

  // A player action's chance tree, folded (grid survival): `leaves` are the
  // runs (their calls' options, kinds and choices) with the value reached.
  struct Run {
    std::vector<int> options, chosen;
    std::vector<Kind> kinds;
    Score v;
  };
  double fold(const std::vector<const Run*>& runs, size_t d) {
    if (runs.size() == 1 && runs[0]->options.size() <= d) {
      return static_cast<double>(runs[0]->v[ScoreKey::GridSurvival]) / kSurvivalOne;
    }
    std::map<int, std::vector<const Run*>> by;
    for (const Run* r : runs) {
      REQUIRE(r->options.size() > d);
      by[r->chosen[d]].push_back(r);
    }
    const int n = runs[0]->options[d];
    const Kind kind = runs[0]->kinds[d];
    double first = 0, second = 0, sum = 0, worst = 2.0;
    for (int j = 0; j < n; ++j) {
      REQUIRE(by.count(j) == 1);
      const double x = fold(by[j], d + 1);
      if (j == 0) first = x;
      if (j == 1) second = x;
      sum += x;
      worst = std::min(worst, x);
    }
    if (n == 1) return first;
    if (kind == Kind::Resist && n == 2) return (1 - resist_p) * first + resist_p * second;
    if (kind == Kind::Even) return sum / n;
    return worst;
  }

  // Every sub-action from b: (description, runner).
  std::vector<std::function<bool(Board&, ActionOptions&)>> actions(const Board& b) {
    std::vector<std::function<bool(Board&, ActionOptions&)>> out;
    LuaHost& lua = e.lua();
    for (const Pawn& p : b.pawns()) {
      if (!p.controlled() || !p.alive()) continue;
      const int32_t uid = p.uid;
      if (can_move(p)) {
        for (Point d : lua.target_area(b, p, "Move", p.pos)) {
          if (!d.valid() || d == p.pos) continue;
          out.push_back([this, uid, d](Board& x, ActionOptions& o) { return e.move(x, uid, d, o).ok(); });
        }
      }
      if (!(p.active && p.movement.powered && !p.frozen)) continue;
      for (int slot = 0; slot < kMaxWeapons; ++slot) {
        const Symbol w = p.weapons[static_cast<size_t>(slot)];
        if (w == kNoSymbol) continue;
        const std::string name(symbol_name(w));
        const auto passive = lua.lua_string(name, "Passive");
        if (passive && !passive->empty()) continue;
        for (Point t : lua.target_area(b, p, name, p.pos)) {
          if (!t.valid()) continue;
          if (lua.is_two_click(name) && !lua.two_click_exception(b, p, name, p.pos, t)) {
            const Point first = lua.translate_first_click(b, p, name, p.pos, t);
            for (Point t2 : lua.second_target_area(b, p, name, p.pos, first)) {
              if (!t2.valid()) continue;
              out.push_back([this, uid, slot, t, t2](Board& x, ActionOptions& o) {
                return e.fire_weapon(x, uid, slot, t, t2, o).ok();
              });
            }
          } else {
            out.push_back([this, uid, slot, t](Board& x, ActionOptions& o) {
              return e.fire_weapon(x, uid, slot, t, std::nullopt, o).ok();
            });
          }
        }
      }
      if (p.mech) {
        out.push_back([this, uid](Board& x, ActionOptions& o) {
          return e.repair(x, uid, kInvalidPoint, "Skill_Repair", o).ok();
        });
      }
    }
    return out;
  }

  Score value(const Board& b, int depth = 0) {
    REQUIRE(depth < 16);
    Score best = end_value(b);
    for (auto& act : actions(b)) {
      Score worst;
      worst.v.fill(INT32_MAX);
      bool legal = true;
      std::vector<Run> runs;
      each_outcome([&](Choices& c) {
        if (!legal) return;
        Board next = b;
        ActionOptions o;
        hooks(c, o, nullptr);
        e.lua().seed(1);
        if (!act(next, o)) {
          legal = false;
          return;
        }
        const Score v = value(next, depth + 1);
        worst = std::min(worst, v);
        std::vector<int> chosen;
        for (size_t i = 0; i < c.options.size(); ++i) {
          chosen.push_back(i < c.forced.size() ? std::min(c.forced[i], c.options[i] - 1) : 0);
        }
        runs.push_back(Run{c.options, chosen, c.kinds, v});
      });
      if (!legal) continue;
      if (score_options.grid_survival && runs.size() > 1) {
        ++player_chance;
        // The odds folded over the action's chance tree; the rest from the
        // worst outcome with odds above 0 when the fold is above 0.
        std::vector<const Run*> ptrs;
        bool lost = false;
        for (const Run& r : runs) {
          ptrs.push_back(&r);
          lost = lost || r.v[ScoreKey::GridSurvival] < kSurvivalOne;
        }
        const int32_t o = rounded(fold(ptrs, 0), lost);
        Score cls;
        cls.v.fill(INT32_MAX);
        for (const Run& r : runs) {
          if (o <= 0 || r.v[ScoreKey::GridSurvival] > 0) cls = std::min(cls, r.v);
        }
        cls[ScoreKey::GridSurvival] = o;
        worst = cls;
      }
      best = std::max(best, worst);
    }
    return best;
  }
};

}  // namespace

// ---- Board hashes -----------------------------------------------------------------

TEST_CASE("board hash: equal boards, different histories, every field counts") {
  NEED_ENGINE();
  Board b;
  const int32_t a = place(b, "PunchMech", {2, 2}, true);
  const int32_t c = place(b, "PunchMech", {5, 5}, true);
  P(b, a).move = 3;
  P(b, c).move = 3;

  // Two orders of the same two moves: different arrival stamps and undo
  // flags, the same board for the search.
  Board x = b, y = b;
  REQUIRE(E.move(x, a, {2, 4}).ok());
  REQUIRE(E.move(x, c, {5, 3}).ok());
  REQUIRE(E.move(y, c, {5, 3}).ok());
  REQUIRE(E.move(y, a, {2, 4}).ok());
  CHECK_FALSE(x == y);  // arrival stamps differ
  CHECK(hash_board(x, HashMode::Search) == hash_board(y, HashMode::Search));
  CHECK(hash_board(x, HashMode::Exact) == hash_board(y, HashMode::Exact));

  // Undo flags only matter in Exact mode.
  Board u = b;
  P(u, a).movement.undo_ready = true;
  CHECK_FALSE(hash_board(u, HashMode::Exact) == hash_board(b, HashMode::Exact));
  CHECK(hash_board(u, HashMode::Search) == hash_board(b, HashMode::Search));

  // A player unit's active flag: the search sees it, End Turn does not.
  Board d = b;
  P(d, a).active = false;
  CHECK_FALSE(hash_board(d, HashMode::Search) == hash_board(b, HashMode::Search));
  CHECK(hash_board(d, HashMode::EndTurn) == hash_board(b, HashMode::EndTurn));

  // Single fields change the hash.
  Board t = b;
  t.tile({0, 0}).smoke = true;
  CHECK_FALSE(hash_board(t) == hash_board(b));
  Board h = b;
  P(h, c).hp = 1;
  CHECK_FALSE(hash_board(h) == hash_board(b));
  Board g = b;
  g.grid_power = 3;
  CHECK_FALSE(hash_board(g) == hash_board(b));
  Board s = b;
  s.spawn_points.push_back({1, 1});
  CHECK_FALSE(hash_board(s) == hash_board(b));
  // Pawn list order counts (it drives attack order).
  Board o1;
  place(o1, "Hornet1", {1, 1});
  place(o1, "Hornet1", {2, 1});
  Board o2 = o1;
  std::swap(o2.pawns()[0], o2.pawns()[1]);
  CHECK_FALSE(hash_board(o1) == hash_board(o2));
}

// ---- Hand-built boards ------------------------------------------------------------

TEST_CASE("solver: one Vek threatens a building; the punch saves it") {
  NEED_ENGINE();
  // Punching from D5 would push the dying Hornet into the building; only the
  // side tiles save it.
  Board b;
  const int32_t m = place(b, "PunchMech", {2, 6}, true);
  const int32_t h = place(b, "Hornet1", {3, 3});
  queue(b, h, {3, 2});
  set_building(b, {3, 2});
  const TurnContext ctx = context();

  // Doing nothing loses the building.
  const auto idle = evaluate_plan(E, b, ctx, {});
  REQUIRE(idle);
  CHECK((*idle)[ScoreKey::GridLost] == -1);

  const SolveResult r = solve_turn(E, b, ctx, quick());
  CHECK(r.proven_optimal);
  CHECK(r.best.worst_case[ScoreKey::GridLost] == 0);
  CHECK(r.best.worst_case[ScoreKey::BuildingHpLost] == 0);
  CHECK(r.best.worst_case[ScoreKey::VekKilled] == 1);  // the punch (2) kills the Hornet (2)
  REQUIRE(r.best.actions.size() == 1);
  CHECK(r.best.actions[0].uid == m);
  CHECK(r.best.actions[0].kind == PlayerAction::Kind::Weapon);
  CHECK(r.best.actions[0].target == Point{3, 3});
  CHECK(r.best.actions[0].move == Point{2, 3});
  CHECK(r.upper_bound == r.best.worst_case);
  CHECK(r.proven_components == used_tier_count(false));
  // The plan's value, re-run open-loop.
  const auto again = evaluate_plan(E, b, ctx, r.best.actions);
  REQUIRE(again);
  CHECK(*again == r.best.worst_case);
  CHECK(r.warnings.empty());
}

TEST_CASE("enemy phase: stop_early ends the phase between steps, never otherwise") {
  NEED_ENGINE();
  // Two Hornets, each queued on its own building.
  Board b;
  place(b, "PunchMech", {6, 6}, true);
  const int32_t h1 = place(b, "Hornet1", {3, 3});
  const int32_t h2 = place(b, "Hornet1", {5, 3});
  queue(b, h1, {3, 2});
  queue(b, h2, {5, 2});
  set_building(b, {3, 2});
  set_building(b, {5, 2});

  TurnContext plain = context();
  Board full = b;
  const PhaseResult pr = E.end_turn(full, plain);
  CHECK_FALSE(pr.stopped);
  CHECK(full.tile({3, 2}).hp == 0);
  CHECK(full.tile({5, 2}).hp == 0);

  // A hook that never stops changes nothing.
  TurnContext never = context();
  int asked = 0;
  never.stop_early = [&](const Board&) {
    ++asked;
    return false;
  };
  Board same = b;
  const PhaseResult pn = E.end_turn(same, never);
  CHECK_FALSE(pn.stopped);
  CHECK(asked >= 2);  // after the status ticks and after each shot
  CHECK(hash_board(same) == hash_board(full));
  CHECK(pn.events.size() == pr.events.size());

  // Stop once a building is down: the second shot never fires.
  TurnContext stop = context();
  stop.stop_early = [](const Board& mid) { return mid.tile({3, 2}).hp == 0 || mid.tile({5, 2}).hp == 0; };
  Board part = b;
  const PhaseResult ps = E.end_turn(part, stop);
  CHECK(ps.stopped);
  CHECK((part.tile({3, 2}).hp == 0) != (part.tile({5, 2}).hp == 0));
}

// Live run 2026-10-12: the Shifty re-activation was taken as a fresh action
// and the plan fired and repaired one mech dozens of times. In the UI the
// mech may only step one tile after attacking.
TEST_CASE("solver: a Shifty mech attacks once, then may only step") {
  NEED_ENGINE();
  // Two Hornets (2 HP) in a row east of the mech, each a punch kill; each
  // threatens a building.
  Board b;
  const int32_t m = place(b, "PunchMech", {2, 5}, true);
  P(b, m).pilot_abilities = kPilotShifty;
  const int32_t h1 = place(b, "Hornet1", {3, 5});
  queue(b, h1, {3, 6});
  set_building(b, {3, 6});
  const int32_t h2 = place(b, "Hornet1", {3, 4});
  queue(b, h2, {3, 3});
  set_building(b, {3, 3});
  const TurnContext ctx = context();
  const SolveResult r = solve_turn(E, b, ctx, quick());
  CHECK(r.proven_optimal);
  int acts = 0;
  for (const PlayerAction& a : r.best.actions) {
    if (a.uid == m && a.kind != PlayerAction::Kind::None) ++acts;
  }
  CHECK(acts == 1);
  CHECK(r.best.worst_case[ScoreKey::VekKilled] <= 1);
  // Punch, step one tile (the Shifty bonus), punch again: the second punch
  // is refused, as in the game.
  PlayerAction punch;
  punch.uid = m;
  punch.kind = PlayerAction::Kind::Weapon;
  punch.weapon = "Prime_Punchmech";
  punch.target = {3, 5};
  PlayerAction again = punch;
  again.move = {2, 4};
  again.target = {3, 4};
  CHECK(evaluate_plan(E, b, ctx, {punch}));
  CHECK_FALSE(evaluate_plan(E, b, ctx, {punch, again}));
}

TEST_CASE("solver: the greedy first action loses a building later") {
  NEED_ENGINE();
  // The Hornet next to the mech is a free kill, but punching it uses the
  // mech's only action; the Scorpion two tiles away is about to hit a
  // building and only a move-and-punch from the far side saves it.
  Board b;
  const int32_t m = place(b, "PunchMech", {4, 4}, true);
  const int32_t easy = place(b, "Hornet1", {4, 3});
  queue(b, easy, {4, 2});  // the Hornet attacks an empty tile
  const int32_t s = place(b, "Scorpion1", {2, 5});
  queue(b, s, {1, 5});
  set_building(b, {1, 5});
  const TurnContext ctx = context();
  const SolveResult r = solve_turn(E, b, ctx, quick());
  CHECK(r.proven_optimal);
  CHECK(r.best.worst_case[ScoreKey::GridLost] == 0);
  REQUIRE(r.best.actions.size() == 1);
  CHECK(r.best.actions[0].uid == m);
  CHECK(r.best.actions[0].target == Point{2, 5});
  // Killing the Hornet instead is worse in the first tier.
  PlayerAction greedy;
  greedy.uid = m;
  greedy.kind = PlayerAction::Kind::Weapon;
  greedy.weapon = "Prime_Punchmech";
  greedy.target = {4, 3};
  const auto g = evaluate_plan(E, b, ctx, {greedy});
  REQUIRE(g);
  CHECK((*g)[ScoreKey::GridLost] == -1);
  CHECK(*g < r.best.worst_case);
}

TEST_CASE("solver: two threats, two mechs; the order of actions matters") {
  NEED_ENGINE();
  // Mech A stands where B must stand to punch the Hornet towards nothing;
  // A must leave first, B punches, then A deals with the second Vek. Whatever
  // the exact plan, the search must save both buildings and match the
  // brute-force value.
  Board b;
  const int32_t a = place(b, "PunchMech", {3, 4}, true);
  const int32_t c = place(b, "PunchMech", {3, 6}, true);
  P(b, a).move = 2;
  P(b, c).move = 3;
  const int32_t h1 = place(b, "Hornet1", {3, 3});
  queue(b, h1, {3, 2});
  set_building(b, {3, 2});
  const int32_t h2 = place(b, "Hornet1", {5, 4});
  queue(b, h2, {6, 4});
  set_building(b, {6, 4});
  const TurnContext ctx = context();
  const SolveResult r = solve_turn(E, b, ctx, quick());
  CHECK(r.proven_optimal);
  CHECK(r.best.worst_case[ScoreKey::GridLost] == 0);
  CHECK(r.best.worst_case[ScoreKey::VekKilled] == 2);
  const auto again = evaluate_plan(E, b, ctx, r.best.actions);
  REQUIRE(again);
  CHECK(*again == r.best.worst_case);
  (void)a;
  (void)c;
}

TEST_CASE("score: the mech exchange slot (mech_grid)") {
  // Three mechs, grid 5 on turn 1 of 4.
  Board before;
  before.grid_power = 5;
  before.turn = 1;
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
    before.add_pawn(m);
  }
  Board dead = before;  // a mech lost, the grid kept
  dead.find_pawn(0)->hp = 0;
  Board grid1 = before;  // one grid lost, every mech kept
  grid1.grid_power = 4;
  Board grid2 = before;
  grid2.grid_power = 3;

  // Off: grid first, as always.
  CHECK(score_turn(before, dead) > score_turn(before, grid1));
  ScoreOptions o;
  CHECK(mech_grid_rate(before, o) == -1);
  // mech_grid 0: a mech outranks building HP, not one grid.
  o.mech_grid = 0;
  CHECK(score_turn(before, dead, nullptr, nullptr, o) > score_turn(before, grid1, nullptr, nullptr, o));
  // mech_grid 1: worth more than 1 grid, less than 2.
  o.mech_grid = 1;
  const Score d1 = score_turn(before, dead, nullptr, nullptr, o);
  CHECK(d1[ScoreKey::GridLost] == -3);
  CHECK(d1[ScoreKey::MechsLost] == -1);
  CHECK(d1 < score_turn(before, grid1, nullptr, nullptr, o));
  CHECK(d1 > score_turn(before, grid2, nullptr, nullptr, o));
  // Never the last grid: a plan that ends at grid 0 ranks below any other.
  Board out = before;
  out.grid_power = 0;
  o.mech_grid = 7;
  Board both = dead;
  both.find_pawn(1)->hp = 0;
  both.find_pawn(2)->hp = 0;
  both.grid_power = 1;
  CHECK(score_turn(before, out, nullptr, nullptr, o) < score_turn(before, both, nullptr, nullptr, o));
  // mech_grid_turns: capped by the turns left after this one.
  o.mech_grid = 2;
  o.mech_grid_turns = true;
  CHECK(mech_grid_rate(before, o) == 2);
  Board last = before;
  last.turn = 4;
  CHECK(mech_grid_rate(last, o) == 0);
  last.turn = 3;
  CHECK(mech_grid_rate(last, o) == 1);
}

void check_mech_grid_naive(Engine& E, const Board& b, const TurnContext& ctx, int rate, const Score& got);

TEST_CASE("solver: mech_grid keeps a mech for a building") {
  NEED_ENGINE();
  // Mech A (1 HP, done for the turn) is about to be stung by a Hornet; a
  // second Hornet is about to hit a building. Mech B can punch one of them.
  // Grid first saves the building and loses A; mech_grid >= 1 saves A.
  Board b;
  b.grid_power = 5;
  b.turn = 1;
  b.total_turns = 4;
  const int32_t a = place(b, "PunchMech", {6, 7}, true);
  P(b, a).hp = 1;
  P(b, a).active = false;
  const int32_t m = place(b, "PunchMech", {4, 4}, true);
  P(b, m).move = 3;
  const int32_t h1 = place(b, "Hornet1", {6, 6});
  queue(b, h1, {6, 7});
  const int32_t h2 = place(b, "Hornet1", {3, 3});
  queue(b, h2, {3, 2});
  set_building(b, {3, 2});
  const TurnContext ctx = context();

  const SolveResult off = solve_turn(E, b, ctx, quick());
  REQUIRE(off.proven_optimal);
  CHECK(off.best.worst_case[ScoreKey::GridLost] == 0);
  CHECK(off.best.worst_case[ScoreKey::MechsLost] == -1);

  SolveOptions o = quick();
  o.mech_grid = 1;
  const SolveResult on = solve_turn(E, b, ctx, o);
  REQUIRE(on.proven_optimal);
  CHECK(on.best.worst_case[ScoreKey::MechsLost] == 0);
  CHECK(on.best.worst_case[ScoreKey::GridLost] == -2);  // 2 x (-1 grid)
  CHECK(on.upper_bound == on.best.worst_case);
  const auto plain = evaluate_plan(E, b, ctx, on.best.actions);
  REQUIRE(plain);
  CHECK((*plain)[ScoreKey::GridLost] == -1);
  CHECK((*plain)[ScoreKey::BuildingHpLost] == -1);
  CHECK((*plain)[ScoreKey::MechsLost] == 0);
  const auto again = evaluate_plan(E, b, ctx, on.best.actions, o);
  REQUIRE(again);
  CHECK(*again == on.best.worst_case);

  // mech_grid 0 (a mech above building HP only) keeps the building.
  o.mech_grid = 0;
  const SolveResult zero = solve_turn(E, b, ctx, o);
  REQUIRE(zero.proven_optimal);
  CHECK(zero.best.worst_case[ScoreKey::MechsLost] == -1);
  // On the mission's last turn, mech_grid_turns caps the rate at 0.
  o.mech_grid = 1;
  o.mech_grid_turns = true;
  Board last = b;
  last.turn = 4;
  const SolveResult end = solve_turn(E, last, ctx, o);
  REQUIRE(end.proven_optimal);
  CHECK(end.best.worst_case[ScoreKey::MechsLost] == -1);
  // Never for the last grid power.
  Board low = b;
  low.grid_power = 1;
  o.mech_grid = 7;
  o.mech_grid_turns = false;
  const SolveResult keep = solve_turn(E, low, ctx, o);
  REQUIRE(keep.proven_optimal);
  CHECK(keep.best.worst_case[ScoreKey::MechsLost] == -1);
  CHECK(keep.best.worst_case[ScoreKey::GridLost] == -15);  // 2 x 0 + 15 x (-1)

  // The same values by naive exhaustive enumeration (defined further down).
  check_mech_grid_naive(E, b, ctx, 1, on.best.worst_case);
  check_mech_grid_naive(E, low, ctx, 7, keep.best.worst_case);
}

TEST_CASE("solver: a chance node whose worst case is not the default outcome") {
  NEED_ENGINE();
  // Lightning strike order (stage 7 V29): the psion struck first is the
  // default branch (no explosion); the Vek struck first explodes next to the
  // mech. The mech cannot act, so the only plan is to end the turn.
  Board b;
  place(b, "Jelly_Explode1", {1, 1});
  place(b, "Scorpion1", {5, 2});
  const int32_t m = place(b, "PunchMech", {5, 3}, true);
  P(b, m).active = false;
  for (const Pawn& p : b.pawns()) {
    if (p.alive() && p.leader != Leader::None) b.psion = p.leader;
  }
  TurnContext ctx = context("Mission_Lightning");
  for (Point p : {Point{1, 1}, Point{5, 2}, Point{1, 5}, Point{6, 6}}) {
    ctx.mission.danger.push_back(DangerTile{p, 1, true, -1});
  }
  // The default branch alone.
  Board d = b;
  E.end_turn(d, ctx);
  const Score dflt = score_turn(b, d, &ctx);
  CHECK(dflt[ScoreKey::MechHpLost] == 0);

  const SolveResult r = solve_turn(E, b, ctx, quick());
  CHECK(r.proven_optimal);
  CHECK(r.best.actions.empty());
  CHECK(r.best.worst_case[ScoreKey::MechHpLost] == -1);
  CHECK(r.best.worst_case < dflt);
  CHECK(r.stats.chance_branches >= 1);
}

TEST_CASE("solver: a resisted building is worse than a destroyed one") {
  NEED_ENGINE();
  // The Hornet hits the building (Grid Defense roll); then the Bouncer
  // pushes the mech towards it. Destroyed: the mech slides onto the rubble.
  // Resisted: the mech bumps into the building (1 HP) and the building rolls
  // again. The worst case is "resist, then no resist", not the default.
  Board b;
  const int32_t h = place(b, "Hornet1", {5, 3});
  queue(b, h, {4, 3});
  set_building(b, {4, 3});
  const int32_t bouncer = place(b, "Bouncer1", {2, 3});
  queue(b, bouncer, {3, 3});
  const int32_t m = place(b, "PunchMech", {3, 3}, true);
  P(b, m).active = false;
  const TurnContext ctx = context();

  auto run = [&](std::vector<bool> resists) {
    Board x = b;
    TurnContext tc = ctx;
    size_t n = 0;
    tc.grid_resist = [&](Point, int) { return n < resists.size() && resists[n++]; };
    E.end_turn(x, tc);
    return score_turn(b, x, &tc);
  };
  const Score none = run({});
  const Score once = run({true, false});
  CHECK(once < none);

  const SolveResult r = solve_turn(E, b, ctx, quick());
  CHECK(r.proven_optimal);
  CHECK(r.best.worst_case == std::min({none, once, run({true, true})}));
  CHECK(r.best.worst_case == once);
  // With Grid Defense off nothing resists: the default is the only outcome.
  Board off = b;
  off.grid_defense = 0;
  const SolveResult r0 = solve_turn(E, off, ctx, quick());
  CHECK(r0.best.worst_case == none);
}

TEST_CASE("solver: player actions run the mission's per-frame hooks (acid storm)") {
  NEED_ENGINE();
  // A shielded Scorpion (2 HP) and two artillery mechs that cannot move. The
  // first shell pops the shield; while the Storm Generator lives the next
  // frame sets ACID (Mission_AcidStorm:UpdateMission runs in the player's
  // turn too), so the second shell's 1 damage is doubled: a kill.
  Board b;
  place(b, "Storm_Generator", {0, 0});
  const int32_t v = place(b, "Scorpion1", {3, 3});
  P(b, v).hp = P(b, v).max_hp = 2;
  P(b, v).shield = true;
  P(b, v).acid = false;
  P(b, place(b, "ArtiMech", {3, 6}, true)).moved = true;
  P(b, place(b, "ArtiMech", {6, 3}, true)).moved = true;
  const TurnContext storm = context("Mission_AcidStorm");
  const SolveResult r = solve_turn(E, b, storm, quick());
  CHECK(r.proven_optimal);
  CHECK(r.best.worst_case[ScoreKey::VekKilled] == 1);
  CHECK(r.best.actions.size() == 2);
  const auto again = evaluate_plan(E, b, storm, r.best.actions);
  REQUIRE(again);
  CHECK(*again == r.best.worst_case);
  // No storm: the second shell does 1, no kill.
  const SolveResult calm = solve_turn(E, b, context(), quick());
  CHECK(calm.proven_optimal);
  CHECK(calm.best.worst_case[ScoreKey::VekKilled] == 0);
}

TEST_CASE("solver: anytime budget returns a plan and a sound bound") {
  NEED_ENGINE();
  Board b;
  place(b, "PunchMech", {3, 6}, true);
  place(b, "PunchMech", {4, 6}, true);
  place(b, "PunchMech", {5, 6}, true);
  const int32_t h = place(b, "Hornet1", {3, 3});
  queue(b, h, {3, 2});
  set_building(b, {3, 2});
  SolveOptions o;
  o.node_limit = 20;
  o.time_limit_s = 0;
  const SolveResult r = solve_turn(E, b, context(), o);
  CHECK_FALSE(r.proven_optimal);
  CHECK(r.timed_out);
  REQUIRE(r.upper_bound);
  CHECK(*r.upper_bound >= r.best.worst_case);
  CHECK(r.proven_components >= 0);
  const auto again = evaluate_plan(E, b, context(), r.best.actions);
  REQUIRE(again);
  CHECK(*again == r.best.worst_case);
}

// ---- Brute force cross-check -------------------------------------------------------

// A 4x4 corner of the board (the rest is mountains): 1-2 mechs of mixed
// types with move 1-2, 1-2 Vek with an adjacent queued target, two
// buildings (Grid Defense rolls happen), sometimes water.
static Board tiny_board(std::mt19937& rng) {
  static const char* kMechs[] = {"PunchMech", "TankMech", "ArtiMech", "LaserMech", "JudoMech", "ChargeMech"};
  static const char* kVek[] = {"Hornet1", "Scorpion1", "Firefly1"};
  auto pick = [&](int n) { return static_cast<int>(rng() % static_cast<unsigned>(n)); };
  Board b;
  for (int i = 0; i < kTileCount; ++i) {
    const Point p = Point::from_index(i);
    if (p.x >= 4 || p.y >= 4) set_mountain(b, p);
  }
  std::vector<Point> free;
  for (int x = 0; x < 4; ++x) {
    for (int y = 0; y < 4; ++y) free.push_back({x, y});
  }
  std::shuffle(free.begin(), free.end(), rng);
  size_t k = 0;
  set_building(b, free[k++]);
  set_building(b, free[k++]);
  if (pick(3) == 0) b.tile(free[k++]).terrain = Terrain::Water;
  const int mechs = 1 + pick(2);
  for (int i = 0; i < mechs; ++i) {
    const int32_t m = place(b, kMechs[pick(6)], free[k++], true);
    P(b, m).move = static_cast<int8_t>(1 + pick(2));
  }
  const int vek = 1 + pick(2);
  for (int i = 0; i < vek; ++i) {
    const Point at = free[k++];
    const int32_t h = place(b, kVek[pick(3)], at);
    std::vector<Point> adj;
    for (Point d : kDirVectors) {
      if ((at + d).valid()) adj.push_back(at + d);
    }
    queue(b, h, adj[static_cast<size_t>(pick(static_cast<int>(adj.size())))]);
  }
  return b;
}

TEST_CASE("solver: tiny boards match a naive exhaustive enumeration") {
  NEED_ENGINE();
  std::mt19937 rng(20261009);
  int checked = 0, with_chance = 0;
  constexpr int kTrials = 30;
  for (int trial = 0; trial < kTrials; ++trial) {
    const Board b = tiny_board(rng);
    const TurnContext ctx = context();
    Naive naive{E, b, ctx};
    const Score want = naive.value(b);
    for (bool tt : {true, false}) {
      SolveOptions o = quick(120);
      o.use_tt = tt;
      o.use_bounds = tt;
      o.order_children = tt;
      o.beam_width = tt ? 6 : 0;
      const SolveResult r = solve_turn(E, b, ctx, o);
      INFO("trial " << trial << " tt " << tt << " naive " << want.describe() << " solver "
                    << r.best.worst_case.describe());
      CHECK(r.proven_optimal);
      CHECK(r.best.worst_case == want);
      if (tt && r.stats.chance_branches > 0) ++with_chance;
    }
    ++checked;
  }
  CHECK(checked == kTrials);
  MESSAGE("brute-force boards: " << checked << ", with chance branches: " << with_chance);
}

void check_mech_grid_naive(Engine& E, const Board& b, const TurnContext& ctx, int rate, const Score& got) {
  Naive naive{E, b, ctx};
  naive.score_options.mech_grid = rate;
  CHECK(naive.value(b) == got);
}

TEST_CASE("solver: tiny boards with the mech exchange match a naive exhaustive enumeration") {
  NEED_ENGINE();
  // As above, with mechs at 1-2 HP so that keeping a mech and keeping a
  // building compete; the exchange's slot and bound against the naive value.
  std::mt19937 rng(20261011);
  int checked = 0, lost = 0, traded = 0;
  constexpr int kTrials = 30;
  for (int trial = 0; trial < kTrials; ++trial) {
    Board b = tiny_board(rng);
    b.grid_power = 1 + static_cast<int>(rng() % 3u);  // sometimes the last grid point is at stake
    bool first = true;
    for (Pawn& p : b.pawns()) {
      if (!p.mech) continue;
      p.hp = static_cast<int8_t>(1 + rng() % 2u);
      if (first && rng() % 3u != 0) p.active = false;  // done for the turn: cannot step away
      first = false;
    }
    // Vek next to a mech attack it (each mech at most once), so that a mech
    // and a building are both at stake.
    std::vector<Point> mechs;
    for (const Pawn& p : b.pawns()) {
      if (p.mech) mechs.push_back(p.pos);
    }
    for (Pawn& p : b.pawns()) {
      if (p.team != Team::Enemy) continue;
      for (auto it = mechs.begin(); it != mechs.end(); ++it) {
        if (std::abs(it->x - p.pos.x) + std::abs(it->y - p.pos.y) == 1) {
          p.queued = QueuedShot{0, p.pos, *it};
          mechs.erase(it);
          break;
        }
      }
    }
    const TurnContext ctx = context();
    for (int rate : {0, 1}) {
      Naive naive{E, b, ctx};
      naive.score_options.mech_grid = rate;
      const Score want = naive.value(b);
      Naive plain{E, b, ctx};
      const Score base = plain.value(b);
      if (rate == 1 && base[ScoreKey::MechsLost] < 0) ++lost;
      if (rate == 1 && base[ScoreKey::MechsLost] < want[ScoreKey::MechsLost]) ++traded;
      for (bool tt : {true, false}) {
        SolveOptions o = quick(120);
        o.mech_grid = rate;
        o.use_tt = tt;
        o.use_bounds = tt;
        o.order_children = tt;
        o.beam_width = tt ? 6 : 0;
        const SolveResult r = solve_turn(E, b, ctx, o);
        INFO("trial " << trial << " rate " << rate << " tt " << tt << " naive " << want.describe() << " solver "
                      << r.best.worst_case.describe());
        CHECK(r.proven_optimal);
        CHECK(r.best.worst_case == want);
      }
    }
    ++checked;
  }
  CHECK(checked == kTrials);
  MESSAGE("mech exchange brute-force boards: " << checked << ", a mech lost by the plain order: " << lost
                                              << ", kept for grid with mech_grid 1: " << traded);
}

TEST_CASE("solver: tiny boards with the lookahead match a naive exhaustive enumeration") {
  NEED_ENGINE();
  // The NextTurn tier is a function of the end board; with it the search's
  // lazy evaluation, per-sample cutoffs, caches and the tier's cap (0) must
  // still give the exhaustive value.
  std::mt19937 rng(20261013);
  int checked = 0, nonzero = 0;
  constexpr int kTrials = 24;
  LookaheadOptions la;
  la.samples = 2;
  for (int trial = 0; trial < kTrials; ++trial) {
    la.sum = trial % 2 == 1;  // both folds
    Board b = tiny_board(rng);
    // Fewer answers, so that next turns lose something: one more Vek, no
    // queued attack yet, and the first mech done for this turn.
    for (int i = 0; i < kTileCount; ++i) {
      const Point p = Point::from_index(i);
      if (p.x < 4 && p.y < 4 && !b.pawn_at(p) && b.tile(p).terrain != Terrain::Building) {
        place(b, "Firefly1", p);
        break;
      }
    }
    for (Pawn& p : b.pawns()) {
      if (p.mech) {
        p.active = false;
        break;
      }
    }
    const TurnContext ctx = context();
    Naive naive{E, b, ctx};
    naive.lookahead = &la;
    naive.score_options.next_turn = true;
    const Score want = naive.value(b);
    if (want[ScoreKey::NextTurn] < 0) ++nonzero;
    for (bool tt : {true, false}) {
      SolveOptions o = quick(300);
      o.lookahead = true;
      o.lookahead_options = la;
      o.use_tt = tt;
      o.use_bounds = tt;
      o.order_children = tt;
      o.beam_width = tt ? 6 : 0;
      const SolveResult r = solve_turn(E, b, ctx, o);
      INFO("trial " << trial << " tt " << tt << " naive " << want.describe(kTierNextTurn) << " solver "
                    << r.best.worst_case.describe(kTierNextTurn));
      CHECK(r.proven_optimal);
      CHECK(r.best.worst_case == want);
    }
    ++checked;
  }
  CHECK(checked == kTrials);
  MESSAGE("lookahead brute-force boards: " << checked << ", with a next-turn loss: " << nonzero);
}

TEST_CASE("solver: tiny boards with the pilot floor match a naive exhaustive enumeration") {
  NEED_ENGINE();
  // The mission's last turn, mech_grid 2 capped by the turns left (0), each
  // mech's pilot at level 0-2: per-mech rates in the slot and in its bound.
  std::mt19937 rng(20261014);
  int checked = 0;
  constexpr int kTrials = 12;
  for (int trial = 0; trial < kTrials; ++trial) {
    Board b = tiny_board(rng);
    b.turn = b.total_turns;
    b.grid_power = 2 + static_cast<int>(rng() % 2u);
    std::vector<Point> mechs;
    for (Pawn& p : b.pawns()) {
      if (!p.mech) continue;
      p.hp = static_cast<int8_t>(1 + rng() % 2u);
      p.pilot_level = static_cast<int8_t>(rng() % 3u);
      mechs.push_back(p.pos);
    }
    for (Pawn& p : b.pawns()) {
      if (p.team != Team::Enemy) continue;
      for (auto it = mechs.begin(); it != mechs.end(); ++it) {
        if (std::abs(it->x - p.pos.x) + std::abs(it->y - p.pos.y) == 1) {
          p.queued = QueuedShot{0, p.pos, *it};
          mechs.erase(it);
          break;
        }
      }
    }
    const TurnContext ctx = context();
    Naive naive{E, b, ctx};
    naive.score_options.mech_grid = 2;
    naive.score_options.mech_grid_turns = true;
    naive.score_options.mech_grid_pilot = true;
    const Score want = naive.value(b);
    for (bool tt : {true, false}) {
      SolveOptions o = quick(120);
      o.mech_grid = 2;
      o.mech_grid_turns = true;
      o.mech_grid_pilot = true;
      o.use_tt = tt;
      o.use_bounds = tt;
      o.order_children = tt;
      o.beam_width = tt ? 6 : 0;
      const SolveResult r = solve_turn(E, b, ctx, o);
      INFO("trial " << trial << " tt " << tt << " naive " << want.describe() << " solver "
                    << r.best.worst_case.describe());
      CHECK(r.proven_optimal);
      CHECK(r.best.worst_case == want);
    }
    ++checked;
  }
  CHECK(checked == kTrials);
}

// ---- Grid survival (SolveOptions::grid_survival) -------------------------------------

namespace {

// Two attacks on 1-HP buildings or one on a 3-HP building: with the grid at 1
// every line loses it unless Grid Defense saves it. The Laser Mech (it
// cannot move) kills the Leaper to the left (one 3-damage hit stopped, two
// 1-damage hits left: two rolls, 2 building HP at worst) or both Hornets
// below (one hit left: one roll, 3 building HP at worst).
Board two_roll_board(int32_t* mech) {
  Board b;
  b.grid_power = 1;
  const int32_t m = place(b, "LaserMech", {3, 3}, true);
  P(b, m).moved = true;
  const int32_t a = place(b, "Leaper1", {2, 3});
  set_building(b, {2, 2}, 3);
  queue(b, a, {2, 2});
  set_mountain(b, {1, 3});
  const int32_t h1 = place(b, "Hornet1", {3, 4});
  set_building(b, {4, 4});
  queue(b, h1, {4, 4});
  const int32_t h2 = place(b, "Hornet1", {3, 5});
  set_building(b, {4, 5});
  queue(b, h2, {4, 5});
  set_mountain(b, {3, 6});
  if (mech) *mech = m;
  return b;
}

}  // namespace

TEST_CASE("grid survival: when every line loses the grid, the one-roll line beats the two-roll line") {
  NEED_ENGINE();
  int32_t m = -1;
  const Board b = two_roll_board(&m);
  TurnContext ctx = context();
  ctx.mission.grid_defense = 25;
  const double p = 0.25;
  auto fire = [&](Point t) {
    PlayerAction a;
    a.uid = m;
    a.kind = PlayerAction::Kind::Weapon;
    a.weapon = std::string(symbol_name(P(const_cast<Board&>(b), m).weapons[0]));
    a.target = t;
    return std::vector<PlayerAction>{a};
  };
  SolveOptions on = quick();
  on.grid_survival = true;
  const auto down = evaluate_plan(E, b, ctx, fire({3, 4}), on);  // both Hornets: one roll left
  const auto left = evaluate_plan(E, b, ctx, fire({2, 3}), on);  // the Leaper: two rolls left
  REQUIRE(down);
  REQUIRE(left);
  CHECK((*down)[ScoreKey::GridSurvival] == static_cast<int32_t>(std::llround(p * kSurvivalOne)));
  CHECK((*left)[ScoreKey::GridSurvival] == static_cast<int32_t>(std::llround(p * p * kSurvivalOne)));
  // The rest of a line with odds above 0 is its worst outcome that keeps the
  // grid: every building resisted.
  CHECK((*down)[ScoreKey::GridLost] == 0);
  CHECK((*left)[ScoreKey::BuildingHpLost] == 0);
  // The worst cases: both lose the grid, the two-roll line fewer building HP.
  const auto down_off = evaluate_plan(E, b, ctx, fire({3, 4}), quick());
  const auto left_off = evaluate_plan(E, b, ctx, fire({2, 3}), quick());
  REQUIRE(down_off);
  REQUIRE(left_off);
  CHECK((*down_off)[ScoreKey::GridLost] == -1);
  CHECK((*left_off)[ScoreKey::GridLost] == -1);
  CHECK((*left_off)[ScoreKey::BuildingHpLost] > (*down_off)[ScoreKey::BuildingHpLost]);

  // Off: every line ends with the grid lost at worst; fewer building HP
  // lost wins, i.e. the two-roll line.
  const SolveResult r_off = solve_turn(E, b, ctx, quick());
  CHECK(r_off.proven_optimal);
  REQUIRE(r_off.best.actions.size() == 1);
  CHECK(r_off.best.actions[0].target == Point{2, 3});
  CHECK(r_off.best.worst_case[ScoreKey::GridSurvival] == 0);

  // On: the odds decide first.
  const SolveResult r_on = solve_turn(E, b, ctx, on);
  CHECK(r_on.proven_optimal);
  REQUIRE(r_on.best.actions.size() == 1);
  CHECK(r_on.best.actions[0].target == Point{3, 4});
  CHECK(r_on.best.worst_case == *down);
  for (const std::string& w : r_on.warnings) CHECK_MESSAGE(w.rfind("re-running", 0) != 0, w);

  // The game's chance comes from the state; without it the board's (15%).
  TurnContext unknown = context();
  const auto down15 = evaluate_plan(E, b, unknown, fire({3, 4}), on);
  REQUIRE(down15);
  CHECK((*down15)[ScoreKey::GridSurvival] == static_cast<int32_t>(std::llround(0.15 * kSurvivalOne)));
}

TEST_CASE("grid survival: a line that keeps the grid scores 1 and ranks as before") {
  NEED_ENGINE();
  // Grid 2: killing the Leaper leaves two 1-HP hits, the grid at 0 at worst;
  // killing the Hornets leaves one 3-damage hit on a 3-HP building: grid 0
  // too. With grid 4 both keep the grid: the plans rank as without the odds.
  int32_t m = -1;
  Board b = two_roll_board(&m);
  b.grid_power = 4;
  TurnContext ctx = context();
  ctx.mission.grid_defense = 25;
  const SolveResult off = solve_turn(E, b, ctx, quick());
  SolveOptions o = quick();
  o.grid_survival = true;
  const SolveResult on = solve_turn(E, b, ctx, o);
  CHECK(off.proven_optimal);
  CHECK(on.proven_optimal);
  CHECK(on.best.worst_case[ScoreKey::GridSurvival] == kSurvivalOne);
  Score same = on.best.worst_case;
  same[ScoreKey::GridSurvival] = 0;
  CHECK(same == off.best.worst_case);
  REQUIRE(on.best.actions.size() == off.best.actions.size());
  for (size_t i = 0; i < on.best.actions.size(); ++i) CHECK(on.best.actions[i].target == off.best.actions[i].target);
}

// A tiny board (tiny_board) with the grid at 1-2 and more of it at stake: Vek
// next to a building attack it, a Hornet joins next to some buildings, and
// often the first mech is done for the turn.
static Board grid_risk_board(std::mt19937& rng) {
  Board b = tiny_board(rng);
  b.grid_power = 1 + static_cast<int>(rng() % 2u);
  for (int i = 0; i < kTileCount; ++i) {
    const Point at = Point::from_index(i);
    if (!b.tile(at).is_building()) continue;
    for (Point d : kDirVectors) {
      const Point q = at + d;
      if (q.valid() && q.x < 4 && q.y < 4 && !b.pawn_at(q) && b.tile(q).terrain == Terrain::Road &&
          rng() % 2u == 0) {
        queue(b, place(b, "Hornet1", q), at);
        break;
      }
    }
  }
  for (Pawn& p : b.pawns()) {
    if (p.mech) {
      if (rng() % 2u == 0) p.active = false;
      break;
    }
  }
  for (Pawn& p : b.pawns()) {
    if (p.team != Team::Enemy) continue;
    for (Point d : kDirVectors) {
      const Point t = p.pos + d;
      if (t.valid() && b.tile(t).is_building()) {
        p.queued = QueuedShot{0, p.pos, t};
        break;
      }
    }
  }
  return b;
}

TEST_CASE("solver: tiny boards with grid survival match a naive exhaustive enumeration") {
  NEED_ENGINE();
  // The naive value folds every enemy phase's chance tree node by node; the
  // player's own chance stays at its worst. On a board where some line keeps
  // the grid for sure the value is the plain one with odds 1.
  std::mt19937 rng(20261015);
  int checked = 0, doomed = 0, positive = 0, odds_decide = 0, player_chance = 0;
  constexpr int kTrials = 30;
  for (int trial = 0; trial < kTrials; ++trial) {
    const Board b = grid_risk_board(rng);
    TurnContext ctx = context();
    static constexpr int kDefense[] = {15, 25, 40, 5, 0};
    ctx.mission.grid_defense = kDefense[trial % 5];
    Naive naive{E, b, ctx};
    naive.score_options.grid_survival = true;
    naive.resist_p = ctx.mission.grid_defense / 100.0;
    const Score want = naive.value(b);
    Naive plain{E, b, ctx};
    const Score base = plain.value(b);
    if (want[ScoreKey::GridSurvival] == kSurvivalOne) {
      Score same = want;
      same[ScoreKey::GridSurvival] = 0;
      CHECK(same == base);
    } else {
      ++doomed;
      if (want[ScoreKey::GridSurvival] > 0) ++positive;
    }
    if (naive.player_chance > 0) ++player_chance;
    for (bool tt : {true, false}) {
      SolveOptions o = quick(120);
      o.grid_survival = true;
      o.use_tt = tt;
      o.use_bounds = tt;
      o.order_children = tt;
      o.beam_width = tt ? 6 : 0;
      const SolveResult r = solve_turn(E, b, ctx, o);
      INFO("trial " << trial << " tt " << tt << " naive " << want.describe(kTierSurvival) << " solver "
                    << r.best.worst_case.describe(kTierSurvival));
      CHECK(r.proven_optimal);
      CHECK(r.best.worst_case == want);
      if (!r.best.contingent) {
        const auto again = evaluate_plan(E, b, ctx, r.best.actions, o);
        REQUIRE(again);
        CHECK(*again == r.best.worst_case);
        // The odds picked a line the plain order ranks lower.
        const auto plain_value = evaluate_plan(E, b, ctx, r.best.actions);
        REQUIRE(plain_value);
        if (tt && *plain_value < base) ++odds_decide;
      }
    }
    ++checked;
  }
  CHECK(checked == kTrials);
  MESSAGE("grid survival brute-force boards: " << checked << ", every line can lose the grid: " << doomed
                                              << " (odds above 0: " << positive << "; the odds picked a line the "
                                              << "plain order ranks lower: " << odds_decide
                                              << "), with chance in the player's actions: " << player_chance);
}

TEST_CASE("solver: grid survival with the lookahead and the mech exchange matches a naive enumeration") {
  NEED_ENGINE();
  // The odds in front of the lookahead's lazy leaves (and its base search)
  // and of the mech exchange's grid slot.
  std::mt19937 rng(20261016);
  LookaheadOptions la;
  la.samples = 2;
  int checked = 0, doomed = 0;
  constexpr int kTrials = 6;
  for (int trial = 0; trial < kTrials; ++trial) {
    const Board b = grid_risk_board(rng);
    TurnContext ctx = context();
    ctx.mission.grid_defense = 25;
    const bool with_la = trial % 2 == 0;
    Naive naive{E, b, ctx};
    naive.score_options.grid_survival = true;
    naive.resist_p = 0.25;
    if (with_la) {
      naive.lookahead = &la;
      naive.score_options.next_turn = true;
    } else {
      naive.score_options.mech_grid = 1;
    }
    const Score want = naive.value(b);
    if (want[ScoreKey::GridSurvival] < kSurvivalOne) ++doomed;
    for (bool tt : {true, false}) {
      SolveOptions o = quick(300);
      o.grid_survival = true;
      if (with_la) {
        o.lookahead = true;
        o.lookahead_options = la;
      } else {
        o.mech_grid = 1;
      }
      o.use_tt = tt;
      o.use_bounds = tt;
      o.order_children = tt;
      o.beam_width = tt ? 6 : 0;
      const SolveResult r = solve_turn(E, b, ctx, o);
      const TierSet shown = kTierSurvival | (with_la ? kTierNextTurn : 0u);
      INFO("trial " << trial << " tt " << tt << " naive " << want.describe(shown) << " solver "
                    << r.best.worst_case.describe(shown));
      CHECK(r.proven_optimal);
      CHECK(r.best.worst_case == want);
    }
    ++checked;
  }
  CHECK(checked == kTrials);
  MESSAGE("grid survival with lookahead / mech exchange: " << checked << " boards, " << doomed
                                                          << " where every line can lose the grid");
}

// ---- Threads and recorded boards ----------------------------------------------------

namespace {

std::vector<Engine*> helpers(size_t n) {
  static std::vector<std::unique_ptr<Engine>> pool;
  while (pool.size() < n) pool.push_back(Engine::create(GameData::default_game_root()));
  std::vector<Engine*> out;
  for (size_t i = 0; i < n; ++i) out.push_back(pool[i].get());
  return out;
}

}  // namespace

TEST_CASE("solver: several threads prove the same value as one") {
  NEED_ENGINE();
  Board b;
  const int32_t a = place(b, "PunchMech", {3, 4}, true);
  const int32_t c = place(b, "PunchMech", {3, 6}, true);
  P(b, a).move = 2;
  P(b, c).move = 3;
  const int32_t h1 = place(b, "Hornet1", {3, 3});
  queue(b, h1, {3, 2});
  set_building(b, {3, 2});
  const int32_t h2 = place(b, "Scorpion1", {5, 4});
  queue(b, h2, {6, 4});
  set_building(b, {6, 4});
  const TurnContext ctx = context();
  const SolveResult one = solve_turn(E, b, ctx, quick());
  SolveOptions o = quick();
  o.helper_engines = helpers(3);
  const SolveResult four = solve_turn(E, b, ctx, o);
  CHECK(four.stats.threads == 4);
  REQUIRE(one.proven_optimal);
  REQUIRE(four.proven_optimal);
  CHECK(one.best.worst_case == four.best.worst_case);
  const auto again = evaluate_plan(E, b, ctx, four.best.actions);
  REQUIRE(again);
  CHECK(*again == four.best.worst_case);
}

TEST_CASE("solver: a recorded board, anytime and threaded, stays consistent") {
  NEED_ENGINE();
  std::string error;
  auto rec = load_recording(std::string(ITB_FIXTURE_DIR) + "/board_m07_turn01.json", &E.data(), &error);
  REQUIRE_MESSAGE(rec, error);
  Board board = rec->board;
  SolveOptions o;
  for (const auto& [uid, pilot] : rec->pilots) {
    if (Pawn* p = board.find_pawn(uid)) {
      p->pilot_abilities |= E.pilot_ability(pilot);
      o.repair_skills.emplace_back(uid, E.repair_skill(pilot));
    }
  }
  TurnContext ctx;
  ctx.mission = rec->mission;
  o.time_limit_s = 3;
  o.helper_engines = helpers(2);
  const SolveResult r = solve_turn(E, board, ctx, o);
  REQUIRE(r.upper_bound);
  CHECK(*r.upper_bound >= r.best.worst_case);
  // Doing nothing is a plan too: the search can only do better.
  const auto idle = evaluate_plan(E, board, ctx, {}, o);
  REQUIRE(idle);
  CHECK(r.best.worst_case >= *idle);
  if (!r.best.contingent) {
    const auto again = evaluate_plan(E, board, ctx, r.best.actions, o);
    REQUIRE(again);
    CHECK(*again == r.best.worst_case);
  }
  for (const std::string& w : r.warnings) CHECK_MESSAGE(w.rfind("re-running", 0) != 0, w);
}

// ---- Adaptive budget (SolveOptions::min_proven_tiers, max_time_s) ------------------

namespace {

bool same_tiers(const Score& a, const Score& b, int tiers) {
  for (int i = 0; i < tiers; ++i) {
    if (a.v[static_cast<size_t>(i)] != b.v[static_cast<size_t>(i)]) return false;
  }
  return true;
}

SolveOptions recorded_options(const Recording& rec, Board& board) {
  SolveOptions o;
  for (const auto& [uid, pilot] : rec.pilots) {
    if (Pawn* p = board.find_pawn(uid)) {
      p->pilot_abilities |= engine()->pilot_ability(pilot);
      o.repair_skills.emplace_back(uid, engine()->repair_skill(pilot));
    }
  }
  return o;
}

}  // namespace

TEST_CASE("solver: adaptive budget, unproven at the base time, proves the first tiers") {
  NEED_ENGINE();
  // A base time that runs out at once: the tier target is never met then (no
  // plan yet), so the search goes on proving only the first k tiers. It must
  // complete, be proven in k tiers, and agree with the full search there.
  std::mt19937 rng(20261010);
  int extended = 0, short_of_full = 0;
  for (int trial = 0; trial < 12; ++trial) {
    const Board b = tiny_board(rng);
    const TurnContext ctx = context();
    const SolveResult full = solve_turn(E, b, ctx, quick(120));
    REQUIRE(full.proven_optimal);
    for (int k : {1, 2, 4, 5}) {
      SolveOptions o = quick(1e-9);
      o.min_proven_tiers = k;
      o.max_time_s = 120;
      const SolveResult r = solve_turn(E, b, ctx, o);
      INFO("trial " << trial << " k " << k << " full " << full.best.worst_case.describe() << " adaptive "
                    << r.best.worst_case.describe());
      CHECK(r.extended);
      CHECK(r.proven_components >= k);
      REQUIRE(r.upper_bound);
      CHECK(*r.upper_bound >= r.best.worst_case);
      CHECK(*r.upper_bound >= full.best.worst_case);  // sound
      CHECK(same_tiers(r.best.worst_case, full.best.worst_case, components_for_tiers(k, false)));
      CHECK(r.best.worst_case <= full.best.worst_case);
      if (r.proven_optimal) CHECK(r.best.worst_case == full.best.worst_case);
      if (!r.best.contingent) {
        const auto again = evaluate_plan(E, b, ctx, r.best.actions);
        REQUIRE(again);
        CHECK(*again == r.best.worst_case);
      }
      if (r.extended) ++extended;
      if (r.best.worst_case < full.best.worst_case) ++short_of_full;
    }
  }
  MESSAGE("adaptive tiny boards: " << extended << " extended, " << short_of_full
                                   << " plans below the full optimum after the first tiers");
}

TEST_CASE("solver: adaptive budget stops at the base time once the tiers are proven") {
  NEED_ENGINE();
  // Nothing threatens a building: ending the turn at once already keeps the
  // grid, so the grid tier is proven from the first plan, and the search
  // stops at the base time even though it is far from complete.
  Board b;
  place(b, "PunchMech", {3, 6}, true);
  place(b, "PunchMech", {4, 6}, true);
  place(b, "PunchMech", {5, 6}, true);
  const int32_t h = place(b, "Hornet1", {3, 3});
  queue(b, h, {3, 2});
  set_building(b, {6, 1});
  SolveOptions o = quick(0.3);
  o.max_time_s = 60;
  o.min_proven_tiers = 1;
  const SolveResult r = solve_turn(E, b, context(), o);
  MESSAGE("base-time stop: " << r.stats.time_s << " s, " << r.best.worst_case.describe() << ", proven in "
                             << r.proven_components << " tiers");
  CHECK_FALSE(r.extended);
  CHECK(r.proven_components >= 1);
  if (!r.proven_optimal) {
    CHECK(r.timed_out);
    CHECK(r.stats.time_s >= 0.3);
    CHECK(r.stats.time_s < 10);  // stopped near the base time, not the cap
  }
}

TEST_CASE("solver: adaptive budget stops once a later plan meets the tier target") {
  NEED_ENGINE();
  // On this recorded board the first plans lose grid; the one that keeps it
  // takes a while to find. Past the base time the search goes on until it
  // has it (the grid tier is then proven), well before the cap.
  std::string error;
  auto rec = load_recording(std::string(ITB_FIXTURE_DIR) + "/board_m07_turn01.json", &E.data(), &error);
  REQUIRE_MESSAGE(rec, error);
  Board board = rec->board;
  SolveOptions o = recorded_options(*rec, board);
  TurnContext ctx;
  ctx.mission = rec->mission;
  o.time_limit_s = 0.05;
  o.max_time_s = 60;
  o.min_proven_tiers = 1;
  const SolveResult r = solve_turn(E, board, ctx, o);
  MESSAGE("target stop: " << r.stats.time_s << " s, best plan at " << r.stats.best_plan_s << " s, "
                          << r.best.worst_case.describe() << ", proven in " << r.proven_components << " tiers");
  CHECK(r.proven_components >= 1);
  CHECK(r.stats.time_s < 30);
  if (r.extended) CHECK(r.stats.time_s >= 0.05);
}

TEST_CASE("solver: adaptive budget runs to the cap while the tiers are unproven") {
  NEED_ENGINE();
  // Every tier (Position has no cap, so only a completed search proves it):
  // the search runs past the base time to the cap, then stops.
  std::string error;
  auto rec = load_recording(std::string(ITB_FIXTURE_DIR) + "/board_m07_turn01.json", &E.data(), &error);
  REQUIRE_MESSAGE(rec, error);
  Board board = rec->board;
  SolveOptions o = recorded_options(*rec, board);
  TurnContext ctx;
  ctx.mission = rec->mission;
  o.time_limit_s = 0.2;
  o.max_time_s = 0.8;
  o.min_proven_tiers = used_tier_count(false);
  const SolveResult r = solve_turn(E, board, ctx, o);
  MESSAGE("cap stop: " << r.stats.time_s << " s, proven " << r.proven_optimal << ", in "
                       << r.proven_components << " tiers");
  CHECK(r.extended);
  if (!r.proven_optimal) {
    CHECK(r.timed_out);
    CHECK(r.stats.time_s >= 0.8);
  }
  REQUIRE(r.upper_bound);
  CHECK(*r.upper_bound >= r.best.worst_case);
  // Without the adaptive options the same base time stops at once.
  o.min_proven_tiers = 0;
  const SolveResult base = solve_turn(E, board, ctx, o);
  CHECK_FALSE(base.extended);
  if (!base.proven_optimal) CHECK(base.stats.time_s < 0.8);
}
