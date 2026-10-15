// Stage 9: the perfect-turn search (solver.hpp).
//
// Values. A node is a board during the player's turn. Its value is the best
// worst-case score the player can guarantee from there:
//   V(B) = max( E(B), max over sub-actions a of C(B, a) )
//   E(B) = min over the enemy phase's chance outcomes of score_turn(root, ..)
//   C(B, a) = min over the chance outcomes o of a of V(B·a·o)
// E(B) is "end the turn now", so every prefix of a plan is a plan. Nodes are
// searched depth-first with alpha-beta: a search returns an Interval [lo, hi]
// that holds V, exact when lo == hi. The global incumbent (the best value
// already guaranteed from the root) raises every alpha.
//
// Soundness. Intervals are only ever built from (a) values the engine
// computed, (b) the min/max rules above and (c) the tier upper bounds of
// `TierBounds` (argued there). A search that runs to the end without being
// cut by the budget therefore proves that no plan beats the incumbent.
//
// Adaptive budget (SolveOptions::min_proven_tiers = k, max_time_s). Before
// the search completes, the root's interval includes the tier bound of its
// root (every unsearched child may still reach it), so the best plan is
// proven in its first k tiers as soon as its first k components equal
// those of ub(root) (or the search completed). That is the stop check at
// time_limit_s: a flag set by improve() when an incumbent reaches it. If it
// is not set, the search goes on with every alpha raised to the incumbent
// with tiers k.. set to INT32_MAX: a node or chance outcome that cannot beat
// the incumbent's first k tiers is cut, including the ones that would only
// improve later tiers. Raising alpha only adds cutoffs, and a cut always
// reports a sound interval (hi <= alpha, or the tier bound), so every
// interval stays sound. A search completed this way has a root bound whose
// first k tiers equal the incumbent's: proven in k tiers, not optimal.
#include "itb/solver.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "itb/board_hash.hpp"
#include "itb/core.hpp"
#include "itb/engine.hpp"
#include "itb/lua_host.hpp"
#include "itb/movement.hpp"
#include "itb/lookahead.hpp"
#include "itb/objectives.hpp"
#include "itb/pilot_xp.hpp"

namespace itb {
namespace {

using Clock = std::chrono::steady_clock;

// The Lua stream (math.random, random_int) is reseeded before every engine
// run, so a run is a function of the board and the chance choices alone. The
// live stream is hidden; any fixed state is one sample of it.
constexpr uint32_t kLuaSeed = 1;
constexpr int kMaxDepth = 48;
// SolveOptions::lookahead: states per beam level whose exact value (with the
// next-turn estimate) is computed to seed incumbents.
constexpr size_t kBeamExact = 3;

Score filled(int32_t v) {
  Score s;
  s.v.fill(v);
  return s;
}
const Score kLow = filled(INT32_MIN);
const Score kHigh = filled(INT32_MAX);

struct Interval {
  Score lo = kLow;
  Score hi = kHigh;
  bool exact() const { return lo == hi; }
};

// The first `tiers` components of a and b are equal.
bool same_prefix(const Score& a, const Score& b, int tiers) {
  for (int i = 0; i < tiers; ++i) {
    if (a.v[static_cast<size_t>(i)] != b.v[static_cast<size_t>(i)]) return false;
  }
  return true;
}

// s with every tier from `tiers` on at its maximum: the least score that a
// plan must beat to improve on s in its first `tiers` tiers.
Score raised(Score s, int tiers) {
  for (int i = std::max(0, tiers); i < kScoreKeys; ++i) s.v[static_cast<size_t>(i)] = INT32_MAX;
  return s;
}

double seconds_since(Clock::time_point t) {
  return std::chrono::duration<double>(Clock::now() - t).count();
}

// ---- Chance nodes ---------------------------------------------------------------
//
// Every chance hook the engine calls (TurnContext / ActionOptions) goes through
// the driver, which numbers the calls of one run in order. A run is primed
// with forced choices for its first calls; later calls take their default
// (no Grid Defense resist, branch 0, the pawn's own seed, the first egg tile).
// Since a run is deterministic given its choices, re-running with the first i
// choices of a run plus another option for call i reaches every other outcome
// of the chance tree exactly once (enumerate_chance).

enum class Hook : uint8_t { Resist, Choose, Seed, Egg };

// How the grid survival odds (SolveOptions::grid_survival) weigh a call's
// branches: a Grid Defense roll by the resist chance, the game's random
// picks evenly, everything else (hidden state, orders the recording does
// not pin down, sampled seeds) at its worst.
enum class Weigh : uint8_t { Worst, Resist, Even };

struct ChanceCall {
  Hook hook;
  int options = 1;  // branches of this call (1 = nothing to branch on)
  int chosen = 0;
  uint32_t seed = 0;     // Seed: the seed handed out
  bool matched = false;  // Seed: its death effect drew random numbers
  Weigh weigh = Weigh::Worst;
  // Resist: rolled with the grid already at 0 (ChanceDriver::set_grid), so
  // the resisted branch loses the grid too (grid power never rises).
  bool dead = false;
};

class ChanceDriver {
 public:
  ChanceDriver(int grid_defense, int extra_seeds)
      : resist_(grid_defense > 0), seeds_(1 + std::max(0, extra_seeds)) {}

  void begin(const std::vector<int>& forced) {
    forced_ = &forced;
    calls_.clear();
    rolls_ = 0;
    grid_ = grid_start_;
  }
  // The grid power at the start of the runs that follow, to mark the rolls
  // made once it is 0 (ChanceCall::dead); -1: unknown (none is marked). Only
  // sound where grid power never rises (TierBounds::grid_monotone): every
  // grid loss is then an unresisted roll's amount.
  void set_grid(int grid) { grid_start_ = grid; }
  // SolveOptions::cert_roll_budget / cert_resist_first: from here on (and in
  // every run that follows) the first `first` Grid Defense rolls are
  // resisted, the next `budget` are not and every later one is, with no
  // branch (budget < 0: off, the rolls branch as usual).
  void set_roll_budget(int budget, int first) {
    budget_ = budget;
    first_ = first;
    rolls_ = 0;
  }
  const std::vector<ChanceCall>& calls() const { return calls_; }
  bool sampled() const { return sampled_; }

  // A death effect drew random numbers (ChanceKind::LuaRandom, amount = the
  // seed): the seed was a sample, so its call branches over the seed sample.
  void mark_random(const std::vector<ChanceRecord>& chances) {
    for (const ChanceRecord& c : chances) {
      if (c.kind != ChanceKind::LuaRandom) continue;
      for (ChanceCall& call : calls_) {
        if (call.hook == Hook::Seed && !call.matched && call.seed == static_cast<uint32_t>(c.amount)) {
          call.matched = true;
          call.options = seeds_;
          sampled_ = true;
          break;
        }
      }
    }
  }

  void bind(ActionOptions& o) {
    o.grid_resist = [this](Point, int amount) { return resist(amount); };
    o.choose = [this](const ChanceRecord& node) {
      return next(Hook::Choose, std::max(1, node.options), weigh_of(node.kind));
    };
    o.death_seed = [this](const Pawn& p) { return seed_for(p); };
    o.spider_egg = [this](Resolver&, const Pawn&, const std::vector<Point>& tiles) {
      return next(Hook::Egg, static_cast<int>(tiles.size()), Weigh::Even);
    };
  }
  void bind(TurnContext& tc) {
    tc.grid_resist = [this](Point, int amount) { return resist(amount); };
    tc.choose = [this](const ChanceRecord& node) {
      return next(Hook::Choose, std::max(1, node.options), weigh_of(node.kind));
    };
    tc.death_seed = [this](const Pawn& p) { return seed_for(p); };
    tc.spider_egg = [this](Resolver&, const Pawn&, const std::vector<Point>& tiles) {
      return next(Hook::Egg, static_cast<int>(tiles.size()), Weigh::Even);
    };
  }

 private:
  // A mission hook's random pick (the Final Cave's bomb drop, the
  // Reactivation thaw: random_removal over a list) is uniform in game.
  // Environment orders and choices are also hidden state the recording
  // lacks, and an XP split only decides level-ups: worst case.
  static Weigh weigh_of(ChanceKind k) { return k == ChanceKind::MissionRandom ? Weigh::Even : Weigh::Worst; }
  bool resist(int amount) {
    if (budget_ >= 0) {
      next(Hook::Resist, 1, Weigh::Resist);
      const int i = rolls_++;
      return i < first_ || i - first_ >= budget_;
    }
    const bool resisted = next(Hook::Resist, resist_ ? 2 : 1, Weigh::Resist) == 1;
    if (grid_start_ >= 0) {
      calls_.back().dead = grid_ <= 0;
      if (!resisted) grid_ = std::max(0, grid_ - amount);
    }
    return resisted;
  }
  int next(Hook h, int options, Weigh w) {
    const size_t i = calls_.size();
    int c = forced_ && i < forced_->size() ? (*forced_)[i] : 0;
    c = std::clamp(c, 0, std::max(0, options - 1));
    calls_.push_back(ChanceCall{h, std::max(1, options), c, 0, false, w});
    return c;
  }
  uint32_t seed_for(const Pawn& p) {
    const size_t i = calls_.size();
    const int k = forced_ && i < forced_->size() ? (*forced_)[i] : 0;
    // k = 0: the engine's default seed (uid + 1); others: fixed samples.
    const uint32_t seed = k == 0 ? static_cast<uint32_t>(p.uid + 1)
                                 : static_cast<uint32_t>(p.uid + 1) * 2654435761u + 40503u * static_cast<uint32_t>(k);
    calls_.push_back(ChanceCall{Hook::Seed, 1, k, seed, false, Weigh::Worst});
    return seed;
  }

  bool resist_;
  int seeds_;
  const std::vector<int>* forced_ = nullptr;
  std::vector<ChanceCall> calls_;
  bool sampled_ = false;
  int budget_ = -1;  // set_roll_budget
  int first_ = 0;    // set_roll_budget
  int rolls_ = 0;    // Grid Defense rolls of this run
  int grid_start_ = -1;  // set_grid
  int grid_ = -1;        // the grid power now (set_grid)
};

enum class EnumEnd { Complete, Stopped, Truncated };

// Runs `run` once per leaf of the chance tree (the default outcome first).
// `run` returns false to stop. `extra` counts the runs after the first.
template <class Run>
EnumEnd enumerate_chance(ChanceDriver& d, int max_leaves, uint64_t& extra, Run&& run) {
  std::vector<std::vector<int>> todo(1);
  int leaves = 0;
  while (!todo.empty()) {
    const std::vector<int> prefix = std::move(todo.back());
    todo.pop_back();
    d.begin(prefix);
    if (leaves++ > 0) ++extra;
    if (!run()) return EnumEnd::Stopped;
    const std::vector<ChanceCall>& calls = d.calls();
    for (size_t i = prefix.size(); i < calls.size(); ++i) {
      for (int j = 0; j < calls[i].options; ++j) {
        if (j == calls[i].chosen) continue;
        std::vector<int> p;
        p.reserve(i + 1);
        for (size_t k = 0; k < i; ++k) p.push_back(calls[k].chosen);
        p.push_back(j);
        todo.push_back(std::move(p));
      }
    }
    if (leaves >= max_leaves && !todo.empty()) return EnumEnd::Truncated;
  }
  return EnumEnd::Complete;
}

// ---- Grid survival odds ------------------------------------------------------------
//
// SolveOptions::grid_survival (README "Grid survival"). GridSurvival is the
// probability that the grid is still above 0 at the end of the turn, in
// kSurvivalOne units. score_turn gives one outcome 1 or 0; a chance node
// folds its outcomes' values over its chance tree (the leaves of
// enumerate_chance, each with the calls of its run): a Grid Defense roll is
// (1 - p) x (not resisted) + p x (resisted), a game's random pick the mean
// of its branches, any other call its worst branch. Branches no leaf reached
// count as `unexplored` (1: an upper bound on the odds, before every leaf is
// known). Every weight is non-negative and the fold is monotone in each
// leaf, so bounds on the leaves bound the odds.
//
// The odds are rounded to kSurvivalOne units with a guard: exactly
// kSurvivalOne when every leaf has kSurvivalOne, at most kSurvivalOne - 1
// otherwise. So a node that never loses the grid has odds 1 and beats every
// node that can lose it, whatever the rounding (a lost leaf behind ten
// rolls may weigh less than one unit). The guarded value is monotone too.
//
// The rest of a chance node's value is the worst (lexicographic) of its
// outcomes' values in its class: the outcomes with odds above 0 when the
// node's odds are above 0 (given the grid survives, how good is the turn;
// an outcome that surely loses it does not count), else all of them (as
// without the odds).

struct ChanceLeaf {
  std::vector<ChanceCall> path;  // the run's chance calls, in order
  int idx = 0;                   // which value the leaf reaches
};

class ChanceTree {
 public:
  ChanceTree() = default;
  // order_ points into leaves_: moves keep the buffer, copies would not.
  ChanceTree(ChanceTree&&) = default;
  ChanceTree& operator=(ChanceTree&&) = default;
  ChanceTree(const ChanceTree&) = delete;
  ChanceTree& operator=(const ChanceTree&) = delete;

  void add(ChanceLeaf l) {
    leaves_.push_back(std::move(l));
    sorted_ = false;
  }
  // The guarded odds (kSurvivalOne units) for leaf values `vals` (by
  // ChanceLeaf::idx, kSurvivalOne units).
  int32_t survival(double p, const std::vector<int32_t>& vals, double unexplored = 1.0) {
    bool lost = false;
    for (const ChanceLeaf& l : leaves_) lost = lost || vals[static_cast<size_t>(l.idx)] < kSurvivalOne;
    if (!lost) return kSurvivalOne;
    sort();
    const double odds = fold(0, order_.size(), 0, p, unexplored, vals);
    const double x = std::clamp(odds, 0.0, 1.0) * kSurvivalOne;
    return static_cast<int32_t>(std::min<long long>(std::llround(x), kSurvivalOne - 1));
  }

