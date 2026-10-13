// The enemy AI's planning (ai_phase.hpp).

#include "itb/ai_phase.hpp"

#include <algorithm>
#include <unordered_map>

#include "itb/game_data.hpp"
#include "itb/movement.hpp"
#include "engine_impl.hpp"

namespace itb {

bool ai_plan_at_least(const AiPlan& a, const AiPlan& b) {
  // A negative position score never matches a positive one.
  if (a.position_score < 0 && b.position_score > 0) return false;
  if (b.position_score < 0 && a.position_score > 0) return true;
  if (a.target_score != b.target_score) return a.target_score > b.target_score;
  return a.position_score >= b.position_score;
}

namespace {

bool on_edge(Point p) { return p.x == 0 || p.y == 0 || p.x == kBoardSize - 1 || p.y == kBoardSize - 1; }

// a strictly better than b under AiPlan's order (SelectPlan's replacement test).
bool strictly_better(const AiPlan& a, const AiPlan& b) {
  if (b.position_score < 0 && a.position_score > 0) return true;
  if (a.target_score != b.target_score) return a.target_score > b.target_score;
  return a.position_score > b.position_score;
}

// A plan the 1-in-4 deviation accepts: off the edge, something to hit, a
// position that is not bad.
bool deviation_ok(const AiPlan& p) {
  return p.target_score > 0 && p.position_score >= 0 && !on_edge(p.loc);
}

// The C library rand() the AI draws from: srand/rand of the platform's libc.
class AiRandom {
 public:
  AiRandom(LuaHost& host, AiRng kind) : host_(host), kind_(kind) {}
  void seed(uint32_t s) {
    if (kind_ == AiRng::Glibc) {
      host_.seed(s);
    } else {
      state_ = s;
    }
  }
  int rand() {
    if (kind_ == AiRng::Glibc) return host_.rand();
    // macOS libc rand(): the Park-Miller "minimal standard" generator
    // (x = 16807 x mod 2^31-1, Schrage's method); the state is the output.
    if (state_ == 0) state_ = 123459876;
    const int64_t hi = static_cast<int64_t>(state_) / 127773;
    const int64_t lo = static_cast<int64_t>(state_) % 127773;
    int64_t x = 16807 * lo - 2836 * hi;
    if (x < 0) x += 0x7fffffff;
    state_ = static_cast<uint32_t>(x);
    return static_cast<int>(x & 0x7fffffff);
  }

 private:
  LuaHost& host_;
  AiRng kind_;
  uint32_t state_ = 1;
};

class Draws {
 public:
  Draws(AiRandom& rng, const AiPhaseOptions& opts, AiPhaseResult& out) : rng_(rng), opts_(opts), out_(out) {}

  // random(n): 0 without a draw for n == 0, else rand() % n.
  int pick(AiDraw kind, int n) {
    if (n <= 0) return 0;
    int v;
    if (opts_.draw) {
      v = std::clamp(opts_.draw(kind, n), 0, n - 1);
    } else {
      v = rng_.rand() % n;
    }
    out_.draws.push_back({kind, n, v});
    return v;
  }
  // The rand() draws of one native Pawn construction.
  void pawn_constructed() {
    constexpr int per = 3;  // measured on the live saves (1, 2 and 4 fit far worse)
    if (opts_.draw) return;
    for (int i = 0; i < per; ++i) rng_.rand();
  }
  // random_bool(4): rand() % 4 == 0.
  bool one_in_four() {
    bool v;
    if (opts_.draw) {
      v = opts_.draw(AiDraw::Deviate, 2) == 1;
    } else {
      v = rng_.rand() % 4 == 0;
    }
    out_.draws.push_back({AiDraw::Deviate, 2, v ? 1 : 0});
    return v;
  }

