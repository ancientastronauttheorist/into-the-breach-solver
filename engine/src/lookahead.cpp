// Next-turn lookahead (lookahead.hpp).

#include "itb/lookahead.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>

#include "itb/ai_phase.hpp"
#include "itb/deploy.hpp"
#include "itb/engine.hpp"
#include "itb/game_data.hpp"
#include "itb/movement.hpp"
#include "itb/solver.hpp"

namespace itb {
namespace {

// The Lua stream is reseeded before every engine run (as the solver does),
// so a reply is a function of the board alone.
constexpr uint32_t kLuaSeed = 1;

bool scored_enemy(const Pawn& p) { return p.team == Team::Enemy && !p.neutral && !p.mech; }

// Enemy types a hidden spawn may be: the scored enemies the player can see on
// the turn's board, minus bosses, minor pawns (eggs, blobs) and mission units;
// sorted, so the sample is a function of the board.
std::vector<std::string> visible_spawn_types(const Engine& engine, const Board& root) {
  std::set<std::string> out;
  for (const Pawn& p : root.pawns()) {
    if (!scored_enemy(p) || !p.alive()) continue;
    const PawnDef* def = engine.data().pawn(p.type);
    if (!def || def->tier == Tier::Boss || def->minor || def->mission || def->skills.empty()) continue;
    out.insert(def->name);
  }
  return {out.begin(), out.end()};
}

bool worse(const NextTurnLoss& a, const NextTurnLoss& b) { return next_turn_value(a) < next_turn_value(b); }

// ---- Greedy reply --------------------------------------------------------------
//
// One unit turn: an optional move, then an optional weapon or repair.
struct UnitTurn {
  int32_t uid = -1;
  Point move = kInvalidPoint;
  int kind = 0;  // 0 none, 1 weapon, 2 repair
  int slot = -1;
  std::string skill;
  Point target = kInvalidPoint;
  Point target2 = kInvalidPoint;
};

class Greedy {
 public:
  Greedy(Engine& e, const TurnContext& ctx, const LookaheadOptions& o) : e_(e), ctx_(ctx), o_(o) {
    ao_ = action_options(ctx_);
    ao_.check_legal = true;
  }

  // Rounds: the unit turn (of a unit that has not played) with the best
  // (loss, pressure) after it, taken while it improves on the board so far;
  // the pressure (queued attackers and their HP) lets a round that cannot
  // lower the loss alone (two Vek on one building) still make progress.
  NextTurnLoss run(const Board& start, std::vector<PlayerAction>* plan) {
    Board cur = start;
    NextTurnLoss best = idle_loss(e_, cur, ctx_);
    int best_pressure = pressure(cur);
    std::set<int32_t> played;
    for (int round = 0; round < o_.greedy_rounds; ++round) {
      if (next_turn_value(best) == 0) break;
      relevant_ = relevant_tiles(cur);
      std::optional<Board> pick;
      int32_t pick_uid = -1;
      UnitTurn pick_turn;
      NextTurnLoss pick_loss = best;
      int pick_pressure = best_pressure;
      for (const Pawn& p : cur.pawns()) {
        if (played.count(p.uid) || !p.controlled() || !p.alive() || !p.pos.valid() || !p.active) continue;
        unit_turns(cur, p, [&](const UnitTurn& t, Board&& b) {
          if (!touches_relevant(cur, b)) return true;
          const NextTurnLoss l = end_loss(start, b);
          const int pr = pressure(b);
          const int32_t lv = next_turn_value(l), pv = next_turn_value(pick_loss);
          if (lv > pv || (lv == pv && pr < pick_pressure)) {
            pick_loss = l;
            pick_pressure = pr;
            pick = std::move(b);
            pick_uid = p.uid;
            pick_turn = t;
            if (lv == 0) return false;
          }
          return true;
        });
        if (pick && next_turn_value(pick_loss) == 0) break;
      }
      if (!pick) break;
      cur = std::move(*pick);
      best = pick_loss;
      best_pressure = pick_pressure;
      played.insert(pick_uid);
      if (plan) plan->push_back(to_action(pick_turn));
    }
    return best;
  }

