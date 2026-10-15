// The survival cap pre-pass (survival_cap.hpp).
//
// Why the ceiling holds (README "Grid survival", certificate): up to its
// first resisted Grid Defense roll an outcome of a plan follows the plan's
// no-resist path, and each unresisted roll costs at least 1 grid power. A
// world where the turn's first M rolls are not resisted and every later one
// is (cert_roll_budget M) follows that path; odds 0 there for every plan
// means every plan's no-resist path loses the grid within M rolls, for
// every random pick and some worst-case pick, so its odds are at most
// 1 - (1 - p)^M (one of those M rolls must be resisted). With the grid at 1
// an outcome keeps the grid only on the all-resist path; odds 0 for every
// plan in the world that resists the first K rolls of each run means every
// such path has more than K rolls: odds at most p^(K + 1).
//
// The ceiling bounds the odds of every plan from the root, not the odds of
// a board deeper in the search: after a chance node in the player's own
// actions (a resisted roll, say) the turn may well keep the grid. So the
// search never uses it as a tier bound (TierBounds caps every node): it is
// a goal for the incumbent at the root (SolveOptions::survival_goal) and a
// cap on the root's reported bound, nothing else.
#include "survival_cap.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string_view>

#include "itb/core.hpp"
#include "itb/engine.hpp"

namespace itb::detail {
namespace {

constexpr size_t kOdds = static_cast<size_t>(ScoreKey::GridSurvival);

double seconds_since(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

// A Grid Charger can raise grid power: the roll argument does not hold.
bool has_grid_charger(const Board& b) {
  for (const Pawn& p : b.pawns()) {
    for (Symbol w : p.weapons) {
      if (w != kNoSymbol && symbol_name(w).starts_with("Support_KO_GridCharger")) return true;
    }
  }
  return false;
}

}  // namespace

SurvivalCap prove_survival_cap(Engine& engine, const Board& board, const TurnContext& ctx, const SolveOptions& options,
                               double budget_s, uint64_t node_budget) {
  SurvivalCap out;
  const auto t0 = std::chrono::steady_clock::now();
  const int grid = board.grid_power;
  const int pct = std::clamp(ctx.mission.grid_defense >= 0 ? ctx.mission.grid_defense : board.grid_defense, 0, 100);
  const double p = pct / 100.0;
  if (grid <= 0 || p >= 1 || has_grid_charger(board)) return out;
  out.ran = true;

  // The proofs rank by the odds alone: the other ranking options change no
  // plan's odds, only the order below them.
  SolveOptions base;
  base.grid_survival = true;
  base.repair_skills = options.repair_skills;
  base.extra_death_seeds = options.extra_death_seeds;
  base.max_chance_leaves = options.max_chance_leaves;
  base.use_tt = options.use_tt;
  base.use_bounds = options.use_bounds;
  base.order_children = options.order_children;
  base.skip_same_effect = options.skip_same_effect;
  base.tt_max_entries = options.tt_max_entries;
  base.helper_engines = options.helper_engines;
  // A quick first plan is no help: in a world being proven every plan has
  // odds 0 (and the beam's widening would hold thread 0 for 40% of it).
  base.beam_width = 0;
  // A plan with odds above 0 disproves the world: stop there.
  base.survival_goal = 1;

  bool spent = false;
  // Every plan has odds 0 in the world (m, k): proven.
  auto prove = [&](int m, int k) {
    const double left = budget_s > 0 ? budget_s - seconds_since(t0) : 0;
    if (budget_s > 0 && left < 0.05) return !(spent = true);
    if (node_budget > 0 && out.nodes >= node_budget) return !(spent = true);
    SolveOptions c = base;
    c.cert_roll_budget = m;
    c.cert_resist_first = k;
    if (budget_s > 0) {
      // The adaptive budget proving the first tier, from the start.
      c.min_proven_tiers = 1;
      c.time_limit_s = 0.01;
      c.max_time_s = std::max(0.02, left);
    } else {
      c.time_limit_s = 0;
    }
    if (node_budget > 0) c.node_limit = node_budget - out.nodes;
    const SolveResult r = solve_turn(engine, board, ctx, c);
    ++out.proofs;
    out.nodes += r.stats.nodes;
    const bool proven = r.chance_exact && r.upper_bound && r.upper_bound->v[kOdds] <= 0;
    // Neither proven nor disproven: the budget ran out.
    if (!proven && r.best.worst_case.v[kOdds] <= 0) spent = true;
    char line[96];
    std::snprintf(line, sizeof line, "%s%s=%d %s %.1fs", out.log.empty() ? "" : ", ", k > 0 ? "K" : "M", k > 0 ? k : m,
                  proven ? "proven" : spent ? "open" : "disproven", r.stats.time_s);
    out.log += line;
    return proven;
  };
  // Any plan reaches the ceiling: nothing left to tighten.
  auto settled = [&] { return out.bound < 1.0 && out.goal_units() <= 0; };

  for (int m = grid; m >= 1 && !settled(); --m) {
    if (!prove(m, 0)) break;
    out.m = m;
    out.bound = 1.0 - std::pow(1.0 - p, m);
  }
  if (grid == 1 && out.m == 1 && p > 0 && !spent) {
    for (int k = 1; k <= 8 && !settled(); ++k) {
      if (!prove(1, k)) break;
      out.k = k;
      out.bound = std::pow(p, k + 1);
    }
  }
  out.time_s = seconds_since(t0);
  return out;
}

}  // namespace itb::detail

namespace itb {

int32_t SurvivalCap::cap_units() const {
  if (bound >= 1.0) return kSurvivalOne;
  const double units = std::ceil(std::max(0.0, bound) * kSurvivalOne) + kSurvivalCapGuard;
  return static_cast<int32_t>(std::min<double>(units, kSurvivalOne));
}

int32_t SurvivalCap::goal_units() const {
  if (bound >= 1.0) return -1;
  const double units = std::floor(std::max(0.0, bound) * kSurvivalOne) - kSurvivalCapGuard;
  return static_cast<int32_t>(std::max(0.0, units));
}

}  // namespace itb