 private:
  AiRandom& rng_;
  const AiPhaseOptions& opts_;
  AiPhaseResult& out_;
};

// unique_rand(x): rand(), redrawn (up to 10 times) while it equals x, +2 if
// it still does.
uint32_t unique_rand(AiRandom& rng, uint32_t x) {
  int v = rng.rand();
  if (static_cast<uint32_t>(v) == x) {
    for (int i = 0; i < 10 && static_cast<uint32_t>(v) == x; ++i) v = rng.rand();
    if (static_cast<uint32_t>(v) == x) v += 2;
  }
  return static_cast<uint32_t>(v);
}

}  // namespace

AiPhaseResult Engine::ai_phase(Board& board, const AiPhaseOptions& options) {
  AiPhaseResult out;
  LuaHost& host = *impl_->host;
  AiRandom rng(host, options.rng);
  Draws draws(rng, options, out);
  uint32_t seed = options.seed;

  // Lua "Weapon" (the skill index the AI arms; skill 0 is Move) per type.
  std::unordered_map<Symbol, int> weapon_field;
  auto weapon_slot = [&](const Pawn& p) {
    auto it = weapon_field.find(p.type);
    if (it == weapon_field.end()) {
      const std::optional<double> w = host.lua_number(symbol_name(p.type), "Weapon");
      it = weapon_field.emplace(p.type, w ? static_cast<int>(*w) : 1).first;
    }
    const int slot = it->second - 1;  // engine slots leave out Move
    if (slot < 0 || slot >= kMaxWeapons || p.weapons[static_cast<size_t>(slot)] == kNoSymbol) return -1;
    return slot;
  };

  // ---- AiController::StartTurn: enemies and neutrals become active.
  // Pawn::StartTurn (BoardPlayer state 2, every non-player pawn) resets their
  // move first.
  board.player_phase = false;
  for (Pawn& p : board.pawns()) {
    if (p.team == Team::Player) continue;
    start_turn_movement(board, p.uid);
  }
  for (Pawn& p : board.pawns()) {
    if ((p.team == Team::Enemy || p.neutral) && p.movement.powered) {
      p.active = true;
    } else if (p.team == Team::Player) {
      // End Turn (Pawn::EndTurn) left the player's units inactive; a board
      // that skipped Engine::end_turn gets the same.
      p.active = false;
    }
  }

  for (int guard = 0; guard < 256; ++guard) {
    // ---- NextPawn: Board::GetActivePawn, the first active pawn in list order.
    Pawn* next = nullptr;
    for (Pawn& p : board.pawns()) {
      if (p.active) {
        next = &p;
        break;
      }
    }
    if (!next) break;
    const int32_t uid = next->uid;
    if (next->frozen || !next->alive()) {
      next->active = false;
      continue;
    }
    AiPawnResult pr;
    pr.uid = uid;
    pr.type = next->type;
    pr.from = next->pos;
    const int slot = weapon_slot(*next);
    const std::string weapon = slot >= 0 ? std::string(symbol_name(next->weapons[static_cast<size_t>(slot)])) : "";
    Point last = kInvalidPoint;
    if (auto it = options.last_target.find(uid); it != options.last_target.end()) last = it->second;

    // ---- StartState(0): reseed, plan.
    if (!options.draw) rng.seed(seed);
    const uint64_t draws_before = host.rand_draws();
    const size_t native_before = out.draws.size();
    Board scratch = board;
    Pawn* sp = scratch.find_pawn(uid);
    const Pawn saved = *sp;
    const Pathing profile = path_profile(scratch, *sp);
    const std::vector<Point> candidates = ai_move_candidates(scratch, *sp);
    std::vector<AiPlan> plans;
    bool good_inside = false;  // AiPlanner +0x20
    for (Point loc : candidates) {
      if (!loc.valid()) continue;
      if (!(can_pass(scratch, loc, profile) || loc == saved.pos)) continue;
      // ---- ScoreLocation
      if (on_edge(loc) && good_inside) continue;
      set_space(scratch, *sp, loc, /*no_injury=*/true);
      std::vector<Point> area;
      if (slot >= 0 && can_act(scratch, *sp)) {
        LuaCall c;
        area = host.target_area(scratch, *sp, weapon, loc, &c);
        if (!c.ok) out.lua_errors.push_back(c.error);
      }
      int position = 0;
      if (move_speed(scratch, *sp) != 0) {
        LuaCall c;
        position = host.score_positioning(scratch, *sp, loc, &c);
        if (!c.ok) out.lua_errors.push_back(c.error);
      }
      if (sp->injured && loc != saved.pos && sp->hp == 1) position = std::min(position, 0);
      int best = 0;
      std::vector<Point> ties;
      for (Point t : area) {
        LuaCall c;
        const uint64_t temp_before = host.temp_pawns();
        const int lua_score = host.target_score(scratch, *sp, weapon, loc, t, &c);
        // Board:GetDeployLocScore constructs a temporary Pawn per call, and
        // the constructor draws from rand().
        for (uint64_t k = host.temp_pawns() - temp_before; k > 0; --k) draws.pawn_constructed();
        if (!c.ok) out.lua_errors.push_back(c.error);
        // SkillManager::GetTargetScore: -5 for repeating the last target.
        const int base = (t == last) ? -5 : 0;
        int s = base + lua_score;
        if (lua_score > 0 && base < 0 && -base >= lua_score) s = 1;
        if (s < best) continue;
        if (s > best) ties.clear();
        best = s;
        ties.push_back(t);
      }
      *sp = saved;
      AiPlan plan;
      plan.loc = loc;
      plan.position_score = position;
      if (best > 0 && !ties.empty()) {
        plan.target = ties[static_cast<size_t>(draws.pick(AiDraw::TargetTie, static_cast<int>(ties.size())))];
        plan.target_score = best;
      }
      if (!on_edge(loc) && plan.target_score > 0 && plan.position_score >= 0) good_inside = true;
      plans.push_back(plan);
    }

    // ---- SelectPlan
    std::vector<AiPlan> best_group;
    std::vector<AiPlan> previous;
    for (const AiPlan& p : plans) {
      if (on_edge(p.loc) && good_inside) continue;
      if (p.position_score < -10) continue;
      if (best_group.empty()) {
        best_group.push_back(p);
        continue;
      }
      if (!ai_plan_at_least(p, best_group.front())) continue;
      if (strictly_better(p, best_group.front())) {
        previous = best_group;
        best_group.clear();
      }
      best_group.push_back(p);
    }
    AiPlan chosen;
    pr.best_ties = static_cast<int>(best_group.size());
    pr.previous_best = static_cast<int>(previous.size());
    if (!best_group.empty()) {
      chosen = best_group[static_cast<size_t>(draws.pick(AiDraw::PlanTie, static_cast<int>(best_group.size())))];
      if (!previous.empty() && draws.one_in_four()) {
        while (!previous.empty()) {
          const int i = draws.pick(AiDraw::DeviatePick, static_cast<int>(previous.size()));
          const AiPlan c = previous[static_cast<size_t>(i)];
          previous.erase(previous.begin() + i);
          if (deviation_ok(c)) {
            chosen = c;
            pr.deviated = true;
            break;
          }
        }
      }
    }
    {
      // Draws Lua made while the pawn planned (none in shipped scripts).
      size_t native = 0;
      if (options.rng == AiRng::Glibc && !options.draw) native = out.draws.size() - native_before;
      pr.lua_draws = static_cast<int>(host.rand_draws() - draws_before - native);
    }
    if (!options.draw) seed = unique_rand(rng, seed);
    pr.plan = chosen;
    if (options.keep_plans) pr.plans = plans;

    // ---- StartState(1): Pawn::ManualMove to the plan's tile.
    ActionResult diag;
    ResolveContext ctx;
    impl_->wire(*this, ctx, options.actions, diag);
    {
      Resolver r(board, ctx);
      if (chosen.loc.valid() && chosen.loc != board.find_pawn(uid)->pos) {
        r.apply([&]() { ai_move(board, uid, chosen.loc, &ctx.rules); });
      }
      r.settle();
    }
    const Pawn* moved = board.find_pawn(uid);
    pr.end = moved ? moved->pos : kInvalidPoint;
    // ---- StartState(2): fire at a valid target (SkillManager::FireWeapon
    // fires only when the target is in the area from where the pawn stands).
    if (moved && moved->alive() && moved->pos.valid() && chosen.target.valid() && slot >= 0) {
      LuaCall c;
      const std::vector<Point> area = host.target_area(board, *moved, weapon, moved->pos, &c);
      if (std::find(area.begin(), area.end(), chosen.target) != area.end()) {
        ActionOptions fo = options.actions;
        fo.check_legal = false;
        fo.known_area = &area;
        const ActionResult a = fire_weapon(board, uid, slot, chosen.target, std::nullopt, fo);
        for (const std::string& e : a.lua_errors) out.lua_errors.push_back(e);
        for (const LuaWrite& w : a.unapplied) out.unapplied.push_back(w);
        const Pawn* after = board.find_pawn(uid);
        pr.queued = after && after->queued.active();
      }
    }
    for (const std::string& e : diag.lua_errors) out.lua_errors.push_back(e);
    for (const LuaWrite& w : diag.unapplied) out.unapplied.push_back(w);
    // Pawn::DoAction / NextPawn: the pawn's turn is over.
    if (Pawn* p = board.find_pawn(uid)) p->active = false;
    out.pawns.push_back(std::move(pr));
  }
  out.seed = seed;
  return out;
}

}  // namespace itb