 private:
  // Queued attackers: 16 per attacker plus its HP (lower is better).
  static int pressure(const Board& b) {
    int v = 0;
    for (const Pawn& p : b.pawns()) {
      if (p.team == Team::Enemy && p.alive() && p.pos.valid() && p.queued.active() && !p.frozen) v += 16 + p.hp;
    }
    return v;
  }

  // The turn's losses from its start to the end of the enemy phase after
  // the actions that led to `b` (their own damage counts too).
  NextTurnLoss end_loss(const Board& start, const Board& b) {
    Board end = b;
    e_.lua().seed(kLuaSeed);
    const PhaseResult pr = e_.end_turn(end, ctx_);
    return turn_loss(start, end, ctx_, pr);
  }

  static PlayerAction to_action(const UnitTurn& t) {
    PlayerAction a;
    a.uid = t.uid;
    a.move = t.move;
    if (t.kind == 1) {
      a.kind = PlayerAction::Kind::Weapon;
      a.weapon = t.skill;
      a.target = t.target;
      if (t.target2.valid()) a.target2 = t.target2;
    } else if (t.kind == 2) {
      a.kind = PlayerAction::Kind::Repair;
      a.weapon = t.skill;
      a.target = t.target;
    }
    return a;
  }

  // Tiles an answer must touch: each queued attacker's tile, its target and
  // the straight segment between them, with their neighbours.
  std::vector<Point> relevant_tiles(const Board& b) {
    std::vector<char> mark(kTileCount, 0);
    auto add = [&](Point p) {
      if (p.valid()) mark[static_cast<size_t>(p.index())] = 1;
    };
    auto add_around = [&](Point p) {
      if (!p.valid()) return;
      add(p);
      for (int d = 0; d < 4; ++d) add(step(p, static_cast<Dir>(d)));
    };
    for (const Pawn& p : b.pawns()) {
      if (p.team != Team::Enemy || !p.alive() || !p.pos.valid() || !p.queued.active()) continue;
      add_around(p.pos);
      add_around(p.queued.target);
      const Point a = p.pos, t = p.queued.target;
      if (a.x == t.x || a.y == t.y) {
        const int dx = (t.x > a.x) - (t.x < a.x), dy = (t.y > a.y) - (t.y < a.y);
        for (Point q = a; q.valid() && q != t; q = Point{q.x + dx, q.y + dy}) add(q);
      }
    }
    std::vector<Point> out;
    for (int i = 0; i < kTileCount; ++i) {
      if (mark[static_cast<size_t>(i)]) out.push_back(Point::from_index(i));
    }
    return out;
  }

  bool is_relevant(Point p) const {
    return std::find(relevant_.begin(), relevant_.end(), p) != relevant_.end();
  }

  // Whether a projectile from `from` toward `t` passes over a relevant
  // attacker first (same row or column, the attacker between them).
  bool in_line_with_attacker(const Board& b, Point from, Point t) const {
    if (from.x != t.x && from.y != t.y) return false;
    for (const Pawn& p : b.pawns()) {
      if (p.team != Team::Enemy || !p.alive() || !p.pos.valid() || !p.queued.active()) continue;
      const Point a = p.pos;
      if (from.x == t.x && a.x == from.x && ((a.y - from.y) * (t.y - from.y) > 0) &&
          std::abs(a.y - from.y) <= std::abs(t.y - from.y)) {
        return true;
      }
      if (from.y == t.y && a.y == from.y && ((a.x - from.x) * (t.x - from.x) > 0) &&
          std::abs(a.x - from.x) <= std::abs(t.x - from.x)) {
        return true;
      }
    }
    return false;
  }

  bool target_ok(const Board& b, Point from, Point t) const {
    if (!t.valid()) return false;
    if (is_relevant(t)) return true;
    for (int d = 0; d < 4; ++d) {
      if (is_relevant(step(t, static_cast<Dir>(d)))) return true;
    }
    return in_line_with_attacker(b, from, t);
  }

