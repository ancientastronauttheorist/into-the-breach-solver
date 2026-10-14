// Deployment search (deploy.hpp).

#include "itb/deploy.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <thread>
#include <unordered_map>
#include <utility>

#include "itb/ai_phase.hpp"
#include "itb/board_hash.hpp"
#include "itb/engine.hpp"
#include "itb/movement.hpp"

namespace itb {

namespace {

using Clock = std::chrono::steady_clock;

double since(Clock::time_point t0) { return std::chrono::duration<double>(Clock::now() - t0).count(); }

Score add(const Score& a, const Score& b) {
  Score s;
  for (size_t i = 0; i < s.v.size(); ++i) s.v[i] = a.v[i] + b.v[i];
  return s;
}

// Runs fn(engine, i) for i in [0, n), one thread per engine, until done or
// `stop` says so.
void parallel(const std::vector<Engine*>& engines, size_t n, const std::function<bool()>& stop,
              const std::function<void(Engine&, size_t)>& fn) {
  std::atomic<size_t> next{0};
  auto work = [&](Engine* e) {
    for (;;) {
      if (stop()) return;
      const size_t i = next.fetch_add(1);
      if (i >= n) return;
      fn(*e, i);
    }
  };
  if (engines.size() <= 1 || n <= 1) {
    work(engines.front());
    return;
  }
  std::vector<std::thread> pool;
  for (size_t t = 1; t < engines.size() && t < n; ++t) pool.emplace_back(work, engines[t]);
  work(engines.front());
  for (auto& t : pool) t.join();
}

bool never() { return false; }

// Finalists: solved ones first, by their value; then by the screen.
bool rank_before(const DeployCandidate& a, const DeployCandidate& b) {
  if (a.value.has_value() != b.value.has_value()) return a.value.has_value();
  if (a.value && deploy_better(*a.value, *b.value)) return true;
  if (a.value && deploy_better(*b.value, *a.value)) return false;
  return deploy_better(a.screen, b.screen);
}

}  // namespace

DeploySummary summarize(const std::vector<std::pair<Score, int>>& values) {
  DeploySummary s;
  s.mean.assign(kScoreKeys, 0.0);
  bool first = true;
  for (const auto& [v, w] : values) {
    if (first || v < s.worst) s.worst = v;
    for (size_t i = 0; i < v.v.size(); ++i) {
      if (first || v.v[i] < s.tier_min.v[i]) s.tier_min.v[i] = v.v[i];
      s.mean[i] += static_cast<double>(w) * v.v[i];
    }
    first = false;
    ++s.outcomes;
    s.seeds += w;
  }
  for (double& m : s.mean) m = s.seeds > 0 ? m / s.seeds : 0.0;
  return s;
}

bool deploy_better(const DeploySummary& a, const DeploySummary& b) {
  for (size_t i = 0; i < a.tier_min.v.size(); ++i) {
    if (a.tier_min.v[i] != b.tier_min.v[i]) return a.tier_min.v[i] > b.tier_min.v[i];
    const double am = i < a.mean.size() ? a.mean[i] : 0.0;
    const double bm = i < b.mean.size() ? b.mean[i] : 0.0;
    // Means of up to a few dozen integer outcomes: closer than this is equal.
    if (std::abs(am - bm) > 1e-9) return am > bm;
  }
  return false;
}

uint32_t deploy_sample_seed(uint32_t seed, int k) {
  uint64_t x = (static_cast<uint64_t>(seed) << 20) + static_cast<uint64_t>(k + 1) * 0x9E3779B97F4A7C15ull;
  x ^= x >> 29;
  x *= 0xBF58476D1CE4E5B9ull;
  x ^= x >> 32;
  // A valid Park-Miller state (1 .. 2^31-2).
  return static_cast<uint32_t>(x % 0x7ffffffeu) + 1;
}

std::vector<int32_t> deploy_mechs(const Board& deployed) {
  std::vector<int32_t> out;
  for (const Pawn& p : deployed.pawns()) {
    if (p.mech && p.team == Team::Player && p.alive() && p.pos.valid()) out.push_back(p.uid);
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<Point> deploy_tiles(const Board& deployed, const std::vector<Point>& zone,
                                const std::vector<int32_t>& mechs) {
  std::vector<Point> out;
  for (Point p : zone) {
    if (!p.valid() || std::find(out.begin(), out.end(), p) != out.end()) continue;
    bool taken = false;
    for (const Pawn& q : deployed.pawns()) {
      if (!q.occupies(p)) continue;
      if (std::find(mechs.begin(), mechs.end(), q.uid) != mechs.end()) continue;
      if (q.alive() || q.corpse) taken = true;
    }
    if (!taken) out.push_back(p);
  }
  return out;
}

namespace {

// A placement up to the AI: the deployed board with the mechs moved, and
// the board after the enemy action state, the mission's NextTurn and the
// spawns (no chance there before turn 1: nothing is queued yet).
struct Placed {
  Board placed;
  Board before_ai;
};

Placed place(Engine& engine, const Board& deployed, const TurnContext& ctx, const std::vector<int32_t>& mechs,
             const std::vector<Point>& tiles) {
  Placed out{deployed, {}};
  Board& b = out.placed;
  for (size_t i = 0; i < mechs.size() && i < tiles.size(); ++i) {
    Pawn* p = b.find_pawn(mechs[i]);
    if (!p) continue;
    p->pos = tiles[i];
    b.stamp_arrival(*p);
  }
  out.before_ai = b;
  engine.end_turn(out.before_ai, ctx);
  return out;
}

// The AI's planning with `ai_seed`, then the start of turn 1. The phase's
// cost is scored with `score_opts` (the turn-1 solves' ranking, so the two
// add up slot by slot: the mech exchange folds mechs into the grid slot).
Board outcome(Engine& engine, const Placed& pl, const TurnContext& ctx, uint32_t ai_seed, Score* phase_cost,
              const ScoreOptions& score_opts = {}) {
  Board b = pl.before_ai;
  AiPhaseOptions ao;
  ao.seed = ai_seed;
  ao.actions = action_options(ctx);
  engine.ai_phase(b, ao);
  // Turn 1 starts: the player's units get their turn.
  b.turn = pl.placed.turn + 1;
  b.player_phase = true;
  for (Pawn& p : b.pawns()) {
    if (p.team != Team::Player) continue;
    start_turn_movement(b, p.uid);
    if (p.controlled() && p.alive() && p.pos.valid() && p.movement.powered) p.active = true;
  }
  if (phase_cost) {
    *phase_cost = score_turn(pl.placed, b, &ctx, nullptr, score_opts);
    // End-board tiers belong to turn 1's own score (its solve or the screen).
    (*phase_cost)[ScoreKey::Position] = 0;
    (*phase_cost)[ScoreKey::Threat] = 0;
  }
  return b;
}

}  // namespace

Board deploy_outcome(Engine& engine, const Board& deployed, const TurnContext& ctx,
                     const std::vector<int32_t>& mechs, const std::vector<Point>& tiles, uint32_t ai_seed,
                     Score* phase_cost) {
  return outcome(engine, place(engine, deployed, ctx, mechs, tiles), ctx, ai_seed, phase_cost);
}

DeployResult deploy_search(Engine& engine, const Board& deployed, const TurnContext& ctx,
                           const std::vector<Point>& zone, const DeployOptions& options) {
  const auto t0 = Clock::now();
  DeployResult out;
  out.mechs = deploy_mechs(deployed);
  if (out.mechs.empty()) {
    out.error = "no mech on the board: place the mechs first (their data comes from the board)";
    return out;
  }
  out.zone = deploy_tiles(deployed, zone, out.mechs);
  const size_t m = out.mechs.size();
  if (out.zone.size() < m) {
    out.error = "the drop zone has " + std::to_string(out.zone.size()) + " free tile(s) for " + std::to_string(m) +
                " mech(s)";
    return out;
  }
  for (const auto* list : {&options.placements, &options.always}) {
    for (const auto& p : *list) {
      if (p.size() != m) {
        out.error = "a placement needs one tile per mech (" + std::to_string(m) + ")";
        return out;
      }
    }
  }
  std::vector<Engine*> engines{&engine};
  for (Engine* e : options.helper_engines) engines.push_back(e);
  const bool limited = options.time_limit_s > 0;
  const ScoreOptions score_opts = options.solve.score_options();

  // Ending turn 1 at once over the screen's AI seeds.
  auto screen_value = [&](Engine& e, const std::vector<Point>& tiles) {
    std::vector<std::pair<Score, int>> values;
    const Placed pl = place(e, deployed, ctx, out.mechs, tiles);
    for (int s = 0; s < std::max(1, options.screen_samples); ++s) {
      Score cost;
      const Board start = outcome(e, pl, ctx, deploy_sample_seed(options.seed, s), &cost, score_opts);
      Board end = start;
      const PhaseResult pr = e.end_turn(end, ctx);
      values.emplace_back(add(cost, score_turn(start, end, &ctx, &pr, score_opts)), 1);
    }
    return summarize(values);
  };

  // ---- Candidates. Given placements are all finalists. Otherwise the
  // screen ranks tile sets, and every assignment of the best sets' tiles is
  // a finalist.
  std::vector<DeployCandidate> finals;
  std::vector<char> screened_final;
  if (!options.placements.empty()) {
    for (const auto& p : options.placements) finals.push_back(DeployCandidate{p, {}, {}, false});
    screened_final.assign(finals.size(), 0);
  } else {
    std::vector<std::vector<Point>> sets;
    std::vector<size_t> pick;
    std::function<void(size_t)> rec = [&](size_t from) {
      if (pick.size() == m) {
        std::vector<Point> set;
        for (size_t i : pick) set.push_back(out.zone[i]);
        sets.push_back(std::move(set));
        return;
      }
      for (size_t i = from; i < out.zone.size(); ++i) {
        pick.push_back(i);
        rec(i + 1);
        pick.pop_back();
      }
    };
    rec(0);
    out.sets = static_cast<int>(sets.size());
    std::vector<DeploySummary> screen(sets.size());
    std::vector<char> screened(sets.size(), 0);
    std::atomic<bool> cut{false};
    // The screen may use up to half the budget.
    parallel(engines, sets.size(),
             [&]() {
               if (limited && since(t0) > 0.5 * options.time_limit_s) {
                 cut = true;
                 return true;
               }
               return false;
             },
             [&](Engine& e, size_t i) {
               screen[i] = screen_value(e, sets[i]);
               screened[i] = 1;
             });
    if (cut) {
      out.timed_out = true;
      out.warnings.push_back("the screen ran out of its share of the time budget: " +
                             std::to_string(std::count(screened.begin(), screened.end(), 1)) + " of " +
                             std::to_string(sets.size()) + " tile sets screened");
    }
    std::vector<size_t> order;
    for (size_t i = 0; i < sets.size(); ++i) {
      if (screened[i]) order.push_back(i);
    }
    std::stable_sort(order.begin(), order.end(),
                     [&](size_t a, size_t b) { return deploy_better(screen[a], screen[b]); });
    if (order.size() > static_cast<size_t>(std::max(1, options.finalists))) {
      order.resize(static_cast<size_t>(std::max(1, options.finalists)));
    }
    for (size_t i : order) {
      std::vector<size_t> perm(m);
      for (size_t k = 0; k < m; ++k) perm[k] = k;
      do {
        DeployCandidate c;
        for (size_t k = 0; k < m; ++k) c.tiles.push_back(sets[i][perm[k]]);
        c.screen = screen[i];
        finals.push_back(std::move(c));
        screened_final.push_back(1);
      } while (options.permute && std::next_permutation(perm.begin(), perm.end()));
    }
  }
  for (const auto& p : options.always) {
    if (std::any_of(finals.begin(), finals.end(), [&](const DeployCandidate& c) { return c.tiles == p; })) continue;
    finals.push_back(DeployCandidate{p, {}, {}, false});
    screened_final.push_back(0);
  }
  // Screen values for finalists that skipped the screen (tie-breaks).
  parallel(engines, finals.size(), never, [&](Engine& e, size_t i) {
    if (!screened_final[i]) finals[i].screen = screen_value(e, finals[i].tiles);
  });
  out.screen_s = since(t0);

  // ---- Outcomes: the distinct turn-1 boards of each finalist's seeds.
  struct Outcome {
    Board board;
    Score cost;
    int weight = 1;  // seeds that gave it
  };
  std::vector<std::vector<Outcome>> outs(finals.size());
  const int samples = std::max(1, options.samples);
  parallel(engines, finals.size(), never, [&](Engine& e, size_t f) {
    std::unordered_map<BoardHash, size_t, BoardHashOf> seen;
    const Placed pl = place(e, deployed, ctx, out.mechs, finals[f].tiles);
    for (int s = 0; s < samples; ++s) {
      Score cost;
      Board b = outcome(e, pl, ctx, deploy_sample_seed(options.seed, s), &cost, score_opts);
      const BoardHash h = hash_board(b, HashMode::Search);
      auto it = seen.find(h);
      if (it != seen.end()) {
        ++outs[f][it->second].weight;
        continue;
      }
      seen.emplace(h, outs[f].size());
      outs[f].push_back(Outcome{std::move(b), cost, 1});
    }
  });
  struct Task {
    size_t f, k;
  };
  std::vector<Task> tasks;
  for (size_t f = 0; f < outs.size(); ++f) {
    for (size_t k = 0; k < outs[f].size(); ++k) tasks.push_back({f, k});
  }
  std::vector<std::optional<Score>> values(tasks.size());
  std::vector<char> proven(tasks.size(), 0);
  // Each solve gets an equal share of what is left, capped by solve_time_s.
  double per_solve = options.solve_time_s;
  if (limited) {
    const double left = std::max(0.0, options.time_limit_s - since(t0));
    const double rounds = std::ceil(static_cast<double>(tasks.size()) / static_cast<double>(engines.size()));
    const double share = rounds > 0 ? 0.9 * left / rounds : left;
    per_solve = per_solve > 0 ? std::min(per_solve, share) : share;
    if (per_solve < 0.5 * options.solve_time_s) {
      out.warnings.push_back("turn-1 solves got " + std::to_string(per_solve).substr(0, 5) +
                             " s each (asked " + std::to_string(options.solve_time_s).substr(0, 5) + " s)");
    }
  }
  std::atomic<int> solves{0};
  parallel(engines, tasks.size(), [&]() { return limited && since(t0) > options.time_limit_s; },
           [&](Engine& e, size_t t) {
             const Outcome& o = outs[tasks[t].f][tasks[t].k];
             SolveOptions so = options.solve;
             so.helper_engines.clear();
             so.time_limit_s = per_solve > 0 ? per_solve : 0;
             so.node_limit = options.solve_nodes;
             so.max_time_s = 0;
             so.min_proven_tiers = 0;
             if (so.time_limit_s <= 0 && so.node_limit == 0) so.time_limit_s = 0.5;
             const SolveResult r = solve_turn(e, o.board, ctx, so);
             values[t] = add(o.cost, r.best.worst_case);
             proven[t] = r.proven_optimal;
             ++solves;
           });
  out.solves = solves;
  for (size_t f = 0; f < finals.size(); ++f) {
    std::vector<std::pair<Score, int>> vals;
    bool complete = !outs[f].empty();
    bool all_proven = complete;
    for (size_t t = 0; t < tasks.size(); ++t) {
      if (tasks[t].f != f) continue;
      if (!values[t]) {
        complete = false;
        continue;
      }
      vals.emplace_back(*values[t], outs[f][tasks[t].k].weight);
      all_proven = all_proven && proven[t];
    }
    // A finalist whose outcomes were not all solved is not comparable.
    if (complete) {
      finals[f].value = summarize(vals);
      finals[f].all_proven = all_proven;
    } else {
      out.timed_out = true;
    }
  }
  if (out.timed_out && std::any_of(finals.begin(), finals.end(), [](const DeployCandidate& c) { return !c.value; })) {
    out.warnings.push_back("the time budget ran out before every finalist's outcomes were solved");
  }
  out.finalist_s = since(t0) - out.screen_s;

  out.finalists = finals;
  std::stable_sort(out.finalists.begin(), out.finalists.end(), rank_before);
  if (!out.finalists.empty()) {
    out.best = out.finalists.front();
    if (!out.best->value) out.warnings.push_back("no finalist was fully solved: the best is by the screen only");
  }
  out.total_s = since(t0);
  out.ok = out.best.has_value();
  if (!out.ok && out.error.empty()) out.error = "no placement evaluated";
  return out;
}

}  // namespace itb
