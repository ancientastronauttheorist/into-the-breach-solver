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
#include <cstdio>
#include <functional>
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

struct ChanceCall {
  Hook hook;
  int options = 1;  // branches of this call (1 = nothing to branch on)
  int chosen = 0;
  uint32_t seed = 0;     // Seed: the seed handed out
  bool matched = false;  // Seed: its death effect drew random numbers
};

class ChanceDriver {
 public:
  ChanceDriver(int grid_defense, int extra_seeds)
      : resist_(grid_defense > 0), seeds_(1 + std::max(0, extra_seeds)) {}

  void begin(const std::vector<int>& forced) {
    forced_ = &forced;
    calls_.clear();
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
    o.grid_resist = [this](Point, int) { return next(Hook::Resist, resist_ ? 2 : 1) == 1; };
    o.choose = [this](const ChanceRecord& node) { return next(Hook::Choose, std::max(1, node.options)); };
    o.death_seed = [this](const Pawn& p) { return seed_for(p); };
    o.spider_egg = [this](Resolver&, const Pawn&, const std::vector<Point>& tiles) {
      return next(Hook::Egg, static_cast<int>(tiles.size()));
    };
  }
  void bind(TurnContext& tc) {
    tc.grid_resist = [this](Point, int) { return next(Hook::Resist, resist_ ? 2 : 1) == 1; };
    tc.choose = [this](const ChanceRecord& node) { return next(Hook::Choose, std::max(1, node.options)); };
    tc.death_seed = [this](const Pawn& p) { return seed_for(p); };
    tc.spider_egg = [this](Resolver&, const Pawn&, const std::vector<Point>& tiles) {
      return next(Hook::Egg, static_cast<int>(tiles.size()));
    };
  }

 private:
  int next(Hook h, int options) {
    const size_t i = calls_.size();
    int c = forced_ && i < forced_->size() ? (*forced_)[i] : 0;
    c = std::clamp(c, 0, std::max(0, options - 1));
    calls_.push_back(ChanceCall{h, std::max(1, options), c, 0, false});
    return c;
  }
  uint32_t seed_for(const Pawn& p) {
    const size_t i = calls_.size();
    const int k = forced_ && i < forced_->size() ? (*forced_)[i] : 0;
    // k = 0: the engine's default seed (uid + 1); others: fixed samples.
    const uint32_t seed = k == 0 ? static_cast<uint32_t>(p.uid + 1)
                                 : static_cast<uint32_t>(p.uid + 1) * 2654435761u + 40503u * static_cast<uint32_t>(k);
    calls_.push_back(ChanceCall{Hook::Seed, 1, k, seed, false});
    return seed;
  }