 private:
  void sort() {
    if (sorted_) return;
    order_.clear();
    for (const ChanceLeaf& l : leaves_) order_.push_back(&l);
    std::sort(order_.begin(), order_.end(), [](const ChanceLeaf* a, const ChanceLeaf* b) {
      const size_t n = std::min(a->path.size(), b->path.size());
      for (size_t i = 0; i < n; ++i) {
        if (a->path[i].chosen != b->path[i].chosen) return a->path[i].chosen < b->path[i].chosen;
      }
      return a->path.size() < b->path.size();
    });
    sorted_ = true;
  }
  // Leaves [lo, hi) of order_ share their first d calls.
  double fold(size_t lo, size_t hi, size_t d, double p, double unexplored, const std::vector<int32_t>& vals) const {
    // Leaves whose path ends here sort first. Normally such a leaf is alone;
    // anything else is folded at its worst.
    size_t k = lo;
    double end = 2.0;
    while (k < hi && order_[k]->path.size() <= d) {
      end = std::min(end, static_cast<double>(vals[static_cast<size_t>(order_[k++]->idx)]) / kSurvivalOne);
    }
    if (k == hi) return end;
    const ChanceCall& c = order_[k]->path[d];
    double sum = 0, worst = 2.0, first = unexplored, second = unexplored;
    const int n = std::max(1, c.options);
    size_t i = k;
    for (int j = 0; j < n; ++j) {
      size_t e = i;
      while (e < hi && order_[e]->path[d].chosen == j) ++e;
      // A roll made with the grid at 0: resisting it keeps nothing.
      const double none = c.dead && j == 1 ? 0.0 : unexplored;
      const double x = e > i ? fold(i, e, d + 1, p, unexplored, vals) : none;
      i = e;
      if (j == 0) first = x;
      if (j == 1) second = x;
      sum += x;
      worst = std::min(worst, x);
    }
    double out = worst;
    if (n == 1) {
      out = first;
    } else if (c.weigh == Weigh::Resist && n == 2) {
      out = (1 - p) * first + p * second;  // branch 1 = resisted
    } else if (c.weigh == Weigh::Even) {
      out = sum / n;
    }
    return std::min(end, out);
  }

  std::vector<ChanceLeaf> leaves_;
  std::vector<const ChanceLeaf*> order_;
  bool sorted_ = false;
};

int32_t clamp_odds(int32_t s) { return std::clamp(s, 0, kSurvivalOne); }

// The Grid Defense chance the odds use: the game's, when the state carries
// it (MissionData::grid_defense), else the board's.
double resist_chance(const Board& root, const TurnContext& ctx) {
  const int pct = ctx.mission.grid_defense >= 0 ? ctx.mission.grid_defense : root.grid_defense;
  return std::clamp(pct, 0, 100) / 100.0;
}

// SolveOptions::cert_roll_budget: the unresisted rolls left to a run that
// starts on `b` (-1: off). Every unresisted roll so far cost at least 1 grid
// power, so at most root - b.grid_power rolls were made before it: this
// budget never lets the turn's unresisted rolls exceed cert_roll_budget,
// and once a run used it up the next run's budget is 0.
int cert_roll_budget(const SolveOptions& o, const Board& root, const Board& b) {
  if (o.cert_roll_budget < 0) return -1;
  return std::max(0, o.cert_roll_budget - (root.grid_power - b.grid_power));
}

constexpr size_t kSurvival = static_cast<size_t>(ScoreKey::GridSurvival);

Score with_odds(Score s, int32_t odds) {
  s.v[kSurvival] = odds;
  return s;
}

// A chance node's value from its outcomes' intervals (grid survival): the
// odds folded over `tree` (outcome k = ChanceLeaf::idx k), the rest from the
// worst outcome of the node's class (above). Sound for any sound intervals:
// an outcome's odds lie in [lo, hi] clamped to [0, 1] (lexicographic bounds
// bound the first component), and a bound's rest only bounds the outcome's
// rest when the bound has the outcome's odds (then the outcome's odds are
// pinned).
Interval combine_odds(ChanceTree& tree, double p, const std::vector<Interval>& r) {
  const size_t n = r.size();
  std::vector<int32_t> slo(n), shi(n);
  bool pinned = true;
  for (size_t k = 0; k < n; ++k) {
    slo[k] = clamp_odds(r[k].lo.v[kSurvival]);
    shi[k] = clamp_odds(r[k].hi.v[kSurvival]);
    pinned = pinned && slo[k] == shi[k];
  }
  const int32_t lo_odds = tree.survival(p, slo), hi_odds = tree.survival(p, shi);
  if (pinned) {
    // The node's odds are known: its rest is the worst in its class.
    Score lo = kHigh, hi = kHigh;
    for (size_t k = 0; k < n; ++k) {
      if (lo_odds > 0 && slo[k] <= 0) continue;
      lo = std::min(lo, r[k].lo.v[kSurvival] == slo[k] ? r[k].lo : with_odds(kLow, slo[k]));
      hi = std::min(hi, r[k].hi.v[kSurvival] == shi[k] ? r[k].hi : with_odds(kHigh, shi[k]));
    }
    return Interval{with_odds(lo, lo_odds), with_odds(hi, hi_odds)};
  }
  Interval out{with_odds(kLow, lo_odds), with_odds(kHigh, hi_odds)};
  if (hi_odds >= kSurvivalOne) {
    // Odds 1 means every outcome has odds 1: all of them count.
    Score hi = kHigh;
    for (size_t k = 0; k < n; ++k) {
      if (r[k].hi.v[kSurvival] == kSurvivalOne) hi = std::min(hi, r[k].hi);
    }
    out.hi = with_odds(hi, kSurvivalOne);
  } else if (hi_odds <= 0) {
    // Odds 0: all of them count, the worst has odds 0; an outcome whose
    // bound has odds 0 bounds it.
    Score hi = kHigh;
    for (size_t k = 0; k < n; ++k) {
      if (r[k].hi.v[kSurvival] == 0) hi = std::min(hi, r[k].hi);
    }
    out.hi = with_odds(hi, 0);
  }
  return out;
}

// ---- Sub-actions -----------------------------------------------------------------

struct SubAction {
  enum Kind : uint8_t { Move, Weapon, Repair };
  int32_t uid = -1;
  Kind kind = Move;
  int8_t slot = -1;
  bool chance = false;  // more than one chance outcome
  Symbol skill = kNoSymbol;  // weapon or repair skill
  Point target = kInvalidPoint;
  Point target2 = kInvalidPoint;
};

ActionResult run_sub(Engine& e, Board& b, const SubAction& a, const ActionOptions& o) {
  switch (a.kind) {
    case SubAction::Move:
      return e.move(b, a.uid, a.target, o);
    case SubAction::Weapon:
      return e.fire_weapon(b, a.uid, a.slot, a.target,
                           a.target2.valid() ? std::optional<Point>(a.target2) : std::nullopt, o);
    case SubAction::Repair:
      return e.repair(b, a.uid, a.target, symbol_name(a.skill), o);
  }
  return {};
}

// Consecutive move + action of one unit become one PlayerAction.
std::vector<PlayerAction> to_player_actions(const std::vector<SubAction>& line) {
  std::vector<PlayerAction> out;
  for (const SubAction& a : line) {
    if (a.kind != SubAction::Move && !out.empty()) {
      PlayerAction& last = out.back();
      if (last.uid == a.uid && last.kind == PlayerAction::Kind::None && last.move.valid()) {
        last.kind = a.kind == SubAction::Weapon ? PlayerAction::Kind::Weapon : PlayerAction::Kind::Repair;
        last.weapon = std::string(symbol_name(a.skill));
        last.target = a.target;
        if (a.target2.valid()) last.target2 = a.target2;
        continue;
      }
    }
    PlayerAction p;
    p.uid = a.uid;
    if (a.kind == SubAction::Move) {
      p.move = a.target;
    } else {
      p.kind = a.kind == SubAction::Weapon ? PlayerAction::Kind::Weapon : PlayerAction::Kind::Repair;
      p.weapon = std::string(symbol_name(a.skill));
      p.target = a.target;
      if (a.target2.valid()) p.target2 = a.target2;
    }
    out.push_back(std::move(p));
  }
  return out;
}

// As score.cpp counts them.
int building_hp(const Board& b) {
  int total = 0;
  for (int i = 0; i < kTileCount; ++i) {
    const Tile& t = b.tile(Point::from_index(i));
    if (t.is_building()) total += std::max<int>(0, t.hp);
  }
  return total;
}

bool scored_enemy(const Pawn& p) { return p.team == Team::Enemy && !p.neutral && !p.mech; }

// ---- Tier upper bounds ------------------------------------------------------------
//
// ub(B) is a Score that no plan continuing from board B can beat: it is
// componentwise >= the score of every such plan, hence lexicographically >=.
// Per component of score_turn (score.cpp):
//   GridLost       = after.grid - root.grid. The engine raises grid power only
//                    through Lua Game:ModifyPowerGrid (Engine::apply_writes),
//                    which no shipped script calls with a gain except
//                    Support_KO_GridCharger's kill script. Without such a
//                    weapon on the board grid power never rises, so after.grid
//                    <= B.grid. With one, after.grid <= grid_power_max.
//   BuildingHpLost = building HP after - at the root. Building HP only rises
//                    in add_building (SpaceDamage iTerrain = TERRAIN_BUILDING),
//                    which no weapon, pawn or death script produces and the
//                    native environments never request (only mission_final's
//                    setup scripts do, which the engine does not run). So the
//                    total never rises: <= hp(B) - hp(root).
//   MechsLost      = sum over the root's mechs of alive_after - alive_before:
//                    <= the number of mechs dead at the root (revived corpses).
//   MechHpLost     = sum of hp_after - hp_before: a pawn's HP never exceeds
//                    its max HP (every heal clamps), and a mech's max HP only
//                    rises by the Soldier psion's +1, which reaches mechs only
//                    with Psion Leech, and by a pilot's Health / Skilled
//                    level-up (+2 each, pilot_hp_room). So <= sum of
//                    (max_hp [+1] + level-up room - hp_before).
//   ObjectivesFailed = -(stars lost) <= 0.
//   NextTurn       = next_turn_value of the lookahead's loss estimate <= 0
//                    (a loss is never negative), 0 when the lookahead is off.
//   Threat         = -(enemies able to hit a building next turn) <= 0 (0
//                    whenever the threat tier is off). No tighter cap is
//                    sound cheaply: any enemy may still be killed, pushed
//                    away, frozen or smoked, and spawns may be blocked. In
//                    the kills experiment (threat_kills) it is VekKilled, or
//                    VekKilled - 100 * threats with both flags: <= the
//                    enemies alive at the root either way. With
//                    objectives_first, plus kObjectivesFirstWeight times
//                    the objective progress, whose cap
//                    (objective_progress_cap, objectives.cpp) bounds each
//                    objective line by what the root board allows; the
//                    ObjectiveProgress component gets that cap too.
//   VekKilled      <= enemies alive at the root (only those are counted).
//   VekHpRemoved   <= their total HP (hp_before - hp_after, hp_after >= 0).
//   ObjectiveProgress, Position: unbounded (INT32_MAX) unless the caller
//                    gives SolveOptions::tier_caps.
// With the mech exchange (SolveOptions::mech_grid, rate r >= 0 from the
// root, constant over the search) the GridLost slot is 2 * GridLost +
// (2r + 1) * MechsLost - kGridOutPenalty [grid ends at 0]. Both
// coefficients are positive, so 2 * (the grid cap above) + (2r + 1) * (the
// mechs cap) bounds it, and with grid monotone a board at grid <= 0 ends
// the turn at grid <= 0, so its penalty is certain and comes off its bound.
// With per-mech rates (mech_grid_pilot) the mechs part is the sum of
// (2 r_m + 1) * delta_m over the root's mechs; only a mech dead at the root
// can have delta_m > 0 (revived), so the sum over those of (2 r_m + 1)
// bounds it.
// GridSurvival (grid_survival; else 0) is odds <= 1, and 0 on a board
// whose grid is already 0: grid power never rises (above), so every leaf
// below it loses the grid.
// tier_caps lowers any component further (the caller vouches for them).
class TierBounds {
 public:
  TierBounds(const Board& root, const std::optional<Score>& caps, const SolveOptions& opt = {},
             const TurnContext* ctx = nullptr) {
    const bool threat_kills = opt.threat_kills;
    root_grid_ = root.grid_power;
    root_building_ = building_hp(root);
    for (const Pawn& p : root.pawns()) {
      for (Symbol w : p.weapons) {
        if (w != kNoSymbol && symbol_name(w).starts_with("Support_KO_GridCharger")) grid_monotone_ = false;
      }
    }
    caps_ = kHigh;
    caps_[ScoreKey::GridLost] = std::max(root.grid_power_max, root.grid_power) - root.grid_power;
    caps_[ScoreKey::BuildingHpLost] = 0;
    const int leech = root.has_passive(kPassivePsionLeech) ? 1 : 0;
    const ScoreOptions so = opt.score_options();
    int dead_mechs = 0, dead_weight = 0, mech_room = 0, enemies = 0, enemy_hp = 0;
    for (const Pawn& p : root.pawns()) {
      const int hp0 = p.alive() && !p.fallen && p.pos.valid() ? p.hp : 0;
      if (p.mech && p.team == Team::Player) {
        if (hp0 <= 0) {
          ++dead_mechs;
          if (so.mech_exchange()) dead_weight += 2 * mech_rate(root, p, so) + 1;
        }
        mech_room += std::max(0, p.max_hp + leech + pilot_hp_room(p) - hp0);
      } else if (scored_enemy(p) && hp0 > 0) {
        ++enemies;
        enemy_hp += hp0;
      }
    }
    caps_[ScoreKey::MechsLost] = dead_mechs;
    caps_[ScoreKey::MechHpLost] = mech_room;
    caps_[ScoreKey::ObjectivesFailed] = 0;
    caps_[ScoreKey::NextTurn] = 0;
    caps_[ScoreKey::Threat] = 0;
    caps_[ScoreKey::VekKilled] = enemies;
    if (threat_kills) caps_[ScoreKey::Threat] = enemies;
    if (opt.objectives_first && opt.tier_on()) {
      progress_cap_ = objective_progress_cap(root, ctx);
      caps_[ScoreKey::ObjectiveProgress] = progress_cap_;
      const int64_t slot = int64_t{kObjectivesFirstWeight} * progress_cap_ + caps_[ScoreKey::Threat];
      caps_[ScoreKey::Threat] = static_cast<int32_t>(std::min<int64_t>(slot, INT32_MAX - 1));
    }
    caps_[ScoreKey::VekHpRemoved] = enemy_hp;
    survival_ = opt.grid_survival;
    caps_[ScoreKey::GridSurvival] = survival_ ? kSurvivalOne : 0;
    grid_cap_ = caps_[ScoreKey::GridLost];
    mech_rate_ = mech_grid_rate(root, so);
    per_mech_ = so.mech_exchange() && so.mech_grid_pilot;
    dead_weight_ = dead_weight;
    if (per_mech_) {
      caps_[ScoreKey::GridLost] = 2 * grid_cap_ + dead_weight_;
    } else if (mech_rate_ >= 0) {
      caps_[ScoreKey::GridLost] = 2 * grid_cap_ + (2 * mech_rate_ + 1) * dead_mechs;
    }
    if (caps) {
      for (size_t i = 0; i < caps_.v.size(); ++i) caps_.v[i] = std::min(caps_.v[i], caps->v[i]);
    }
  }