  bool passive(Symbol w) {
    auto it = passive_.find(w);
    if (it != passive_.end()) return it->second;
    const std::string name(symbol_name(w));
    const std::optional<std::string> p = e_.lua().lua_string(name, "Passive");
    const bool is = (p && !p->empty()) || !e_.lua().lua_string(name, "Name");
    passive_[w] = is;
    return is;
  }

  std::string repair_skill(int32_t uid) const {
    for (const auto& [u, s] : o_.repair_skills) {
      if (u == uid) return s;
    }
    return "Skill_Repair";
  }

  static void unique_valid(std::vector<Point>& v) {
    std::vector<Point> out;
    for (Point p : v) {
      if (p.valid() && std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
    }
    v = std::move(out);
  }

  // Actions of unit p from `base` (p moved there already on `b`).
  void actions_from(const Board& b, const Pawn& p, Point move, std::vector<UnitTurn>& out) {
    if (!can_act(b, p) || p.frozen) return;
    LuaHost& lua = e_.lua();
    for (int slot = 0; slot < kMaxWeapons; ++slot) {
      const Symbol w = p.weapons[static_cast<size_t>(slot)];
      if (w == kNoSymbol || passive(w)) continue;
      const std::string name(symbol_name(w));
      std::vector<Point> area = lua.target_area(b, p, name, p.pos);
      unique_valid(area);
      const bool two = lua.is_two_click(name);
      for (Point t : area) {
        UnitTurn u{p.uid, move, 1, slot, name, t, kInvalidPoint};
        if (two && !lua.two_click_exception(b, p, name, p.pos, t)) {
          const Point first = lua.translate_first_click(b, p, name, p.pos, t);
          std::vector<Point> second = lua.second_target_area(b, p, name, p.pos, first);
          unique_valid(second);
          for (Point t2 : second) {
            if (!target_ok(b, p.pos, t) && !target_ok(b, p.pos, t2)) continue;
            u.target2 = t2;
            out.push_back(u);
          }
        } else if (target_ok(b, p.pos, t)) {
          out.push_back(u);
        }
      }
    }
    if (p.mech && is_relevant(p.pos)) {
      out.push_back(UnitTurn{p.uid, move, 2, -1, repair_skill(p.uid), p.pos, kInvalidPoint});
    }
  }

  // Calls fn(turn, board after it) for every candidate unit turn of p on b
  // (each move run once); fn returns false to stop.
  template <class Fn>
  void unit_turns(const Board& b, const Pawn& p, Fn&& fn) {
    std::vector<UnitTurn> acts;
    auto from_base = [&](const Board& base, const Pawn& q, Point move) {
      acts.clear();
      actions_from(base, q, move, acts);
      for (const UnitTurn& t : acts) {
        Board x = base;
        if (!act(x, t)) continue;
        if (!fn(t, std::move(x))) return false;
      }
      return true;
    };
    if (!from_base(b, p, kInvalidPoint)) return;
    if (!can_move(p)) return;
    std::vector<Point> dests = e_.lua().target_area(b, p, "Move", p.pos);
    unique_valid(dests);
    const bool here_relevant = is_relevant(p.pos);
    for (Point d : dests) {
      if (d == p.pos) continue;
      Board moved = b;
      e_.lua().seed(kLuaSeed);
      if (!e_.move(moved, p.uid, d, ao_).ok()) continue;
      const Pawn* q = moved.find_pawn(p.uid);
      if (!q || !q->alive() || !q->pos.valid()) continue;
      if (here_relevant || is_relevant(d)) {
        if (!fn(UnitTurn{p.uid, d, 0, -1, "", kInvalidPoint, kInvalidPoint}, Board(moved))) return;
      }
      if (!from_base(moved, *q, d)) return;
    }
  }

  // The action part of a unit turn (its move already made on b).
  bool act(Board& b, const UnitTurn& t) {
    if (t.kind == 1) {
      e_.lua().seed(kLuaSeed);
      const ActionResult r = e_.fire_weapon(b, t.uid, t.slot, t.target,
                                            t.target2.valid() ? std::optional<Point>(t.target2) : std::nullopt, ao_);
      if (!r.ok()) return false;
    } else if (t.kind == 2) {
      e_.lua().seed(kLuaSeed);
      if (!e_.repair(b, t.uid, t.target, t.skill, ao_).ok()) return false;
    }
    return true;
  }

  // Whether the unit turn changed anything on a relevant tile (pawns there,
  // their statuses and queued shots, the tile's state).
  bool touches_relevant(const Board& before, const Board& after) const {
    for (Point p : relevant_) {
      if (!(before.tile(p) == after.tile(p))) return true;
      const Pawn* a = before.pawn_at(p);
      const Pawn* c = after.pawn_at(p);
      if (!a != !c) return true;
      if (a && !(*a == *c)) {
        // The same pawn, untouched, except for its turn bookkeeping.
        Pawn x = *a, y = *c;
        x.active = y.active;
        x.moved = y.moved;
        x.movement = y.movement;
        x.arrival = y.arrival;
        if (!(x == y)) return true;
      }
    }
    for (const Pawn& p : before.pawns()) {
      if (p.team != Team::Enemy || !p.queued.active()) continue;
      const Pawn* q = after.find_pawn(p.uid);
      if (!q || q->pos != p.pos || !(q->queued == p.queued) || q->hp != p.hp) return true;
    }
    return false;
  }

  Engine& e_;
  const TurnContext& ctx_;
  const LookaheadOptions& o_;
  ActionOptions ao_;
  std::vector<Point> relevant_;
  std::unordered_map<Symbol, bool> passive_;
};

}  // namespace

NextTurnLoss turn_loss(const Board& before, const Board& after, const TurnContext& ctx, const PhaseResult& pr) {
  const Score s = score_turn(before, after, &ctx, &pr);
  NextTurnLoss l;
  l.grid = std::max(0, -s[ScoreKey::GridLost]);
  l.building_hp = std::max(0, -s[ScoreKey::BuildingHpLost]);
  l.mechs = std::max(0, -s[ScoreKey::MechsLost]);
  return l;
}

uint32_t lookahead_seed(const LookaheadOptions& options, int k) { return deploy_sample_seed(options.seed, k); }

Board next_turn_start(Engine& engine, const Board& root, const Board& after, const PhaseResult* phase,
                      const TurnContext& ctx, uint32_t ai_seed, const LookaheadOptions& options, AiPhaseResult* ai) {
  Board b = after;
  if (options.sample_spawns && phase && !phase->emerged_unknown.empty()) {
    const std::vector<std::string> types = visible_spawn_types(engine, root);
    if (!types.empty()) {
      size_t i = static_cast<size_t>(ai_seed % 7919u);
      for (Point p : phase->emerged_unknown) {
        if (!p.valid() || b.pawn_at(p)) continue;
        const PawnDef* def = engine.data().pawn(types[i++ % types.size()]);
        if (!def) continue;
        Pawn pawn = engine.data().make_pawn(*def, b.next_uid++, p);
        Pawn& added = b.add_pawn(pawn);
        b.stamp_arrival(added);
      }
    }
  }
  AiPhaseOptions ao;
  ao.seed = ai_seed;
  ao.actions = action_options(ctx);
  // The target each enemy shot at this turn (the game's targetHistory): its
  // queued target at the start of the turn, which the player saw.
  for (const Pawn& p : root.pawns()) {
    if (p.team == Team::Enemy && p.queued.active()) ao.last_target[p.uid] = p.queued.target;
  }
  engine.lua().seed(kLuaSeed);
  AiPhaseResult r = engine.ai_phase(b, ao);
  if (ai) *ai = std::move(r);
  // The next player turn starts (deploy.cpp does the same for turn 1).
  b.turn = root.turn + 1;
  b.player_phase = true;
  for (Pawn& p : b.pawns()) {
    if (p.team != Team::Player) continue;
    start_turn_movement(b, p.uid);
    if (p.controlled() && p.alive() && p.pos.valid() && p.movement.powered) p.active = true;
  }
  return b;
}

NextTurnLoss idle_loss(Engine& engine, const Board& start, const TurnContext& ctx) {
  Board end = start;
  engine.lua().seed(kLuaSeed);
  const PhaseResult pr = engine.end_turn(end, ctx);
  return turn_loss(start, end, ctx, pr);
}

NextTurnLoss reply_loss(Engine& engine, const Board& start, const TurnContext& ctx, const LookaheadOptions& options,
                        std::vector<PlayerAction>* plan) {
  switch (options.reply) {
    case LookaheadOptions::Reply::Idle:
      return idle_loss(engine, start, ctx);
    case LookaheadOptions::Reply::Greedy: {
      Greedy g(engine, ctx, options);
      return g.run(start, plan);
    }
    case LookaheadOptions::Reply::Search: {
      SolveOptions so;
      so.time_limit_s = 0;
      so.node_limit = std::max<uint64_t>(1, options.search_nodes);
      so.beam_width = 1;
      so.repair_skills = options.repair_skills;
      const SolveResult r = solve_turn(engine, start, ctx, so);
      if (plan) *plan = r.best.actions;
      NextTurnLoss l;
      l.grid = std::max(0, -r.best.worst_case[ScoreKey::GridLost]);
      l.building_hp = std::max(0, -r.best.worst_case[ScoreKey::BuildingHpLost]);
      l.mechs = std::max(0, -r.best.worst_case[ScoreKey::MechsLost]);
      return l;
    }
  }
  return {};
}

bool no_next_turn(const Board& root, const PhaseResult* phase) {
  return (phase && phase->mission_ended) || root.turn >= root.total_turns;
}

NextTurnLoss lookahead_sample(Engine& engine, const Board& root, const Board& after, const PhaseResult* phase,
                              const TurnContext& ctx, const LookaheadOptions& options, int k) {
  NextTurnLoss l;
  if (no_next_turn(root, phase)) {
    l.mission_over = true;
    return l;
  }
  // The next turn's context: the same mission, no hidden spawn types, the
  // default outcome of every chance node (no Grid Defense resist, first
  // branch), no logs.
  TurnContext next = ctx;
  next.spawn_types.clear();
  next.events = nullptr;
  next.log = nullptr;
  next.grid_resist = nullptr;
  next.choose = nullptr;
  next.death_seed = nullptr;
  next.spider_egg = nullptr;
  const Board start = next_turn_start(engine, root, after, phase, next, lookahead_seed(options, k), options);
  return reply_loss(engine, start, next, options);
}

NextTurnLoss lookahead_loss(Engine& engine, const Board& root, const Board& after, const PhaseResult* phase,
                            const TurnContext& ctx, const LookaheadOptions& options,
                            std::vector<NextTurnLoss>* per_sample) {
  NextTurnLoss worst;
  if (no_next_turn(root, phase)) {
    worst.mission_over = true;
    return worst;
  }
  for (int k = 0; k < std::max(1, options.samples); ++k) {
    const NextTurnLoss l = lookahead_sample(engine, root, after, phase, ctx, options, k);
    if (per_sample) per_sample->push_back(l);
    if (k == 0 || worse(l, worst)) worst = l;
  }
  return worst;
}

int32_t lookahead_value(Engine& engine, const Board& root, const Board& after, const PhaseResult* phase,
                        const TurnContext& ctx, const LookaheadOptions& options,
                        std::vector<NextTurnLoss>* per_sample) {
  if (no_next_turn(root, phase)) return 0;
  int32_t acc = 0;
  for (int k = 0; k < std::max(1, options.samples); ++k) {
    const NextTurnLoss l = lookahead_sample(engine, root, after, phase, ctx, options, k);
    if (per_sample) per_sample->push_back(l);
    acc = lookahead_fold(acc, next_turn_value(l), options.sum);
  }
  return acc;
}

}  // namespace itb