  bool resist_;
  int seeds_;
  const std::vector<int>* forced_ = nullptr;
  std::vector<ChanceCall> calls_;
  bool sampled_ = false;
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
    int dead_mechs = 0, mech_room = 0, enemies = 0, enemy_hp = 0;
    for (const Pawn& p : root.pawns()) {
      const int hp0 = p.alive() && !p.fallen && p.pos.valid() ? p.hp : 0;
      if (p.mech && p.team == Team::Player) {
        if (hp0 <= 0) ++dead_mechs;
        mech_room += std::max(0, p.max_hp + leech + pilot_hp_room(p) - hp0);
      } else if (scored_enemy(p) && hp0 > 0) {
        ++enemies;
        enemy_hp += hp0;
      }
    }
    caps_[ScoreKey::MechsLost] = dead_mechs;
    caps_[ScoreKey::MechHpLost] = mech_room;
    caps_[ScoreKey::ObjectivesFailed] = 0;
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
    grid_cap_ = caps_[ScoreKey::GridLost];
    mech_rate_ = mech_grid_rate(root, opt.score_options());
    if (mech_rate_ >= 0) {
      caps_[ScoreKey::GridLost] = 2 * grid_cap_ + (2 * mech_rate_ + 1) * dead_mechs;
    }
    if (caps) {
      for (size_t i = 0; i < caps_.v.size(); ++i) caps_.v[i] = std::min(caps_.v[i], caps->v[i]);
    }
  }

  Score global() const { return caps_; }
  // objective_progress_cap at the root (objectives_first only, else -1).
  int progress_cap() const { return progress_cap_; }

  Score of(const Board& b) const {
    Score s = caps_;
    if (grid_monotone_ && mech_rate_ >= 0) {
      const int grid = std::min(grid_cap_, b.grid_power - root_grid_);
      const int32_t slot = mech_grid_slot(mech_rate_, grid, s[ScoreKey::MechsLost], b.grid_power);
      s[ScoreKey::GridLost] = std::min(s[ScoreKey::GridLost], slot);
    } else if (grid_monotone_) {
      s[ScoreKey::GridLost] = std::min(s[ScoreKey::GridLost], b.grid_power - root_grid_);
    }
    s[ScoreKey::BuildingHpLost] = std::min(s[ScoreKey::BuildingHpLost], building_hp(b) - root_building_);
    return s;
  }

 private:
  int root_grid_ = 0;
  int root_building_ = 0;
  bool grid_monotone_ = true;
  int progress_cap_ = -1;
  int grid_cap_ = 0;   // the GridLost cap in grid units
  int mech_rate_ = -1; // mech_grid_rate at the root (-1: off)
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
      : root(r), o(opt), bounds(r, opt.tier_caps, opt, &ctx), tt(opt.tt_max_entries), leaf(opt.tt_max_entries), busy(SIZE_MAX) {
    base = ctx;
    base.events = nullptr;
    base.log = nullptr;
    adaptive = opt.min_proven_tiers > 0 && opt.time_limit_s > 0 && opt.max_time_s > opt.time_limit_s;
    tiers = components_for_tiers(std::clamp(opt.min_proven_tiers, 0, used_tier_count(opt.tier_on())),
                                 opt.tier_on());
    score_options = opt.score_options();
    root_ub = bounds.of(r);
  }
  const Board& root;
  const SolveOptions& o;
  TierBounds bounds;
  TurnContext base;
  SharedMap<TTEntry> tt;
  SharedMap<Interval> leaf;
  SharedMap<int> busy;
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
  std::atomic<bool> tiers_proven{false};  // the incumbent meets root_ub in the first `tiers`
  std::atomic<bool> prefix_mode{false};   // past time_limit_s, proving the first `tiers` only

  std::mutex mu;  // the incumbent and the warnings
  bool has_incumbent = false;
  Score incumbent = kLow;
  std::vector<SubAction> best_line;
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
    if (!incumbent(inc)) return alpha;
    if (sh_.prefix_mode.load(std::memory_order_relaxed)) inc = raised(inc, sh_.tiers);
    return std::max(alpha, inc);
  }

  // E(B): the worst case of ending the turn on B.
  Interval leaf(const Board& b, const Score& alpha) {
    BoardHash key{};
    if (o_.use_tt) {
      key = hash_board(b, HashMode::EndTurn);
      Interval memo;
      if (sh_.leaf.get(key, memo) && (memo.exact() || memo.hi <= alpha)) {
        ++stats_.leaf_hits;
        return memo;
      }
    }
    Score worst = kHigh;
    bool cut = false;
    const EnumEnd end = enumerate_chance(driver_, o_.max_chance_leaves, stats_.chance_branches, [&] {
      Board after = b;
      const Clock::time_point t0 = Clock::now();
      e_.lua().seed(kLuaSeed);
      const PhaseResult pr = e_.end_turn(after, tc_);
      stats_.enemy_phase_s += seconds_since(t0);
      ++stats_.enemy_phases;
      driver_.mark_random(pr.chances);
      if (!pr.exact) sh_.note("enemy phase: the recorded data does not pin every step down (EnvInexact)");
      if (!pr.emerged_unknown.empty()) sh_.note("enemy phase: spawns of unknown type emerge without a pawn");
      const Score s = score_turn(sh_.root, after, &tc_, &pr, sh_.score_options);
      if (sh_.bounds.progress_cap() >= 0 && s[ScoreKey::ObjectiveProgress] > sh_.bounds.progress_cap()) {
        sh_.note("objective progress above its derived cap (objective_progress_cap is unsound here)");
      }
      worst = std::min(worst, s);
      if (worst <= alpha) {
        cut = true;
        return false;
      }
      return true;
    });
    if (end == EnumEnd::Truncated) sh_.chance_exact = false;
    if (cut) ++stats_.chance_cutoffs;
    const Interval r = cut ? Interval{kLow, worst} : Interval{worst, worst};
    if (o_.use_tt) sh_.leaf.put(key, r, intersect);
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
  // b, when unit_actions computed it cleanly (ActionOptions::known_area).
  bool execute(const Board& b, SubAction& act, std::vector<Outcome>& outs,
               const std::vector<Point>* area = nullptr) {
    outs.clear();
    bool refused = false;
    aopts_.known_area = area;
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
      for (const Outcome& o : outs) {
        if (o.hash == h) return true;
      }
      outs.push_back(Outcome{std::move(next), h});
      return true;
    });
    aopts_.known_area = nullptr;
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

  void generate(const Board& b, const BoardHash& h, std::vector<Child>& out) {
    out.clear();
    std::unordered_set<BoardHash, BoardHashOf> seen;
    seen.insert(h);
    Actions acts;
    for (const Pawn& p : b.pawns()) {
      if (unit_can_play(p)) unit_actions(b, p, true, true, acts);
    }
    for (size_t i = 0; i < acts.acts.size(); ++i) {
      SubAction& act = acts.acts[i];
      Child c;
      if (!execute(b, act, c.outcomes, acts.area_of(i))) continue;
      if (c.outcomes.size() == 1 && !seen.insert(c.outcomes[0].hash).second) {
        ++stats_.duplicate_children;
        continue;
      }
      c.act = act;
      out.push_back(std::move(c));
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
        const Interval end = leaf(o.board, alpha_of(kLow));
        if (chance_free && end.exact()) improve(end.lo, line);
        next.push_back(State{std::move(o.board), std::move(line), chance_free, played, end.hi});
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
            for (size_t i = 0; i < acts.acts.size(); ++i) {
              SubAction& a = acts.acts[i];
              if (budget_exceeded()) return;
              if (!execute(base.board, a, outs, acts.area_of(i)) || outs.empty()) continue;
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

  // V over the chance outcomes of one child: the minimum.
  Interval chance_node(Child& c, Score alpha, const Score& beta, std::vector<SubAction>* pv, int depth) {
    Score lo = kHigh, hi = kHigh;
    for (size_t k = 0; k < c.outcomes.size(); ++k) {
      std::vector<SubAction> sub;
      const Interval r = search(c.outcomes[k].board, c.outcomes[k].hash, alpha, std::min(beta, hi), false,
                                k == 0 ? &sub : nullptr, depth + 1);
      if (k == 0 && pv) *pv = std::move(sub);
      lo = std::min(lo, r.lo);
      hi = std::min(hi, r.hi);
      if (k + 1 < c.outcomes.size() && (hi <= alpha_of(alpha) || aborted_)) {
        if (!aborted_) ++stats_.chance_cutoffs;
        return Interval{kLow, hi};  // the outcomes left can only lower it
      }
    }
    return Interval{lo, hi};
  }

  Interval search(const Board& b, const BoardHash& h, Score alpha, const Score& beta, bool chance_free,
                  std::vector<SubAction>* pv, int depth) {
    if (pv) pv->clear();
    if (budget_exceeded()) return Interval{kLow, sh_.bounds.of(b)};
    alpha = alpha_of(alpha);
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
    alpha = std::max(alpha_of(alpha), lo);

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

    std::vector<Child> children;
    generate(b, h, children);
    if (o_.order_children) {
      for (Child& c : children) {
        if (budget_exceeded()) break;
        Score key = kHigh;
        for (const Outcome& oc : c.outcomes) key = std::min(key, leaf(oc.board, alpha_of(alpha)).hi);
        c.order = key;
      }
      std::stable_sort(children.begin(), children.end(),
                       [](const Child& x, const Child& y) { return x.order > y.order; });
    }

    // With several threads, children another thread is searching right now
    // wait for a second pass (by then usually settled in the table).
    std::vector<size_t> later;
    bool cut = false;
    auto visit = [&](Child& c) {
      alpha = std::max(alpha_of(alpha), lo);
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
        r = search(c.outcomes[0].board, c.outcomes[0].hash, alpha, beta, chance_free, &sub, depth + 1);
      } else {
        r = chance_node(c, alpha, beta, &sub, depth);
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
  uint64_t seen_version_ = ~uint64_t{0};
  bool has_incumbent_ = false;
  Score incumbent_ = kLow;
  std::vector<SubAction> path_;
  SolveStats stats_;
};

}  // namespace

SolveResult solve_turn(Engine& engine, const Board& board, const TurnContext& ctx, const SolveOptions& options) {
  Shared sh(board, ctx, options);
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
  if (!sh.has_incumbent) return out;
  out.best.actions = to_player_actions(sh.best_line);
  out.best.worst_case = sh.incumbent;
  out.best.contingent =
      std::any_of(sh.best_line.begin(), sh.best_line.end(), [](const SubAction& a) { return a.chance; });
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
                             roots[i].hi.describe(options.tier_on()));
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
    if (tier_used(i, options.tier_on())) ++k;
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
                             (again ? again->describe(options.tier_on()) : std::string("a refusal")));
    }
  }
  return out;
}

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
  auto ok = [](const ActionResult& r) { return r.ok() || r.status == ActionStatus::NoEffect; };
  enumerate_chance(d, options.max_chance_leaves, extra, [&] {
    Board b = board;
    for (size_t i = 0; i < plan.size(); ++i) {
      const PlayerAction& a = plan[i];
      const Pawn* p = b.find_pawn(a.uid);
      if (a.move.valid() && p && a.move != p->pos) {
        engine.lua().seed(kLuaSeed);
        const ActionResult r = engine.move(b, a.uid, a.move, ao);
        if (!ok(r)) {
          if (refused) *refused = "action " + std::to_string(i) + " move: " + to_string(r.status);
          bad = true;
          return false;
        }
        d.mark_random(r.resolve.chances);
      }
      if (a.kind == PlayerAction::Kind::None) continue;
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
    }
    engine.lua().seed(kLuaSeed);
    const PhaseResult pr = engine.end_turn(b, tc);
    d.mark_random(pr.chances);
    worst = std::min(worst, score_turn(board, b, &tc, &pr, options.score_options()));
    return true;
  });
  if (bad) return std::nullopt;
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