  Score global() const { return caps_; }
  // Grid power never rises on this board (no Grid Charger).
  bool grid_monotone() const { return grid_monotone_; }
  // objective_progress_cap at the root (objectives_first only, else -1).
  int progress_cap() const { return progress_cap_; }

  Score of(const Board& b) const {
    Score s = caps_;
    if (grid_monotone_ && per_mech_) {
      const int grid = std::min(grid_cap_, b.grid_power - root_grid_);
      s[ScoreKey::GridLost] = std::min(s[ScoreKey::GridLost], mech_grid_slot_w(grid, dead_weight_, b.grid_power));
    } else if (grid_monotone_ && mech_rate_ >= 0) {
      const int grid = std::min(grid_cap_, b.grid_power - root_grid_);
      const int32_t slot = mech_grid_slot(mech_rate_, grid, s[ScoreKey::MechsLost], b.grid_power);
      s[ScoreKey::GridLost] = std::min(s[ScoreKey::GridLost], slot);
    } else if (grid_monotone_) {
      s[ScoreKey::GridLost] = std::min(s[ScoreKey::GridLost], b.grid_power - root_grid_);
    }
    s[ScoreKey::BuildingHpLost] = std::min(s[ScoreKey::BuildingHpLost], building_hp(b) - root_building_);
    if (survival_ && grid_monotone_ && b.grid_power <= 0) s[ScoreKey::GridSurvival] = 0;
    return s;
  }

 private:
  int root_grid_ = 0;
  int root_building_ = 0;
  bool grid_monotone_ = true;
  int progress_cap_ = -1;
  int grid_cap_ = 0;   // the GridLost cap in grid units
  int mech_rate_ = -1; // mech_grid_rate at the root (-1: off)
  bool per_mech_ = false;  // mech_grid_pilot: per-mech rates
  int dead_weight_ = 0;    // sum of 2 r_m + 1 over the root's dead mechs
  bool survival_ = false;  // grid_survival
  Score caps_;
};

// ---- The search -------------------------------------------------------------------

struct Outcome {
  Board board;
  BoardHash hash;
};

struct Child {
  SubAction act;
  std::vector<Outcome> outcomes;  // the default outcome first
  Score order;                    // best case of ending the turn right after
  // grid_survival, more than one outcome: the action's chance tree, each
  // leaf reaching outcomes[idx].
  std::shared_ptr<ChanceTree> tree;
};

// An end-of-turn value. `stopped`: the enemy phase was ended early by the
// bound (TurnContext::stop_early), so `v` is [-inf, the bound there]: sound,
// but a poor key for ordering children or ranking the beam.
struct LeafMemo {
  Interval v;
  bool stopped = false;
  // grid_survival: the worst outcome so far, each with its own odds (1 or
  // 0): the order key the search had before the odds (child ordering under
  // an alpha that surely keeps the grid ranks by it).
  Score key = kHigh;
};

struct TTEntry {
  Interval v;
  SubAction best;
  bool has_best = false;
};

// Two sound intervals for one value: their intersection is sound too.
Interval intersect(const Interval& a, const Interval& b) {
  const Interval r{std::max(a.lo, b.lo), std::min(a.hi, b.hi)};
  return r.lo <= r.hi ? r : b;  // only a truncated chance tree can disagree
}
LeafMemo merge_leaf(const LeafMemo& old, const LeafMemo& next) {
  // A real cut value is never above a stopped one of the same board (the
  // default outcome runs first), so the merge is stopped only if both are.
  return LeafMemo{intersect(old.v, next.v), old.stopped && next.stopped, std::min(old.key, next.key)};
}
TTEntry merge_entries(const TTEntry& old, const TTEntry& next) {
  TTEntry out = next.v.lo >= old.v.lo ? next : old;
  out.v = intersect(old.v, next.v);
  return out;
}

// A hash map in independently locked shards, shared by the search threads.
// Entries are only ever sound intervals, so any thread may use any entry,
// and dropping one only loses information. Once `cap` entries are stored, a
// new entry replaces the deepest of the few entries in the neighbouring
// buckets of its shard (entries near the root stand for the largest
// subtrees); below the cap nothing is ever dropped.
template <class V>
class SharedMap {
 public:
  explicit SharedMap(size_t cap) : cap_(cap) {}
  bool get(const BoardHash& k, V& out) {
    Shard& s = shard(k);
    std::lock_guard lock(s.mu);
    auto it = s.map.find(k);
    if (it == s.map.end()) return false;
    out = it->second.v;
    return true;
  }
  // An existing entry is merged with merge(old, new). `depth`: how deep in
  // the search the entry was computed (the replacement priority).
  template <class Merge>
  void put(const BoardHash& k, const V& v, Merge&& merge, int depth = 0) {
    Shard& s = shard(k);
    std::lock_guard lock(s.mu);
    auto it = s.map.find(k);
    if (it != s.map.end()) {
      it->second.v = merge(it->second.v, v);
      it->second.depth = std::min(it->second.depth, depth);
      return;
    }
    if (size_.load(std::memory_order_relaxed) >= cap_) {
      if (!evict(s, k)) return;
    } else {
      size_.fetch_add(1, std::memory_order_relaxed);
    }
    s.map.emplace(k, Slot{v, depth});
  }
  size_t size() const { return size_.load(std::memory_order_relaxed); }
  // In-progress counts (V = int): boards some thread is searching now.
  void add(const BoardHash& k, int delta) {
    Shard& s = shard(k);
    std::lock_guard lock(s.mu);
    if ((s.map[k].v += delta) == 0) s.map.erase(k);
  }

 private:
  static constexpr size_t kShards = 64;
  static constexpr size_t kProbe = 8;  // buckets looked at for a victim
  struct Slot {
    V v{};
    int depth = 0;
  };
  struct Shard {
    std::mutex mu;
    std::unordered_map<BoardHash, Slot, BoardHashOf> map;
  };
  // Removes the deepest entry among those in the buckets after k's own.
  static bool evict(Shard& s, const BoardHash& k) {
    const size_t buckets = s.map.bucket_count();
    if (buckets == 0 || s.map.empty()) return false;
    const size_t first = s.map.bucket(k);
    BoardHash victim{};
    int deepest = -1;
    for (size_t i = 0; i < kProbe; ++i) {
      const size_t b = (first + i) % buckets;
      for (auto it = s.map.begin(b); it != s.map.end(b); ++it) {
        if (it->second.depth > deepest) {
          deepest = it->second.depth;
          victim = it->first;
        }
      }
    }
    if (deepest < 0) return false;
    s.map.erase(victim);
    return true;
  }
  Shard& shard(const BoardHash& k) { return shards_[k.hi % kShards]; }
  std::array<Shard, kShards> shards_;
  std::atomic<size_t> size_{0};
  size_t cap_;
};

// What the search threads share.
struct Shared {
  Shared(const Board& r, const TurnContext& ctx, const SolveOptions& opt)
      : root(r), o(opt), bounds(r, opt.tier_caps, opt, &ctx), tt(opt.tt_max_entries), leaf(opt.tt_max_entries), busy(SIZE_MAX),
        next_turn(opt.lookahead ? opt.tt_max_entries : 0) {
    base = ctx;
    base.events = nullptr;
    base.log = nullptr;
    adaptive = opt.min_proven_tiers > 0 && opt.time_limit_s > 0 && opt.max_time_s > opt.time_limit_s;
    tiers = components_for_tiers(std::clamp(opt.min_proven_tiers, 0, used_tier_count(opt.tiers())), opt.tiers());
    score_options = opt.score_options();
    root_ub = bounds.of(r);
    resist_p = resist_chance(r, ctx);
  }
  const Board& root;
  const SolveOptions& o;
  TierBounds bounds;
  TurnContext base;
  SharedMap<TTEntry> tt;
  SharedMap<LeafMemo> leaf;
  SharedMap<int> busy;
  // SolveOptions::lookahead: NextTurn values by the hash of the end board
  // (after the enemy phase), shared by the threads.
  SharedMap<std::vector<int32_t>> next_turn;
  // The lookahead search's prior: the tables of the search without the
  // lookahead that ran first (solve_turn). Their upper bounds hold here: a
  // plan's score with the lookahead is its score without it with NextTurn
  // lowered from its cap (0), so every value here is at most the value
  // there, which is at most its bound. (Their lower bounds do not hold.)
  const Shared* prior = nullptr;
  size_t threads = 1;
  Clock::time_point start;
  std::atomic<bool> stop{false};       // budget spent or the search completed
  std::atomic<bool> completed{false};  // some thread finished the root search
  std::atomic<uint64_t> nodes{0};
  std::atomic<bool> chance_exact{true};

  // Adaptive budget (solver.cpp header).
  bool adaptive = false;
  int tiers = 0;                          // ScoreKey components holding min_proven_tiers
  ScoreOptions score_options;
  Score root_ub;                          // bounds.of(root)
  double resist_p = 0;                    // grid_survival: the Grid Defense chance
  // Grid survival's survey (search_survival): every alpha is at least
  // `floor` (odds 1), so only lines that surely keep the grid are searched;
  // with no such line found by `survey_s` seconds or `survey_nodes` nodes
  // the search stops (0: no such limit).
  bool survey = false;
  Score floor = kLow;
  double survey_s = 0;
  uint64_t survey_nodes = 0;
  std::atomic<bool> tiers_proven{false};  // the incumbent meets root_ub in the first `tiers`
  std::atomic<bool> prefix_mode{false};   // past time_limit_s, proving the first `tiers` only

  std::mutex mu;  // the incumbent and the warnings
  bool has_incumbent = false;
  Score incumbent = kLow;
  std::vector<SubAction> best_line;
  // A seeded incumbent (SolveOptions::lookahead, LookaheadOptions::
  // base_share): its plan, used while no line has improved on it.
  bool seeded = false;
  std::vector<PlayerAction> seeded_actions;
  bool seeded_contingent = false;
  double first_plan_s = 0, best_plan_s = 0;
  std::vector<std::string> warnings;

  // Bumped (under `mu`) whenever the incumbent improves, so threads can keep
  // a copy and only lock when it changed.
  std::atomic<uint64_t> version{0};
  bool incumbent_value(Score& out) {
    std::lock_guard lock(mu);
    out = incumbent;
    return has_incumbent;
  }
  void note(const std::string& w) {
    std::lock_guard lock(mu);
    if (std::find(warnings.begin(), warnings.end(), w) == warnings.end()) warnings.push_back(w);
  }
};

// One search thread: its own engine (Lua state), chance driver and path.
class Worker {
 public:
  Worker(Engine& engine, Shared& shared, size_t index)
      : e_(engine), sh_(shared), o_(shared.o), index_(index), driver_(shared.root.grid_defense, shared.o.extra_death_seeds) {
    tc_ = shared.base;
    driver_.bind(tc_);
    aopts_ = action_options(tc_);  // the mission's per-frame hooks (points into tc_)
    driver_.bind(aopts_);
    aopts_.check_legal = true;
    for (const auto& [uid, skill] : o_.repair_skills) repair_[uid] = intern(skill);
    move_skill_ = intern("Move");
    default_repair_ = intern("Skill_Repair");
    auto env_on = [](const char* name, bool dflt) {
      const char* v = std::getenv(name);
      return v && *v ? std::string_view(v) != "0" : dflt;
    };
    verify_effects_ = env_on("ITB_VERIFY_EFFECTS", false);
    same_effect_ = o_.skip_same_effect && env_on("ITB_SAME_EFFECT", true);
    skip_spent_ = o_.skip_spent_turns && env_on("ITB_SKIP_SPENT", true);
  }

  // Beam pre-pass (thread 0), then the full search. Returns the root's
  // interval; complete() says whether the search ran to its end.
  Interval run() {
    if (index_ == 0 && o_.beam_width > 0) {
      // With helper threads searching meanwhile, thread 0 widens the beam
      // (x3 per round) for up to 40% of the time limit: wider beams find
      // plans whose first unit turns look bad on their own.
      size_t width = static_cast<size_t>(o_.beam_width);
      // With the adaptive budget the search may run on to max_time_s (it
      // stops at the base time only once its first tiers are proven), so the
      // widening may too.
      const double budget = sh_.adaptive ? o_.max_time_s : o_.time_limit_s;
      const double until = budget > 0 ? 0.4 * budget : 0;
      beam(width, until);
      while (sh_.threads > 1 && until > 0 && width < 1000 && !budget_exceeded() &&
             seconds_since(sh_.start) < until) {
        width *= 3;
        beam(width, until);
      }
    }
    const BoardHash h = hash_board(sh_.root, HashMode::Search);
    std::vector<SubAction> pv;
    const Interval r = search(sh_.root, h, kLow, kHigh, true, &pv, 0);
    if (!aborted_) {
      sh_.completed = true;
      sh_.stop = true;
    }
    return r;
  }
  bool complete() const { return !aborted_; }
  const SolveStats& stats() const { return stats_; }
  bool sampled() const { return driver_.sampled(); }

 private:
  bool budget_exceeded() {
    if (aborted_) return true;
    bool spent = false;
    if (o_.node_limit && sh_.nodes.load(std::memory_order_relaxed) >= o_.node_limit) spent = true;
    if (sh_.survey && sh_.version.load(std::memory_order_relaxed) == 0) {
      if (sh_.survey_nodes && sh_.nodes.load(std::memory_order_relaxed) >= sh_.survey_nodes) spent = true;
      if (sh_.survey_s > 0 && seconds_since(sh_.start) >= sh_.survey_s) spent = true;
    }
    if (o_.time_limit_s > 0) {
      const double t = seconds_since(sh_.start);
      if (t >= o_.time_limit_s) {
        if (!sh_.adaptive || t >= o_.max_time_s || sh_.tiers_proven.load(std::memory_order_relaxed)) {
          spent = true;
        } else if (!sh_.prefix_mode.load(std::memory_order_relaxed)) {
          sh_.prefix_mode.store(true);
        }
      }
    }
    if (spent) sh_.stop = true;
    if (sh_.stop.load(std::memory_order_relaxed)) aborted_ = true;
    return aborted_;
  }

  void improve(const Score& v, const std::vector<SubAction>& tail) {
    std::lock_guard lock(sh_.mu);
    if (sh_.has_incumbent && v <= sh_.incumbent) return;
    if (!sh_.has_incumbent) sh_.first_plan_s = seconds_since(sh_.start);
    sh_.has_incumbent = true;
    sh_.incumbent = v;
    sh_.seeded = false;
    sh_.best_line = path_;
    sh_.best_line.insert(sh_.best_line.end(), tail.begin(), tail.end());
    sh_.best_plan_s = seconds_since(sh_.start);
    if (sh_.adaptive && same_prefix(v, sh_.root_ub, sh_.tiers)) sh_.tiers_proven = true;
    sh_.version.fetch_add(1, std::memory_order_release);
  }

  // The incumbent, from this thread's copy unless it changed.
  bool incumbent(Score& out) {
    const uint64_t v = sh_.version.load(std::memory_order_acquire);
    if (v != seen_version_) {
      has_incumbent_ = sh_.incumbent_value(incumbent_);
      seen_version_ = v;
    }
    out = incumbent_;
    return has_incumbent_;
  }

  Score alpha_of(const Score& alpha) {
    Score inc;
    if (!incumbent(inc)) return sh_.survey ? std::max(alpha, sh_.floor) : alpha;
    if (sh_.prefix_mode.load(std::memory_order_relaxed)) inc = raised(inc, sh_.tiers);
    return std::max(alpha, inc);
  }

  // The NextTurn tier of one chance outcome of ending the turn: `s` is its
  // score with NextTurn at the cap (0), `after` the board after the enemy
  // phase `pr` (lookahead.hpp). The value is the worst of the samples'
  // next_turn_value; the samples run one at a time, and as soon as the worst
  // so far puts `s` at or below `alpha` the rest are skipped (`cut`: then the
  // returned value is only an upper bound). Sample values are cached by the
  // end board (with what the phase result adds: unknown spawns, mission end),
  // so a later call resumes where an earlier one stopped.
  int32_t next_turn_value_of(const Score& s, const Board& after, const PhaseResult& pr, const Score& alpha,
                             bool& cut) {
    cut = false;
    BoardHash key = hash_board(after, HashMode::Search);
    uint64_t extra = pr.mission_ended ? 0x9E3779B97F4A7C15ull : 0;
    for (Point p : pr.emerged_unknown) extra = extra * 1099511628211ull + static_cast<uint64_t>(p.index() + 1);
    key.lo ^= extra;
    std::vector<int32_t> vals;
    const bool had = sh_.next_turn.get(key, vals);
    const size_t known = vals.size();
    const int samples = std::max(1, o_.lookahead_options.samples);
    int32_t worst = 0;
    Score t = s;
    for (int k = 0; k < samples; ++k) {
      if (static_cast<size_t>(k) < vals.size()) {
        ++stats_.lookahead_hits;
      } else if (k > 0 && budget_exceeded()) {
        cut = true;  // `worst` so far is still an upper bound
        break;
      } else {
        const Clock::time_point t0 = Clock::now();
        const NextTurnLoss l = lookahead_sample(e_, sh_.root, after, &pr, tc_, o_.lookahead_options, k);
        stats_.lookahead_s += seconds_since(t0);
        ++stats_.lookahead_evals;
        vals.push_back(next_turn_value(l));
        if (l.mission_over) {
          vals.resize(static_cast<size_t>(samples), 0);  // no next turn: every sample is 0
        }
      }
      worst = lookahead_fold(worst, vals[static_cast<size_t>(k)], o_.lookahead_options.sum);
      t[ScoreKey::NextTurn] = worst;
      if (k + 1 < samples && t <= alpha) {
        cut = true;
        break;
      }
    }
    if (!had || vals.size() > known) {
      sh_.next_turn.put(key, vals, [](const std::vector<int32_t>& a, const std::vector<int32_t>& b) {
        return b.size() >= a.size() ? b : a;
      });
    }
    return worst;
  }

  // E(B): the worst case of ending the turn on B. With the lookahead, each
  // chance outcome is first scored with NextTurn at its cap (0), an upper
  // bound; the lookahead then runs only for outcomes that can still decide
  // the minimum, and not at all when the bound is already <= alpha or the
  // caller only wants bounds (`lazy`: child ordering, the beam's ranking),
  // in which case the interval is [NextTurn at its floor, at its cap].
  // `may_stop`: the enemy phase may end early once the outcome is cut (its
  // value is then only the bound; ranking callers want the real one).
  //
  // Grid survival (SolveOptions::grid_survival): each outcome's GridSurvival
  // says whether the grid survived it; the leaf's are the odds over all of
  // them and its rest the worst outcome's of its class (above). Until every
  // outcome is known the odds are only bounded (upper), so the cut and the
  // early stop of the phase differ while alpha may lose the grid. `key`
  // receives LeafMemo::key.
  Interval leaf(const Board& b, const Score& alpha, bool lazy = false, bool may_stop = true, Score* key = nullptr) {
    const bool la = o_.lookahead;
    const bool surv = o_.grid_survival;
    lazy = lazy && la;
    BoardHash hkey{};
    if (o_.use_tt) {
      hkey = hash_board(b, HashMode::EndTurn);
      LeafMemo memo;
      if (sh_.leaf.get(hkey, memo) && (may_stop || !memo.stopped) &&
          (memo.v.exact() || memo.v.hi <= alpha || lazy)) {
        ++stats_.leaf_hits;
        if (key) *key = memo.key;
        return memo.v;
      }
    }
    if (o_.use_tt && sh_.prior && may_stop) {
      LeafMemo memo;
      if (const_cast<Shared*>(sh_.prior)->leaf.get(hkey, memo) && memo.v.hi <= alpha) {
        ++stats_.prior_hits;
        if (key) *key = memo.key;
        return Interval{kLow, memo.v.hi};
      }
    }
    bool stopped_early = false;
    ++stats_.leaf_evals;
    struct Pending {
      Score s;  // NextTurn at its cap
      Board after;
      PhaseResult pr;
    };
    std::vector<Pending> pending;
    Score worst = kHigh;  // every outcome so far
    Score alive = kHigh;  // grid_survival: the outcomes so far that kept the grid
    ChanceTree tree;      // grid_survival: every outcome so far, and their odds
    std::vector<int32_t> vals;
    bool cut = false;
    // Grid survival: the phase may only stop early, and one outcome settle
    // the cut, while alpha surely keeps the grid (odds 1): then an outcome
    // that loses it puts this leaf's odds below alpha's at once, as it put
    // GridLost below alpha's without the odds. Under a lower alpha every
    // outcome's share of the odds counts.
    const bool sure = !surv || alpha.v[kSurvival] >= kSurvivalOne;    // An upper bound on this leaf's value from the outcomes so far. Odds:
    // every outcome not yet run counted as surviving. Rest: if every outcome
    // so far kept the grid, the worst of them (odds 1 needs every outcome to
    // keep it); odds 0, the worst of them; odds above 0, the worst outcome
    // that kept the grid, if any so far.
    size_t checked = 0;
    auto upper = [&]() -> Score {
      if (!surv || worst.v[kSurvival] >= kSurvivalOne) return worst;
      if (sure) return with_odds(kHigh, kSurvivalOne - 1);
      const int32_t odds = tree.survival(sh_.resist_p, vals);
      if (odds <= 0) return with_odds(worst, 0);
      return with_odds(alive.v[kSurvival] >= kSurvivalOne ? alive : kHigh, odds);
    };
    driver_.set_roll_budget(cert_roll_budget(o_, sh_.root, b), o_.cert_resist_first);
    driver_.set_grid(sh_.bounds.grid_monotone() ? b.grid_power : -1);
    const EnumEnd end = enumerate_chance(driver_, o_.max_chance_leaves, stats_.chance_branches, [&] {
      Board after = b;
      const Clock::time_point t0 = Clock::now();
      e_.lua().seed(kLuaSeed);
      // Stop the phase once the board's tier bound is <= alpha: grid power
      // and building HP never rise (TierBounds), so the rest of the phase
      // cannot lift this outcome above it.
      if (o_.use_bounds && may_stop && sure) {
        tc_.stop_early = [&](const Board& mid) { return sh_.bounds.of(mid) <= alpha; };
      }
      const PhaseResult pr = e_.end_turn(after, tc_);
      tc_.stop_early = nullptr;
      stats_.enemy_phase_s += seconds_since(t0);
      ++stats_.enemy_phases;
      driver_.mark_random(pr.chances);
      if (pr.stopped) {
        ++stats_.phases_stopped;
        stopped_early = true;
        const Score bound = sh_.bounds.of(after);
        worst = std::min(worst, bound);
        if (surv) {
          tree.add(ChanceLeaf{driver_.calls(), static_cast<int>(vals.size())});
          vals.push_back(bound.v[kSurvival]);
        }
        cut = true;
        return false;
      }
      if (!pr.exact) sh_.note("enemy phase: the recorded data does not pin every step down (EnvInexact)");
      if (!pr.emerged_unknown.empty()) sh_.note("enemy phase: spawns of unknown type emerge without a pawn");
      const Score s = score_turn(sh_.root, after, &tc_, &pr, sh_.score_options);
      if (sh_.bounds.progress_cap() >= 0 && s[ScoreKey::ObjectiveProgress] > sh_.bounds.progress_cap()) {
        sh_.note("objective progress above its derived cap (objective_progress_cap is unsound here)");
      }
      worst = std::min(worst, s);
      if (surv) {
        if (s.v[kSurvival] >= kSurvivalOne) alive = std::min(alive, s);
        tree.add(ChanceLeaf{driver_.calls(), static_cast<int>(vals.size())});
        vals.push_back(s.v[kSurvival]);
        // Folding the odds sorts the outcomes so far: under an alpha that
        // may lose the grid, try the cut after 1, 2, 4, 8, ... outcomes.
        const bool cheap = worst.v[kSurvival] >= kSurvivalOne || sure;
        if (cheap || vals.size() >= 2 * checked) {
          if (!cheap) checked = vals.size();
          if (upper() <= alpha) {
            cut = true;
            return false;
          }
        }
      } else if (worst <= alpha) {
        cut = true;
        return false;
      }
      if (la) pending.push_back(Pending{s, std::move(after), pr});
      return true;
    });
    if (end == EnumEnd::Truncated) sh_.chance_exact = false;
    if (cut) ++stats_.chance_cutoffs;
    Interval r = cut ? Interval{kLow, worst} : Interval{worst, worst};
    // Grid survival, every outcome known (a truncated tree counts those it
    // did not reach as surviving; chance_exact is off then): the odds, and
    // the class whose worst outcome gives the rest.
    int32_t odds = kSurvivalOne;
    bool in_class_alive = false;
    if (surv) {
      if (cut) {
        r.hi = upper();
      } else {
        odds = tree.survival(sh_.resist_p, vals);
        in_class_alive = odds > 0;
        const Score& cls = in_class_alive ? alive : worst;
        r = Interval{with_odds(cls, odds), with_odds(cls, odds)};
      }
    }
    if (la && !cut) {
      const int32_t floor = lookahead_floor(o_.lookahead_options);
      auto floor_of = [floor](Score s) {
        s[ScoreKey::NextTurn] = floor;
        return s;
      };
      // Grid survival: only the outcomes of the class count.
      auto outside = [&](const Pending& p) { return in_class_alive && p.s.v[kSurvival] < kSurvivalOne; };
      if (lazy) {
        Score lo = kHigh;
        for (const Pending& p : pending) {
          if (!outside(p)) lo = std::min(lo, floor_of(p.s));
        }
        r = Interval{lo, worst};
        if (surv) r = Interval{with_odds(lo, odds), with_odds(in_class_alive ? alive : worst, odds)};
      } else {
        // Outcomes in increasing order of their bound; one whose lowest
        // possible value cannot go below the minimum so far is skipped.
        std::stable_sort(pending.begin(), pending.end(),
                         [](const Pending& x, const Pending& y) { return x.s < y.s; });
        // Grid survival: the odds are known. The rest decides against alpha
        // only when the odds equal alpha's, so the class's outcomes are
        // compared with alpha at their own odds; odds below alpha's: cut
        // now; above: no cut.
        Score a = alpha;
        bool done = false;
        if (surv) {
          const Score whole = with_odds(in_class_alive ? alive : worst, odds);  // NextTurn at its cap
          if (whole <= alpha) {
            r = Interval{kLow, whole};
            cut = done = true;
          } else if (odds == alpha.v[kSurvival]) {
            a.v[kSurvival] = in_class_alive ? kSurvivalOne : worst.v[kSurvival];
          } else {
            a = kLow;
          }
        }
        Score exact = kHigh;
        for (const Pending& p : pending) {
          if (done) break;
          if (outside(p) || floor_of(p.s) >= exact) continue;
          Score s = p.s;
          bool stopped = false;
          s[ScoreKey::NextTurn] = next_turn_value_of(p.s, p.after, p.pr, a, stopped);
          exact = std::min(exact, s);
          if (stopped || exact <= a) {
            cut = true;
            break;
          }
        }
        if (cut) ++stats_.chance_cutoffs;
        if (!done) {
          r = cut ? Interval{kLow, exact} : Interval{exact, exact};
          if (surv) {
            r.hi.v[kSurvival] = odds;
            if (!cut) r.lo.v[kSurvival] = odds;
          }
        }
      }
    }
    if (o_.use_tt) sh_.leaf.put(hkey, LeafMemo{r, stopped_early, worst}, merge_leaf);
    if (key) *key = worst;
    return r;
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

  bool two_click(Symbol w) {
    auto it = two_click_.find(w);
    if (it != two_click_.end()) return it->second;
    const bool is = e_.lua().is_two_click(symbol_name(w));
    two_click_[w] = is;
    return is;
  }

  static void valid_unique(std::vector<Point>& pts) {
    std::vector<Point> out;
    for (Point p : pts) {
      if (p.valid() && std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
    }
    pts = std::move(out);
  }

  // Runs one sub-action from b through every chance outcome. False if the
  // engine refuses it or nothing happens. `area`: the skill's target area on
  // b, when unit_actions computed it cleanly (ActionOptions::known_area);
  // `effect`: the shot's pure effect (ActionOptions::known_effect).
  // `tree` (grid survival): receives the chance tree, each leaf reaching its
  // outcome's index in `outs`.
  bool execute(const Board& b, SubAction& act, std::vector<Outcome>& outs,
               const std::vector<Point>* area = nullptr, const SkillEffect* effect = nullptr,
               ChanceTree* tree = nullptr) {
    outs.clear();
    bool refused = false;
    aopts_.known_area = area;
    aopts_.known_effect = effect;
    driver_.set_roll_budget(cert_roll_budget(o_, sh_.root, b), o_.cert_resist_first);
    driver_.set_grid(sh_.bounds.grid_monotone() ? b.grid_power : -1);
    const EnumEnd end = enumerate_chance(driver_, o_.max_chance_leaves, stats_.chance_branches, [&] {
      Board next = b;
      const Clock::time_point t0 = Clock::now();
      e_.lua().seed(kLuaSeed);
      const ActionResult r = run_sub(e_, next, act, aopts_);
      stats_.sub_action_s += seconds_since(t0);
      ++stats_.sub_actions;
      if (!r.ok()) {
        refused = true;
        return false;
      }
      driver_.mark_random(r.resolve.chances);
      const BoardHash h = hash_board(next, HashMode::Search);
      size_t k = 0;
      while (k < outs.size() && outs[k].hash != h) ++k;
      if (k == outs.size()) outs.push_back(Outcome{std::move(next), h});
      if (tree) tree->add(ChanceLeaf{driver_.calls(), static_cast<int>(k)});
      return true;
    });
    aopts_.known_area = nullptr;
    aopts_.known_effect = nullptr;
    if (refused) return false;
    if (end == EnumEnd::Truncated) sh_.chance_exact = false;
    act.chance = outs.size() > 1;
    return true;
  }

  // Sub-actions to try, each with the index of its skill's target area in
  // `areas` (-1: none known).
  struct Actions {
    std::vector<SubAction> acts;
    std::vector<int32_t> area;
    std::vector<std::vector<Point>> areas;
    void clear() {
      acts.clear();
      area.clear();
      areas.clear();
    }
    const std::vector<Point>* area_of(size_t i) const {
      return area[i] >= 0 ? &areas[static_cast<size_t>(area[i])] : nullptr;
    }
  };

  // LuaHost::target_area, kept in `out.areas` when the call can stand in for
  // the engine's own legality check (no Lua error, no random numbers drawn).
  // Returns the area and its index (-1 if it cannot be reused).
  std::pair<std::vector<Point>, int32_t> target_area(const Board& b, const Pawn& p, std::string_view skill,
                                                      Actions& out) {
    LuaHost& lua = e_.lua();
    LuaCall call;
    const uint64_t draws = lua.rand_draws();
    std::vector<Point> area = lua.target_area(b, p, skill, p.pos, &call);
    if (!call.ok || lua.rand_draws() != draws) return {std::move(area), -1};
    out.areas.push_back(area);
    return {std::move(area), static_cast<int32_t>(out.areas.size() - 1)};
  }

  // The sub-actions unit p may try on b (not yet run): weapons and repair if
  // `acts`, moves if `moves`.
  void unit_actions(const Board& b, const Pawn& p, bool acts, bool moves, Actions& out) {
    LuaHost& lua = e_.lua();
    const int32_t uid = p.uid;
    const Point at = p.pos;
    if (acts && can_act(b, p) && !p.frozen) {
      for (int slot = 0; slot < kMaxWeapons; ++slot) {
        const Symbol w = p.weapons[static_cast<size_t>(slot)];
        if (w == kNoSymbol || passive(w)) continue;
        const std::string name(symbol_name(w));
        auto [targets, area] = target_area(b, p, name, out);
        valid_unique(targets);
        const bool two = two_click(w);
        for (Point t : targets) {
          SubAction a;
          a.uid = uid;
          a.kind = SubAction::Weapon;
          a.slot = static_cast<int8_t>(slot);
          a.skill = w;
          a.target = t;
          if (two && !lua.two_click_exception(b, p, name, at, t)) {
            const Point first = lua.translate_first_click(b, p, name, at, t);
            std::vector<Point> second = lua.second_target_area(b, p, name, at, first);
            valid_unique(second);
            for (Point t2 : second) {
              a.target2 = t2;
              out.acts.push_back(a);
              out.area.push_back(area);
            }
          } else {
            out.acts.push_back(a);
            out.area.push_back(area);
          }
        }
      }
      if (p.mech) {
        auto it = repair_.find(uid);
        const Symbol skill = it != repair_.end() ? it->second : default_repair_;
        auto [targets, area] = target_area(b, p, symbol_name(skill), out);
        valid_unique(targets);
        if (targets.empty()) targets.push_back(at);
        for (Point t : targets) {
          SubAction a;
          a.uid = uid;
          a.kind = SubAction::Repair;
          a.skill = skill;
          a.target = t;
          out.acts.push_back(a);
          out.area.push_back(area);
        }
      }
    }
    if (moves && can_move(p)) {
      auto [dests, area] = target_area(b, p, "Move", out);
      valid_unique(dests);
      for (Point d : dests) {
        if (d == at) continue;
        SubAction a;
        a.uid = uid;
        a.kind = SubAction::Move;
        a.skill = move_skill_;
        a.target = d;
        out.acts.push_back(a);
        out.area.push_back(area);
      }
    }
  }

  static bool unit_can_play(const Pawn& p) { return p.controlled() && p.alive() && p.pos.valid(); }

  // Same-effect shots. A weapon's target only reaches the board through
  // the effect it builds (Engine::pure_weapon_effect): two targets of one
  // unit's weapon slot whose pure effects are equal (SkillEffect::target
  // aside, which a prepared effect never reads; no queued part) give the
  // same outcomes, so the second needs no run. Every field of the effect
  // takes part (SpaceDamage::operator== is defaulted).
  static bool same_effect(const SkillEffect& a, const SkillEffect& b) {
    static_assert(sizeof(SkillEffect) == 72, "SkillEffect changed: compare the new field in same_effect");
    return a.effect == b.effect && a.q_effect == b.q_effect && a.origin == b.origin && a.owner == b.owner &&
           a.team == b.team && a.follow_up == b.follow_up;
  }

  void generate(const Board& b, const BoardHash& h, std::vector<Child>& out) {
    out.clear();
    std::unordered_set<BoardHash, BoardHashOf> seen;
    seen.insert(h);
    Actions acts;
    for (const Pawn& p : b.pawns()) {
      if (unit_can_play(p)) unit_actions(b, p, true, true, acts);
    }
    // Shots already run from b with a pure effect: where their outcomes went
    // (`child`: index in `out`, or npos when they duplicated a sibling).
    struct Fired {
      int32_t uid;
      int8_t slot;
      SkillEffect fx;
      size_t child;
    };
    std::vector<Fired> fired;
    for (size_t i = 0; i < acts.acts.size(); ++i) {
      SubAction& act = acts.acts[i];
      std::optional<SkillEffect> fx;
      if (same_effect_ && act.kind == SubAction::Weapon && !act.target2.valid()) {
        aopts_.known_area = acts.area_of(i);
        fx = e_.pure_weapon_effect(b, act.uid, act.slot, act.target, aopts_);
        aopts_.known_area = nullptr;
        if (fx && !fx->q_effect.empty()) fx.reset();  // the queued shot keeps the target
      }
      if (fx) {
        const Fired* twin = nullptr;
        for (const Fired& f : fired) {
          if (f.uid == act.uid && f.slot == act.slot && same_effect(f.fx, *fx)) {
            twin = &f;
            break;
          }
        }
        if (twin) {
          ++stats_.same_effect_shots;
          if (verify_effects_) {
            verify_twin(b, act, acts.area_of(i), twin->child == SIZE_MAX ? nullptr : &out[twin->child], seen);
          }
          if (twin->child == SIZE_MAX) {
            ++stats_.duplicate_children;  // as running it would have found
            continue;
          }
          Child c;
          c.act = act;
          c.act.chance = out[twin->child].act.chance;
          c.outcomes = out[twin->child].outcomes;
          c.tree = out[twin->child].tree;
          if (c.outcomes.size() == 1) {  // (a single outcome is in `seen`: never reached)
            ++stats_.duplicate_children;
            continue;
          }
          out.push_back(std::move(c));
          continue;
        }
      }
      Child c;
      ChanceTree tree;
      if (!execute(b, act, c.outcomes, acts.area_of(i), fx ? &*fx : nullptr, o_.grid_survival ? &tree : nullptr)) {
        continue;
      }
      if (o_.grid_survival && c.outcomes.size() > 1) c.tree = std::make_shared<ChanceTree>(std::move(tree));
      if (verify_effects_ && fx) {
        // The shot run with its known effect must match a run that builds it.
        SubAction a = act;
        std::vector<Outcome> outs;
        bool same = execute(b, a, outs, acts.area_of(i)) && outs.size() == c.outcomes.size();
        for (size_t k = 0; same && k < outs.size(); ++k) same = outs[k].hash == c.outcomes[k].hash;
        if (!same) {
          ++stats_.same_effect_mismatches;
          sh_.note("known-effect shot gave another board: " + std::string(symbol_name(act.skill)));
        }
      }
      if (c.outcomes.size() == 1 && !seen.insert(c.outcomes[0].hash).second) {
        ++stats_.duplicate_children;
        if (fx) fired.push_back(Fired{act.uid, act.slot, std::move(*fx), SIZE_MAX});
        continue;
      }
      if (skip_spent_ && c.outcomes.size() == 1 && spent_turn_only(b, c.outcomes[0].board, act.uid)) {
        ++stats_.spent_turns;
        if (fx) fired.push_back(Fired{act.uid, act.slot, std::move(*fx), SIZE_MAX});
        continue;
      }
      c.act = act;
      if (fx) fired.push_back(Fired{act.uid, act.slot, std::move(*fx), out.size()});
      out.push_back(std::move(c));
    }
  }

  // Spent turns. A weapon or Repair of unit A whose one outcome `c` is the
  // board `b` it was fired from, except that A is no longer active (and the
  // undo flags, which nothing the search runs reads), is not a child: a plan
  // through it is matched by the same plan with that sub-action left out.
  //
  // Soundness. Let X and Y differ only in A.active (X true, Y false) and the
  // undo flags. Then V(Y) <= V(X), by induction over the sub-actions left:
  //   - ending the turn: Engine::end_turn first sets every player unit's
  //     active false and clears every undo flag (enemy_phase.cpp), so both
  //     end boards are equal, and with them every score tier;
  //   - Y's sub-actions are other units' (A, inactive, can neither move nor
  //     act: can_move / can_act); each is legal from X and has the same
  //     outcomes there, because A.active and the undo flags are read only by
  //     A's own legality checks (can_move, can_act), by Lua Pawn:IsActive and
  //     Pawn:IsUndoPossible (no shipped script calls IsActive; IsUndoPossible
  //     only in the tutorial) and by the undo machinery the search never
  //     uses. Each outcome pair differs again only in those flags (or is
  //     equal, if the sub-action set A.active), so the induction applies, and
  //     the chance folds (minimum, the grid survival odds) are monotone.
  // A plan from b that takes such a sub-action is therefore worth no more
  // than the plan without it, whose sub-actions are all children of b's
  // other children: dropping the child leaves V(b) unchanged. (The plan
  // without it never passes through a skipped child: removing every such
  // sub-action from a plan gives a plan with none.) Only exact equality
  // counts: a spent Boost, a used limited charge or a moved flag is a change.
  bool spent_turn_only(const Board& b, const Board& c, int32_t uid) {
    const Pawn* pb = b.find_pawn(uid);
    const Pawn* pc = c.find_pawn(uid);
    if (!pb || !pc || !pb->active || pc->active || b.pawns().size() != c.pawns().size()) return false;
    Board& x = spent_scratch_;
    x = c;
    for (size_t i = 0; i < x.pawns().size(); ++i) {
      Pawn& p = x.pawns()[i];
      const Pawn& q = b.pawns()[i];
      if (p.uid != q.uid) return false;
      p.movement.undo_ready = q.movement.undo_ready;
      if (p.uid == uid) p.active = true;
    }
    return x == b;
  }

  // ITB_VERIFY_EFFECTS=1: a same-effect shot is run anyway and must give
  // its twin's outcomes (a mismatch is a warning and is counted).
  void verify_twin(const Board& b, const SubAction& act, const std::vector<Point>* area, const Child* twin,
                   const std::unordered_set<BoardHash, BoardHashOf>& seen) {
    SubAction a = act;
    std::vector<Outcome> outs;
    const bool ok = execute(b, a, outs, area);
    bool same = ok;
    if (ok && twin) {
      same = outs.size() == twin->outcomes.size();
      for (size_t k = 0; same && k < outs.size(); ++k) same = outs[k].hash == twin->outcomes[k].hash;
    } else if (ok) {
      same = outs.size() == 1 && seen.count(outs[0].hash) > 0;  // its twin's board was a sibling's
    }
    if (!same) {
      ++stats_.same_effect_mismatches;
      sh_.note("same-effect shot gave another board: " + std::string(symbol_name(act.skill)));
    }
  }

  // A quick first plan: a beam over whole unit turns (an optional move, then
  // an optional action), ranked by the value of ending the turn there. To
  // keep plans whose value needs the other units' turns, the beam keeps the
  // best `beam_width` states for every set of units that have played. It
  // only finds incumbents; the search proper proves them.
  void beam(size_t width, double until) {
    struct State {
      Board board;
      std::vector<SubAction> line;
      bool chance_free = true;
      uint32_t played = 0;  // bit i: units[i] has had its turn
      Score key;
    };
    std::vector<int32_t> units;
    for (const Pawn& p : sh_.root.pawns()) {
      if (unit_can_play(p) && units.size() < 31) units.push_back(p.uid);
    }
    std::vector<State> beam(1);
    beam[0].board = sh_.root;
    std::unordered_set<BoardHash, BoardHashOf> seen;
    seen.insert(hash_board(sh_.root, HashMode::Search));
    std::vector<Outcome> outs;
    for (size_t level = 0; level < units.size() && !beam.empty(); ++level) {
      std::vector<State> next;
      auto consider = [&](Outcome& o, std::vector<SubAction> line, bool chance_free, uint32_t played) {
        if (!seen.insert(o.hash).second) return;
        const Score a = alpha_of(kLow);
        Score old_key = kHigh;
        const Interval end = leaf(o.board, a, o_.lookahead, false, &old_key);
        if (chance_free && end.exact()) improve(end.lo, line);
        // Grid survival: the beam ranks by the old key (LeafMemo::key: the
        // worst outcome, each with its own odds 1 or 0), so it keeps what it
        // kept without the odds (a leaf cut on its odds has lost its order).
        next.push_back(State{std::move(o.board), std::move(line), chance_free, played,
                             o_.grid_survival ? old_key : end.hi});
      };
      for (const State& st : beam) {
        for (size_t u = 0; u < units.size(); ++u) {
          if (st.played & (1u << u)) continue;
          const Pawn* p = st.board.find_pawn(units[u]);
          if (!p || !unit_can_play(*p)) continue;
          if (budget_exceeded() || (until > 0 && width > static_cast<size_t>(o_.beam_width) &&
                                    seconds_since(sh_.start) >= until)) {
            return;
          }
          const uint32_t played = st.played | (1u << u);
          // Where the unit acts from: here, or after each of its moves.
          std::vector<State> bases;
          bases.push_back(State{st.board, st.line, st.chance_free, played, kLow});
          Actions moves;
          unit_actions(st.board, *p, false, true, moves);
          for (size_t i = 0; i < moves.acts.size(); ++i) {
            SubAction& m = moves.acts[i];
            if (!execute(st.board, m, outs, moves.area_of(i)) || outs.empty()) continue;
            std::vector<SubAction> line = st.line;
            line.push_back(m);
            bases.push_back(State{outs[0].board, line, st.chance_free && !m.chance, played, kLow});
            consider(outs[0], line, st.chance_free && !m.chance, played);
          }
          for (const State& base : bases) {
            const Pawn* q = base.board.find_pawn(units[u]);
            if (!q || !unit_can_play(*q)) continue;
            Actions acts;
            unit_actions(base.board, *q, true, false, acts);
            // Same-effect shots (generate): the twin's board is in `seen`.
            std::vector<std::pair<int8_t, SkillEffect>> fired;
            for (size_t i = 0; i < acts.acts.size(); ++i) {
              SubAction& a = acts.acts[i];
              if (budget_exceeded()) return;
              std::optional<SkillEffect> fx;
              if (same_effect_ && a.kind == SubAction::Weapon && !a.target2.valid()) {
                aopts_.known_area = acts.area_of(i);
                fx = e_.pure_weapon_effect(base.board, a.uid, a.slot, a.target, aopts_);
                aopts_.known_area = nullptr;
                if (fx && !fx->q_effect.empty()) fx.reset();
                if (fx && std::any_of(fired.begin(), fired.end(), [&](const auto& f) {
                      return f.first == a.slot && same_effect(f.second, *fx);
                    })) {
                  ++stats_.same_effect_shots;
                  continue;
                }
              }
              if (!execute(base.board, a, outs, acts.area_of(i), fx ? &*fx : nullptr) || outs.empty()) continue;
              if (fx) fired.emplace_back(a.slot, std::move(*fx));
              std::vector<SubAction> line = base.line;
              line.push_back(a);
              consider(outs[0], std::move(line), base.chance_free && !a.chance, played);
            }
          }
        }
      }
      std::stable_sort(next.begin(), next.end(), [](const State& x, const State& y) {
        return x.played != y.played ? x.played < y.played : x.key > y.key;
      });
      std::vector<State> kept;
      for (size_t i = 0, run = 0; i < next.size(); ++i) {
        run = (i > 0 && next[i].played == next[i - 1].played) ? run + 1 : 0;
        if (run < width) kept.push_back(std::move(next[i]));
      }
      if (o_.lookahead) {
        // The beam ranks by bounds (NextTurn at its cap); the best few
        // chance-free states of the level get their exact value, so the
        // beam still seeds incumbents.
        std::vector<size_t> best;
        for (size_t i = 0; i < kept.size(); ++i) {
          if (kept[i].chance_free) best.push_back(i);
        }
        std::stable_sort(best.begin(), best.end(), [&](size_t x, size_t y) { return kept[x].key > kept[y].key; });
        for (size_t j = 0; j < best.size() && j < kBeamExact; ++j) {
          if (budget_exceeded()) return;
          State& st = kept[best[j]];
          const Interval end = leaf(st.board, alpha_of(kLow), false, false);
          if (end.exact()) improve(end.lo, st.line);
        }
      }
      beam = std::move(kept);
    }
  }

  // The best line from b along the table's best sub-actions (default
  // outcomes), for a table hit that improves the incumbent.
  std::vector<SubAction> table_line(const Board& start) {
    std::vector<SubAction> line;
    Board cur = start;
    std::vector<Outcome> outs;
    for (int guard = 0; guard < kMaxDepth; ++guard) {
      TTEntry e;
      if (!sh_.tt.get(hash_board(cur, HashMode::Search), e) || !e.has_best) break;
      SubAction act = e.best;
      if (!execute(cur, act, outs) || outs.empty()) break;
      line.push_back(act);
      cur = std::move(outs[0].board);
    }
    return line;
  }

  // alpha_of, except in a `local` search (below a grid survival chance node
  // whose odds may be below alpha's): there the window is the node's own.
  Score raise(const Score& alpha, bool local) { return local ? alpha : alpha_of(alpha); }

  // V over the chance outcomes of one child: the minimum (with grid
  // survival: chance_odds).
  Interval chance_node(Child& c, Score alpha, const Score& beta, std::vector<SubAction>* pv, int depth, bool local) {
    if (o_.grid_survival && c.tree) return chance_odds(c, alpha, beta, pv, depth, local);
    Score lo = kHigh, hi = kHigh;
    for (size_t k = 0; k < c.outcomes.size(); ++k) {
      std::vector<SubAction> sub;
      const Interval r = search(c.outcomes[k].board, c.outcomes[k].hash, alpha, std::min(beta, hi), false,
                                k == 0 ? &sub : nullptr, depth + 1, local);
      if (k == 0 && pv) *pv = std::move(sub);
      lo = std::min(lo, r.lo);
      hi = std::min(hi, r.hi);
      if (k + 1 < c.outcomes.size() && (hi <= raise(alpha, local) || aborted_)) {
        if (!aborted_) ++stats_.chance_cutoffs;
        return Interval{kLow, hi};  // the outcomes left can only lower it
      }
    }
    return Interval{lo, hi};
  }

  // Grid survival: V over the outcomes of one child is their odds folded
  // over the action's chance tree, its rest the worst of its class
  // (combine_odds). While alpha surely keeps the grid (odds 1) the node
  // only beats alpha if every outcome has odds 1, where the fold is the
  // minimum: the outcomes are searched as before (alpha, beta lowered to
  // the bound so far). Under a lower alpha a weighted outcome may matter even
  // below alpha (Star1): each outcome is searched in a `local` window, with
  // no incumbent raise, cut only where its odds alone would put the node's
  // odds below alpha's, the other outcomes counted at their upper bounds
  // (outcomes not searched yet at their tier bound: odds 0 once the grid
  // is gone). Thresholds only fall short of what is needed (the others'
  // bounds only drop later), so after one pass either some outcome was cut
  // and the node's odds are below alpha's, or every outcome is exact. An
  // outcome with no grid left (odds 0 for sure) is only searched when the
  // node's odds may be 0 and alpha's are not above 0: its rest counts then.
  Interval chance_odds(Child& c, const Score& alpha, const Score& beta, std::vector<SubAction>* pv, int depth,
                       bool local) {
    const size_t n = c.outcomes.size();
    ChanceTree& tree = *c.tree;
    const double p = sh_.resist_p;
    std::vector<Interval> r(n, Interval{kLow, kHigh});
    Score a = raise(alpha, local);
    const bool sure = a.v[kSurvival] >= kSurvivalOne;
    std::vector<size_t> order;
    if (sure) {
      for (size_t k = 0; k < n; ++k) order.push_back(k);
    } else {
      for (size_t k = 0; k < n; ++k) r[k].hi = sh_.bounds.of(c.outcomes[k].board);
      for (int dead = 0; dead < 2; ++dead) {
        for (size_t k = 0; k < n; ++k) {
          if ((r[k].hi.v[kSurvival] <= 0) == (dead == 1)) order.push_back(k);
        }
      }
    }
    for (size_t i = 0; i < order.size(); ++i) {
      const size_t k = order[i];
      Score ak = alpha, bk = beta;
      bool loc = local;
      if (sure) {
        bk = std::min(beta, combine_odds(tree, p, r).hi);
      } else if (r[k].hi.v[kSurvival] <= 0) {
        // No grid left: its rest only counts when the node's odds are 0,
        // which puts the node at or below an alpha with odds above 0.
        const Interval now = combine_odds(tree, p, r);
        if (a.v[kSurvival] > 0 || now.lo.v[kSurvival] > 0) continue;
        ak = a;  // odds 0 for all: the minimum, alpha as it is
        bk = kHigh;
        loc = true;
      } else if (a.v[kSurvival] <= 0) {
        // Alpha's odds are 0: an outcome at or below alpha has odds 0 and
        // only counts when the node's odds are 0 too (the minimum then).
        ak = a;
        bk = kHigh;
        loc = true;
      } else {
        // The highest odds of outcome k that, with the others at their
        // upper bounds, still leave the node's odds below alpha's.
        std::vector<int32_t> vals(n);
        for (size_t j = 0; j < n; ++j) vals[j] = clamp_odds(r[j].hi.v[kSurvival]);
        const int32_t target = a.v[kSurvival] - 1;
        auto odds_at = [&](int32_t t) {
          vals[k] = t;
          return tree.survival(p, vals);
        };
        int32_t t = -1;
        if (odds_at(0) <= target) {
          int32_t lo = 0, hi = kSurvivalOne;
          while (lo < hi) {
            const int32_t mid = lo + (hi - lo + 1) / 2;
            if (odds_at(mid) <= target) {
              lo = mid;
            } else {
              hi = mid - 1;
            }
          }
          t = lo;
        }
        ak = t >= 0 ? with_odds(kHigh, t) : kLow;  // cut iff the outcome's odds are <= t
        bk = kHigh;
        loc = true;
      }
      std::vector<SubAction> sub;
      const Interval rk = search(c.outcomes[k].board, c.outcomes[k].hash, ak, bk, false, k == 0 ? &sub : nullptr,
                                 depth + 1, loc);
      if (k == 0 && pv) *pv = std::move(sub);
      r[k] = Interval{std::max(r[k].lo, rk.lo), std::min(r[k].hi, rk.hi)};
      if (r[k].hi < r[k].lo) r[k] = rk;  // (only a truncated chance tree disagrees)
      a = raise(alpha, local);
      const Interval now = combine_odds(tree, p, r);
      if (i + 1 < order.size() && (now.hi <= a || aborted_)) {
        if (!aborted_) ++stats_.chance_cutoffs;
        return Interval{kLow, now.hi};
      }
    }
    return combine_odds(tree, p, r);
  }

  Interval search(const Board& b, const BoardHash& h, Score alpha, const Score& beta, bool chance_free,
                  std::vector<SubAction>* pv, int depth, bool local = false) {
    if (pv) pv->clear();
    if (budget_exceeded()) return Interval{kLow, sh_.bounds.of(b)};
    alpha = raise(alpha, local);
    if (o_.use_tt) {
      TTEntry e;
      if (sh_.tt.get(h, e)) {
        const Interval& v = e.v;
        if (v.exact() || v.hi <= alpha || v.lo >= beta) {
          ++stats_.tt_hits;
          Score inc;
          const bool has = incumbent(inc);
          if (chance_free && v.lo > kLow && (!has || v.lo > inc)) {
            const std::vector<SubAction> line = table_line(b);
            improve(v.lo, line);
            if (pv) *pv = line;
          } else if (pv && v.exact()) {
            *pv = table_line(b);
          }
          return v;
        }
      }
    }
    if (o_.use_tt && sh_.prior) {
      TTEntry e;
      if (const_cast<Shared*>(sh_.prior)->tt.get(h, e) && e.v.hi <= alpha) {
        ++stats_.prior_hits;
        return Interval{kLow, e.v.hi};
      }
    }
    ++stats_.nodes;
    sh_.nodes.fetch_add(1, std::memory_order_relaxed);
    const bool shared = sh_.threads > 1;
    if (shared) sh_.busy.add(h, 1);

    // End the turn here.
    const Interval end = leaf(b, alpha);
    Score lo = end.lo, hi = end.hi;
    std::vector<SubAction> best_pv;
    SubAction best_act;
    bool has_best = false;
    if (chance_free && end.exact()) improve(end.lo, {});
    alpha = std::max(raise(alpha, local), lo);

    const Score ub = sh_.bounds.of(b);
    auto finish = [&](Interval r) {
      if (shared) sh_.busy.add(h, -1);
      if (o_.use_tt && !aborted_) sh_.tt.put(h, TTEntry{r, best_act, has_best}, merge_entries, depth);
      if (chance_free && r.lo > kLow) improve(r.lo, best_pv);
      if (pv) *pv = best_pv;
      return r;
    };
    if (lo >= beta) return finish(Interval{lo, std::max(hi, ub)});
    if (o_.use_bounds && ub <= alpha) {
      ++stats_.bound_prunes;
      return finish(Interval{lo, std::max(hi, ub)});
    }
    if (depth >= kMaxDepth) {
      sh_.note("search depth limit reached");
      sh_.chance_exact = false;
      return finish(Interval{lo, std::max(hi, ub)});
    }

    ++stats_.depth_nodes[static_cast<size_t>(std::min<int>(depth, static_cast<int>(stats_.depth_nodes.size()) - 1))];
    std::vector<Child> children;
    Clock::time_point tg = Clock::now();
    generate(b, h, children);
    stats_.generate_s += seconds_since(tg);
    stats_.children += children.size();
    tg = Clock::now();
    if (o_.order_children) {
      for (Child& c : children) {
        if (budget_exceeded()) break;
        const Score a = raise(alpha, local);
        if (o_.use_bounds) {
          // A child its tier bound cuts (visit) needs no end-of-turn value.
          Score cub = kLow;
          for (const Outcome& oc : c.outcomes) cub = std::max(cub, sh_.bounds.of(oc.board));
          if (cub <= a) {
            c.order = cub;
            continue;
          }
        }
        Score key = kHigh;
        const bool may_stop = sh_.prefix_mode.load(std::memory_order_relaxed);
        // Grid survival: the old key (LeafMemo::key), so children are tried
        // in the order they were without the odds (on boards where every
        // line can lose the grid it also finds the best odds sooner than the
        // odds' bounds do: fewer buildings hit, fewer rolls).
        const bool old_key = o_.grid_survival;
        for (const Outcome& oc : c.outcomes) {
          Score k = kHigh;
          const Interval v = leaf(oc.board, a, true, may_stop, &k);
          key = std::min(key, old_key ? k : v.hi);
        }
        c.order = key;
      }
      std::stable_sort(children.begin(), children.end(),
                       [](const Child& x, const Child& y) { return x.order > y.order; });
    }
    stats_.order_s += seconds_since(tg);

    // With several threads, children another thread is searching right now
    // wait for a second pass (by then usually settled in the table).
    std::vector<size_t> later;
    bool cut = false;
    auto visit = [&](Child& c) {
      alpha = std::max(raise(alpha, local), lo);
      if (o_.use_bounds) {
        Score cub = kLow;
        for (const Outcome& oc : c.outcomes) cub = std::max(cub, sh_.bounds.of(oc.board));
        if (cub <= alpha) {
          ++stats_.bound_prunes;
          hi = std::max(hi, cub);
          return;
        }
      }
      std::vector<SubAction> sub;
      path_.push_back(c.act);
      Interval r;
      if (c.outcomes.size() == 1) {
        r = search(c.outcomes[0].board, c.outcomes[0].hash, alpha, beta, chance_free, &sub, depth + 1, local);
      } else {
        r = chance_node(c, alpha, beta, &sub, depth, local);
        Score inc;
        const bool has = incumbent(inc);
        if (chance_free && r.lo > kLow && (!has || r.lo > inc)) improve(r.lo, sub);
      }
      path_.pop_back();
      hi = std::max(hi, r.hi);
      if (r.lo > lo) {
        lo = r.lo;
        best_act = c.act;
        has_best = true;
        best_pv.clear();
        best_pv.push_back(c.act);
        best_pv.insert(best_pv.end(), sub.begin(), sub.end());
      }
      if (lo >= beta) cut = true;
    };
    for (int pass = 0; pass < 2 && !cut; ++pass) {
      const size_t n = pass == 0 ? children.size() : later.size();
      for (size_t i = 0; i < n && !cut; ++i) {
        Child& c = children[pass == 0 ? i : later[i]];
        if (budget_exceeded()) {
          hi = std::max(hi, ub);  // the children left are not searched
          cut = true;
          break;
        }
        if (pass == 0 && shared && c.outcomes.size() == 1) {
          int n_busy = 0;
          if (sh_.busy.get(c.outcomes[0].hash, n_busy) && n_busy > 0) {
            later.push_back(i);
            continue;
          }
        }
        visit(c);
      }
    }
    if (cut && lo >= beta) hi = std::max(hi, ub);  // the children left are not searched
    return finish(Interval{lo, hi});
  }

  Engine& e_;
  Shared& sh_;
  const SolveOptions& o_;
  size_t index_;
  ChanceDriver driver_;
  TurnContext tc_;
  ActionOptions aopts_;
  std::unordered_map<int32_t, Symbol> repair_;
  std::unordered_map<Symbol, bool> passive_, two_click_;
  Symbol move_skill_ = kNoSymbol;
  Symbol default_repair_ = kNoSymbol;
  bool aborted_ = false;
  bool verify_effects_ = false;
  bool same_effect_ = true;
  bool skip_spent_ = true;
  Board spent_scratch_;  // spent_turn_only
  uint64_t seen_version_ = ~uint64_t{0};
  bool has_incumbent_ = false;
  Score incumbent_ = kLow;
  std::vector<SubAction> path_;
  SolveStats stats_;
};

}  // namespace

namespace {

struct Seed {
  Score value;
  std::vector<PlayerAction> actions;
  bool contingent = false;
};

// `prior`: tables of an earlier search whose upper bounds hold for this one
// (Shared::prior). `keep`: receives this search's tables.
// `survey` (grid survival): search only lines that surely keep the grid
// (Shared::survey), and give up after `survey->first` seconds or
// `survey->second` nodes without one. `found`: whether a plan was found.
SolveResult search_turn(Engine& engine, const Board& board, const TurnContext& ctx, const SolveOptions& options,
                        const Seed* seed, const Shared* prior = nullptr, std::unique_ptr<Shared>* keep = nullptr,
                        const std::pair<double, uint64_t>* survey = nullptr, bool* found = nullptr) {
  auto owned = std::make_unique<Shared>(board, ctx, options);
  Shared& sh = *owned;
  sh.prior = prior;
  if (survey) {
    sh.survey = true;
    sh.floor = with_odds(kLow, kSurvivalOne);
    sh.survey_s = survey->first;
    sh.survey_nodes = survey->second;
  }
  // Hands the tables over on every return (after the workers are gone).
  struct Keep {
    std::unique_ptr<Shared>& owned;
    std::unique_ptr<Shared>* keep;
    ~Keep() {
      if (keep) *keep = std::move(owned);
    }
  } keep_tables{owned, keep};
  if (seed) {
    sh.has_incumbent = true;
    sh.incumbent = seed->value;
    sh.seeded = true;
    sh.seeded_actions = seed->actions;
    sh.seeded_contingent = seed->contingent;
    if (sh.adaptive && same_prefix(seed->value, sh.root_ub, sh.tiers)) sh.tiers_proven = true;
  }
  std::vector<Engine*> engines{&engine};
  for (Engine* e : options.helper_engines) {
    if (e && e != &engine) engines.push_back(e);
  }
  sh.threads = engines.size();
  sh.start = Clock::now();
  std::vector<std::unique_ptr<Worker>> workers;
  for (size_t i = 0; i < engines.size(); ++i) workers.push_back(std::make_unique<Worker>(*engines[i], sh, i));
  std::vector<Interval> roots(engines.size());
  std::vector<std::thread> threads;
  for (size_t i = 1; i < engines.size(); ++i) {
    threads.emplace_back([&, i] { roots[i] = workers[i]->run(); });
  }
  roots[0] = workers[0]->run();
  for (std::thread& t : threads) t.join();

  SolveResult out;
  for (const auto& w : workers) {
    const SolveStats& s = w->stats();
    out.stats.nodes += s.nodes;
    out.stats.sub_actions += s.sub_actions;
    out.stats.enemy_phases += s.enemy_phases;
    out.stats.chance_branches += s.chance_branches;
    out.stats.tt_hits += s.tt_hits;
    out.stats.leaf_hits += s.leaf_hits;
    out.stats.duplicate_children += s.duplicate_children;
    out.stats.bound_prunes += s.bound_prunes;
    out.stats.chance_cutoffs += s.chance_cutoffs;
    out.stats.sub_action_s += s.sub_action_s;
    out.stats.enemy_phase_s += s.enemy_phase_s;
    out.stats.lookahead_evals += s.lookahead_evals;
    out.stats.lookahead_hits += s.lookahead_hits;
    out.stats.lookahead_s += s.lookahead_s;
    out.stats.leaf_evals += s.leaf_evals;
    out.stats.phases_stopped += s.phases_stopped;
    out.stats.same_effect_shots += s.same_effect_shots;
    out.stats.same_effect_mismatches += s.same_effect_mismatches;
    out.stats.spent_turns += s.spent_turns;
    out.stats.prior_hits += s.prior_hits;
    out.stats.children += s.children;
    out.stats.generate_s += s.generate_s;
    out.stats.order_s += s.order_s;
    for (size_t d = 0; d < s.depth_nodes.size(); ++d) out.stats.depth_nodes[d] += s.depth_nodes[d];
    if (w->sampled()) sh.chance_exact = false;
  }
  out.stats.time_s = seconds_since(sh.start);
  out.stats.tt_entries = sh.tt.size();
  out.stats.leaf_entries = sh.leaf.size();
  out.stats.first_plan_s = sh.first_plan_s;
  out.stats.best_plan_s = sh.best_plan_s;
  out.stats.threads = static_cast<int>(engines.size());
  out.chance_exact = sh.chance_exact;
  out.timed_out = !sh.completed;
  out.extended = sh.prefix_mode;
  out.warnings = sh.warnings;
  if (found) *found = sh.has_incumbent;
  if (!sh.has_incumbent) return out;
  if (sh.seeded) {
    out.best.actions = sh.seeded_actions;
    out.best.contingent = sh.seeded_contingent;
  } else {
    out.best.actions = to_player_actions(sh.best_line);
    out.best.contingent =
        std::any_of(sh.best_line.begin(), sh.best_line.end(), [](const SubAction& a) { return a.chance; });
  }
  out.best.worst_case = sh.incumbent;
  // A search completed while proving only the first tiers proves those.
  out.proven_optimal = sh.completed && out.chance_exact && !out.extended;
  // Every root interval is sound; the tightest bound wins. A completed
  // search has root.hi <= the incumbent (solver.cpp header), or with the
  // adaptive budget, <= it in the tiers being proven.
  const Score claimed = out.extended ? raised(sh.incumbent, sh.tiers) : sh.incumbent;
  Score upper = kHigh;
  for (size_t i = 0; i < workers.size(); ++i) {
    if (workers[i]->complete() && roots[i].hi > claimed) {
      out.warnings.push_back("search completed with a root bound above the plan: " +
                             roots[i].hi.describe(options.tiers()));
    }
    upper = std::min(upper, roots[i].hi);
  }
  upper = std::max(upper, sh.incumbent);
  if (out.extended && sh.completed && out.chance_exact && upper == sh.incumbent) out.proven_optimal = true;
  if (out.proven_optimal) upper = sh.incumbent;
  out.upper_bound = upper;
  // Leading tiers in use where the bound meets the plan (an unused Threat
  // tier is 0 in both and not counted).
  int k = 0;
  for (int i = 0; i < kScoreKeys && upper.v[static_cast<size_t>(i)] == sh.incumbent.v[static_cast<size_t>(i)]; ++i) {
    if (tier_used(i, options.tiers())) ++k;
  }
  out.proven_components = k;
  if (!out.chance_exact) {
    out.warnings.push_back("chance not enumerated exactly (sampled death-effect seed or chance tree over the cap)");
  }
  // The returned plan, re-run open-loop: the same value unless it branches
  // on a chance node (then the plan is a policy, see Plan::contingent).
  if (!out.best.contingent && !out.best.actions.empty()) {
    const std::optional<Score> again = evaluate_plan(engine, board, ctx, out.best.actions, options);
    if (!again || *again != out.best.worst_case) {
      out.warnings.push_back("re-running the plan gives " +
                             (again ? again->describe(options.tiers()) : std::string("a refusal")));
    }
  }
  return out;
}

void add_stats(SolveStats& out, const SolveStats& in) {
  out.nodes += in.nodes;
  out.sub_actions += in.sub_actions;
  out.enemy_phases += in.enemy_phases;
  out.chance_branches += in.chance_branches;
  out.tt_hits += in.tt_hits;
  out.leaf_hits += in.leaf_hits;
  out.duplicate_children += in.duplicate_children;
  out.bound_prunes += in.bound_prunes;
  out.chance_cutoffs += in.chance_cutoffs;
  out.leaf_evals += in.leaf_evals;
  out.phases_stopped += in.phases_stopped;
  out.same_effect_shots += in.same_effect_shots;
  out.same_effect_mismatches += in.same_effect_mismatches;
  out.spent_turns += in.spent_turns;
  out.children += in.children;
  out.generate_s += in.generate_s;
  out.order_s += in.order_s;
  for (size_t d = 0; d < out.depth_nodes.size(); ++d) out.depth_nodes[d] += in.depth_nodes[d];
  out.sub_action_s += in.sub_action_s;
  out.enemy_phase_s += in.enemy_phase_s;
}

// The survey's share of the budget (search_survival).
constexpr double kSurveyShare = 0.25;

// Grid survival without a seed: first a survey (search_turn `survey`) that
// only searches lines surely keeping the grid, the incumbent floored at
// odds 1, so it runs as the search without the odds (an outcome losing the
// grid cuts at once). As soon as it finds such a line, that line's odds are
// 1 and every line that can lose the grid ranks below it: the survey is the
// whole search, with the whole budget. If it finds none within
// kSurveyShare of the budget (or proves there is none), the search with the
// odds runs on what is left. Lines that can lose the grid are only searched
// where no line keeps it for sure (searching them under an incumbent below
// odds 1 costs more: no cut by building HP and below, outcomes weighed).
SolveResult search_survival(Engine& engine, const Board& board, const TurnContext& ctx, const SolveOptions& options,
                            std::unique_ptr<Shared>* keep = nullptr) {
  if (!options.grid_survival) return search_turn(engine, board, ctx, options, nullptr, nullptr, keep);
  const Clock::time_point t0 = Clock::now();
  const std::pair<double, uint64_t> survey{
      options.time_limit_s > 0 ? options.time_limit_s * kSurveyShare : 0,
      options.node_limit > 0 ? std::max<uint64_t>(1, static_cast<uint64_t>(options.node_limit * kSurveyShare)) : 0};
  bool found = false;
  SolveResult first = search_turn(engine, board, ctx, options, nullptr, nullptr, keep, &survey, &found);
  if (found) return first;
  const double used = seconds_since(t0);
  SolveOptions rest = options;
  if (options.time_limit_s > 0) rest.time_limit_s = std::max(0.01, options.time_limit_s - used);
  if (options.max_time_s > 0) rest.max_time_s = std::max(rest.time_limit_s + 0.01, options.max_time_s - used);
  if (options.node_limit > 0) {
    rest.node_limit = std::max<uint64_t>(1, options.node_limit - std::min(options.node_limit - 1, first.stats.nodes));
  }
  SolveResult out = search_turn(engine, board, ctx, rest, nullptr, nullptr, keep);
  add_stats(out.stats, first.stats);
  out.stats.survey_s = used;
  out.stats.time_s = seconds_since(t0);
  out.stats.first_plan_s += used;
  out.stats.best_plan_s += used;
  return out;
}

}  // namespace

// With the lookahead, a search without it first (LookaheadOptions::
// base_share of the budget): its plan, scored with the lookahead
// (evaluate_plan: open-loop, a value the plan surely reaches), seeds the
// incumbent of the lookahead search, which gets the rest of the budget.
SolveResult solve_turn(Engine& engine, const Board& board, const TurnContext& ctx, const SolveOptions& options) {
  const double share = options.lookahead_options.base_share;
  if (!options.lookahead || share <= 0) return search_survival(engine, board, ctx, options);
  const Clock::time_point t0 = Clock::now();
  SolveOptions base = options;
  base.lookahead = false;
  base.max_time_s = 0;
  base.min_proven_tiers = 0;
  base.time_limit_s = options.time_limit_s > 0 ? options.time_limit_s * share : 0;
  // The tiers above NextTurn come first: while they are unproven the base
  // search may go on (adaptive budget) up to LookaheadOptions::base_max_share
  // of the whole budget (max_time_s, else time_limit_s).
  if (base.time_limit_s > 0) {
    const double whole = options.max_time_s > options.time_limit_s ? options.max_time_s : options.time_limit_s;
    const double cap = whole * std::max(share, options.lookahead_options.base_max_share);
    if (cap > base.time_limit_s) {
      base.max_time_s = cap;
      base.min_proven_tiers = 4;  // grid, buildings, mechs, stars: the tiers above NextTurn
    }
  }
  base.node_limit = options.node_limit > 0 ? std::max<uint64_t>(1, static_cast<uint64_t>(options.node_limit * share)) : 0;
  std::unique_ptr<Shared> base_tables;
  const SolveResult first = search_survival(engine, board, ctx, base, &base_tables);
  const double used = seconds_since(t0);
  SolveOptions rest = options;
  if (options.time_limit_s > 0) rest.time_limit_s = std::max(0.01, options.time_limit_s - used);
  if (options.max_time_s > 0) rest.max_time_s = std::max(rest.time_limit_s + 0.01, options.max_time_s - used);
  if (options.node_limit > 0) rest.node_limit = std::max<uint64_t>(1, options.node_limit - std::min(options.node_limit - 1, first.stats.nodes));
  std::optional<Seed> seed;
  // (An empty plan, the base search having found nothing, is ending the
  // turn at once: a plan too.)
  if (std::optional<Score> v = evaluate_plan(engine, board, ctx, first.best.actions, options)) {
    seed = Seed{*v, first.best.actions, first.best.contingent};
  }
  SolveResult out = search_turn(engine, board, ctx, rest, seed ? &*seed : nullptr, base_tables.get());
  base_tables.reset();
  // Both searches' work and time.
  add_stats(out.stats, first.stats);
  out.stats.first_plan_s = first.stats.first_plan_s;
  out.stats.best_plan_s += used;
  out.stats.time_s = seconds_since(t0);
  out.stats.base_search_s = used;
  out.stats.base_proven_components = first.proven_components;
  out.stats.base_best_plan_s = first.stats.best_plan_s;
  // The base search's bound holds for this search too: with the lookahead a
  // plan scores what it scores without it, NextTurn lowered from its cap
  // (0), so the value is at most the base search's value, which is at most
  // its bound. The tighter of the two bounds is sound (the base search may
  // have proven tiers the lookahead search did not get to).
  if (first.upper_bound && out.upper_bound && *first.upper_bound < *out.upper_bound) {
    if (*first.upper_bound < out.best.worst_case) {
      out.warnings.push_back("the base search's bound is below the plan: " +
                             first.upper_bound->describe(options.tiers()));
    } else {
      out.upper_bound = *first.upper_bound;
      int k = 0;
      for (int i = 0; i < kScoreKeys && out.upper_bound->v[static_cast<size_t>(i)] == out.best.worst_case.v[static_cast<size_t>(i)];
           ++i) {
        if (tier_used(i, options.tiers())) ++k;
      }
      out.proven_components = std::max(out.proven_components, k);
      if (*out.upper_bound == out.best.worst_case && out.chance_exact && first.chance_exact) out.proven_optimal = true;
    }
  }
  return out;
}

namespace {

// evaluate_plan with grid survival: one run of the plan and its enemy phase
// per chance leaf. `player`: the chance calls of the player's actions, each
// with the index of its sub-action (a move, then the weapon or repair, per
// PlayerAction); `enemy`: those of the enemy phase.
struct PlanRun {
  std::vector<std::pair<int, ChanceCall>> player;
  std::vector<ChanceCall> enemy;
  Score s;
};

// The value of the plan from player call `d` on, for runs that share their
// first d player calls, as the search values it (chance_odds, leaf): the
// enemy phase's odds and class once the player calls end; else the odds of
// the sub-action at call d folded over its outcomes (one per path through
// its calls), each valued from there on, and the worst of its class.
Score plan_value(const std::vector<PlanRun>& runs, const std::vector<size_t>& ids, size_t d, double p) {
  const PlanRun& first = runs[ids[0]];
  if (first.player.size() <= d) {
    ChanceTree tree;
    std::vector<int32_t> vals;
    Score worst = kHigh, alive = kHigh;
    for (size_t id : ids) {
      const PlanRun& r = runs[id];
      tree.add(ChanceLeaf{r.enemy, static_cast<int>(vals.size())});
      vals.push_back(r.s.v[kSurvival]);
      worst = std::min(worst, r.s);
      if (r.s.v[kSurvival] >= kSurvivalOne) alive = std::min(alive, r.s);
    }
    const int32_t odds = tree.survival(p, vals);
    return with_odds(odds > 0 ? alive : worst, odds);
  }
  const int sub = first.player[d].first;
  // Group the runs by their path through this sub-action's calls.
  std::map<std::vector<int>, std::vector<size_t>> groups;
  std::map<std::vector<int>, std::vector<ChanceCall>> paths;
  for (size_t id : ids) {
    const PlanRun& r = runs[id];
    std::vector<int> key;
    std::vector<ChanceCall> path;
    for (size_t i = d; i < r.player.size() && r.player[i].first == sub; ++i) {
      key.push_back(r.player[i].second.chosen);
      path.push_back(r.player[i].second);
    }
    groups[key].push_back(id);
    paths.emplace(key, std::move(path));
  }
  ChanceTree tree;
  std::vector<Score> values;
  std::vector<int32_t> vals;
  for (const auto& [key, members] : groups) {
    tree.add(ChanceLeaf{paths[key], static_cast<int>(values.size())});
    values.push_back(plan_value(runs, members, d + key.size(), p));
    vals.push_back(values.back().v[kSurvival]);
  }
  const int32_t odds = tree.survival(p, vals);
  Score cls = kHigh;
  for (const Score& v : values) {
    if (odds <= 0 || v.v[kSurvival] > 0) cls = std::min(cls, v);
  }
  return with_odds(cls, odds);
}

}  // namespace

std::optional<Score> evaluate_plan(Engine& engine, const Board& board, const TurnContext& ctx,
                                   const std::vector<PlayerAction>& plan, const SolveOptions& options,
                                   std::string* refused) {
  ChanceDriver d(board.grid_defense, options.extra_death_seeds);
  TurnContext tc = ctx;
  tc.events = nullptr;
  tc.log = nullptr;
  d.bind(tc);
  ActionOptions ao = action_options(tc);
  d.bind(ao);
  Score worst = kHigh;
  bool bad = false;
  uint64_t extra = 0;
  // Grid survival: every run, its chance calls tagged by sub-action.
  std::vector<PlanRun> runs;
  std::vector<int> tags;  // the sub-action of each player call of this run
  auto tag = [&](int sub) {
    while (tags.size() < d.calls().size()) tags.push_back(sub);
  };
  auto ok = [](const ActionResult& r) { return r.ok() || r.status == ActionStatus::NoEffect; };
  enumerate_chance(d, options.max_chance_leaves, extra, [&] {
    tags.clear();
    Board b = board;
    for (size_t i = 0; i < plan.size(); ++i) {
      const PlayerAction& a = plan[i];
      const Pawn* p = b.find_pawn(a.uid);
      if (a.move.valid() && p && a.move != p->pos) {
        d.set_roll_budget(cert_roll_budget(options, board, b), options.cert_resist_first);
        engine.lua().seed(kLuaSeed);
        const ActionResult r = engine.move(b, a.uid, a.move, ao);
        if (!ok(r)) {
          if (refused) *refused = "action " + std::to_string(i) + " move: " + to_string(r.status);
          bad = true;
          return false;
        }
        d.mark_random(r.resolve.chances);
        tag(static_cast<int>(2 * i));
      }
      if (a.kind == PlayerAction::Kind::None) continue;
      d.set_roll_budget(cert_roll_budget(options, board, b), options.cert_resist_first);
      engine.lua().seed(kLuaSeed);
      const ActionResult r =
          a.kind == PlayerAction::Kind::Weapon
              ? engine.fire_weapon(b, a.uid, a.weapon, a.target, a.target2, ao)
              : engine.repair(b, a.uid, a.target, a.weapon.empty() ? "Skill_Repair" : a.weapon, ao);
      if (!ok(r)) {
        if (refused) *refused = "action " + std::to_string(i) + " " + a.weapon + ": " + to_string(r.status);
        bad = true;
        return false;
      }
      d.mark_random(r.resolve.chances);
      tag(static_cast<int>(2 * i + 1));
    }
    const size_t player_calls = d.calls().size();
    d.set_roll_budget(cert_roll_budget(options, board, b), options.cert_resist_first);
    engine.lua().seed(kLuaSeed);
    const PhaseResult pr = engine.end_turn(b, tc);
    d.mark_random(pr.chances);
    Score s = score_turn(board, b, &tc, &pr, options.score_options());
    if (options.lookahead) {
      s[ScoreKey::NextTurn] = lookahead_value(engine, board, b, &pr, tc, options.lookahead_options);
    }
    worst = std::min(worst, s);
    if (options.grid_survival) {
      const std::vector<ChanceCall>& calls = d.calls();
      PlanRun run;
      for (size_t i = 0; i < player_calls; ++i) run.player.emplace_back(tags[i], calls[i]);
      run.enemy.assign(calls.begin() + static_cast<std::ptrdiff_t>(player_calls), calls.end());
      run.s = s;
      runs.push_back(std::move(run));
    }
    return true;
  });
  if (bad) return std::nullopt;
  if (options.grid_survival && !runs.empty()) {
    std::vector<size_t> ids(runs.size());
    for (size_t i = 0; i < ids.size(); ++i) ids[i] = i;
    worst = plan_value(runs, ids, 0, resist_chance(board, ctx));
  }
  return worst;
}

std::string describe_action(const Board& board, const PlayerAction& a) {
  const Pawn* p = board.find_pawn(a.uid);
  std::string s = p ? std::string(symbol_name(p->type)) : std::string("unit");
  s += "#" + std::to_string(a.uid);
  if (a.move.valid()) s += " move " + to_visual(a.move);
  if (a.kind == PlayerAction::Kind::Weapon) {
    s += (a.move.valid() ? ", " : " ") + std::string("fire ") + a.weapon + " at " + to_visual(a.target);
    if (a.target2) s += " then " + to_visual(*a.target2);
  } else if (a.kind == PlayerAction::Kind::Repair) {
    s += (a.move.valid() ? ", " : " ") + std::string("repair") +
         (a.weapon.empty() || a.weapon == "Skill_Repair" ? std::string() : " (" + a.weapon + ")");
    if (a.target.valid() && p && a.target != (a.move.valid() ? a.move : p->pos)) s += " at " + to_visual(a.target);
  } else if (!a.move.valid()) {
    s += " (no action)";
  }
  return s;
}

}  // namespace itb
