// The frame-exact executor's state and phases (stage 3/4). Internal to the
// engine; the public face is itb/executor.hpp.
//
// Frames are numbered from 0. Within a frame the game runs, in order:
//   input  player fire / the enemy driver (Board::ApplyEffect); the squad's
//          share of XP (BoardPlayer::UpdateXP) at the frame's start
//   P1     weapon animations: flights advance, impacts apply their hit
//   P2     tiles in x-major order: tile animations, deferred chasms, rules
//   P3     pawns in board-list order: removal, status rules, push/lunge
//          trackers, falls, deaths, walks, flights, queued-shot cancelling
//   P4     the stacked-effect queue (at most one chunk)
//   P5     only when no effect is active: psion leaders, kill credit and
//          corpse explosions, teleporters, then the last shot is cleared
//   P6     render: death animations and XP popups advance
// A timer started in some phase gets its first update the next time its
// owner's phase runs, which is how the game's same-frame orderings arise.
#pragma once

#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "itb/executor.hpp"
#include "itb/timing.hpp"

namespace itb::detail {

enum class Phase : uint8_t { Input = 0, P1 = 1, P2 = 2, P3 = 3, P4 = 4, P5 = 5, P6 = 6 };

// One entry of Board's stacked effects (`effects` + parallel `delays`).
struct StackEntry {
  SkillEffect effect;
  float delay = kFullDelay;  // FULL -1, PROJ -2, > 0 seconds left, anything else: now
  Symbol shot = kNoSymbol;   // the weapon name carried with the effect
  int cause = 0;             // what released it (timing analysis)
};

// A weapon animation on the board's list (P1).
struct WeaponAnim {
  ProjectileKind kind = ProjectileKind::None;
  SpaceDamage hit;         // the copy taken at launch
  bool applies = false;    // impact applies `hit` (projectiles, artillery, droppers)
  int64_t end_frame = 0;   // P1 of this frame: impact / no longer busy
  int cause = 0;
};

// A tile animation started with ANIM_DELAY: the tile is busy until P2 of
// `end_frame`, where it finishes and is pruned.
struct TileAnim {
  Point point;
  int64_t end_frame = 0;
};

// The shared push / lunge tracker of a pawn.
struct Tracker {
  bool running = false;
  int64_t end_frame = 0;
  Dir dir = Dir::None;
  bool has_melee = false;  // a lunge: the stored hit lands when it finishes
  SpaceDamage melee;
  Point from = kInvalidPoint;
  int cause = 0;
};

// Transient per-pawn state (Pawn fields the turn-step Board does not keep).
struct PawnSim {
  int32_t uid = -1;
  Tracker tracker;
  // Walk (SetManualPath): remaining steps and the step timer.
  bool walking = false;
  std::vector<Point> walk_queue;
  int64_t walk_next = 0;
  // Own leap/charge flight.
  bool flying = false;
  int64_t flight_end = 0;
  // Fall animation (fall state 2).
  bool falling = false;
  int64_t fall_end = 0;
  // Teleport: 1 vanishing, 2 appearing.
  int teleport_phase = 0;
  int64_t teleport_end = 0;
  Point teleport_to = kInvalidPoint;
  // Burrow: 1 diving, 2 emerging. A dive with no destination (a hurt
  // burrower, Burrow(-1, -1)) ends underground, off the board.
  int burrow_phase = 0;
  int64_t burrow_end = 0;
  Point burrow_to = kInvalidPoint;
  // Death bookkeeping.
  bool dead = false;              // seen dead
  bool death_pending = false;     // +0x961: ProcessDeath not run yet
  bool death_fx_pending = false;  // TriggerDeathEffect not run yet
  bool detonate_pending = false;  // +0x1331: corpse explosion not run yet
  int64_t death_frame = 0;        // first render (P6) update of the death animation
  int frozen_updates = -1;        // KillInstant: the animation stopped after this many updates
  const AnimTimeline* death_anim = nullptr;  // null: no death animation
  int64_t xp_gone = -1;           // first frame whose P3 sees no XP popup
};

// PawnSim by uid: O(1) lookup through a small open-addressing index (uids
// are sparse: recorded boards mix 0..2 with 2600+), stable references
// (deque storage), erase by uid. Iteration order is storage order; every
// caller treats the entries as an unordered set.
class PawnSimTable {
 public:
  PawnSim& get(int32_t uid) {
    if (PawnSim* ps = find(uid)) return *ps;
    size_t slot;
    if (!free_.empty()) {
      slot = free_.back();
      free_.pop_back();
      slots_[slot] = PawnSim{};
    } else {
      slot = slots_.size();
      slots_.emplace_back();
    }
    slots_[slot].uid = uid;
    insert(uid, static_cast<int32_t>(slot));
    return slots_[slot];
  }
  PawnSim* find(int32_t uid) {
    const int32_t i = lookup(uid);
    return i >= 0 ? &slots_[static_cast<size_t>(i)] : nullptr;
  }
  const PawnSim* find(int32_t uid) const { return const_cast<PawnSimTable*>(this)->find(uid); }
  void erase(int32_t uid) {
    if (keys_.empty()) return;
    for (size_t k = home(uid);; k = (k + 1) & mask()) {
      if (keys_[k] == kEmpty) return;
      if (keys_[k] == uid && vals_[k] >= 0) {
        const size_t slot = static_cast<size_t>(vals_[k]);
        slots_[slot].uid = kFree;
        free_.push_back(slot);
        vals_[k] = kTomb;  // keeps later keys of this probe chain reachable
        return;
      }
    }
  }
  template <class F>
  void for_each(F&& f) const {
    for (const PawnSim& ps : slots_) {
      if (ps.uid != kFree) f(ps);
    }
  }

 private:
  static constexpr int32_t kFree = INT32_MIN;   // PawnSim::uid of an unused slot
  static constexpr int32_t kEmpty = INT32_MIN;  // keys_: never used
  static constexpr int32_t kTomb = -1;          // vals_: erased entry
  size_t mask() const { return keys_.size() - 1; }
  size_t home(int32_t uid) const {
    return (static_cast<uint32_t>(uid) * 0x9E3779B1u >> 7) & mask();
  }
  int32_t lookup(int32_t uid) const {
    if (keys_.empty()) return -1;
    for (size_t k = home(uid);; k = (k + 1) & mask()) {
      if (keys_[k] == kEmpty) return -1;
      if (keys_[k] == uid && vals_[k] >= 0) return vals_[k];
    }
  }
  void insert(int32_t uid, int32_t slot) {
    if (keys_.empty() || 2 * (used_ + 1) > keys_.size()) grow();
    for (size_t k = home(uid);; k = (k + 1) & mask()) {
      if (keys_[k] == kEmpty) {
        keys_[k] = uid;
        vals_[k] = slot;
        ++used_;
        return;
      }
    }
  }
  // Rebuilds the index at twice the size (tombstones dropped).
  void grow() {
    std::vector<int32_t> keys = std::move(keys_), vals = std::move(vals_);
    keys_.assign(std::max<size_t>(64, keys.size() * 2), kEmpty);
    vals_.assign(keys_.size(), kTomb);
    used_ = 0;
    for (size_t k = 0; k < keys.size(); ++k) {
      if (keys[k] != kEmpty && vals[k] >= 0) insert(keys[k], vals[k]);
    }
  }
  std::deque<PawnSim> slots_;
  std::vector<size_t> free_;
  std::vector<int32_t> keys_;  // uid per index entry (kEmpty: never used)
  std::vector<int32_t> vals_;  // slot per index entry (kTomb: erased)
  size_t used_ = 0;            // index entries in use, tombstones included
};

// A board change the timing analysis looks at.
enum class ActionKind : uint8_t { Chunk, DelayedChunk, GatedChunk, ProjChunk, Impact, PushEnd };

struct Action {
  ActionKind kind;
  int64_t frame = 0;
  std::vector<Point> tiles;
  int cause = 0;   // id of this action's own cause
  int origin = 0;  // for PushEnd: the cause that started the push
  bool hazard = false;  // PushEnd: the pawn landed where a pickup awaits
};

class Simulation final : public FrameHooks {
 public:
  Simulation(Resolver& owner, Board& board, ResolveContext& ctx);
  ~Simulation() override;
  Simulation(const Simulation&) = delete;
  Simulation& operator=(const Simulation&) = delete;

  ResolveResult resolve(const SkillEffect& effect, const WeaponInfo& weapon);
  ResolveResult settle();
  void add_effect(SkillEffect effect);
  void add_delay(float seconds);
  void damage_space(const SpaceDamage& sd);
  void bounce(Point p, int amount);
  void add_chance(const ChanceRecord& chance);
  ResolveResult apply(const std::function<void()>& edit);
  void leaders_now() { update_leaders(); }
  bool is_busy() const { return busy_state() != 0; }

  Board& board() { return board_; }
  int64_t frame() const { return frame_; }
  const FrameClock& clock() const { return clock_; }

  // FrameHooks.
  bool pawn_busy(const Pawn& pawn, bool ignore_push) const override;
  bool hole_ready(Point p) const override;
  bool start_fall(Pawn& pawn) override;
  void instant_kill(const Pawn& pawn) override;
  void start_dive(Pawn& pawn) override;
  void grid_critical() override;

 private:
  // ---- frame loop (executor.cpp)
  void run(ResolveResult& result);
  void run_frame(int64_t f);
  bool idle() const;
  int64_t next_event(int64_t f) const;
  void skip_frames(int64_t count);
  int64_t first_update(Phase updater) const;
  int64_t first_pawn_update(int32_t uid) const;

  // ---- busy state (executor.cpp)
  int busy_state() const;
  bool effect_active() const;
  bool death_anim_playing(const PawnSim& ps) const;
  int death_updates(const PawnSim& ps) const;

  // ---- effects (executor.cpp)
  void apply_effect(const SkillEffect& effect, Symbol shot, int cause, ActionKind kind);
  void apply_hit(const SpaceDamage& sd, int cause);
  void launch(const SpaceDamage& sd, int cause);
  void start_melee(Pawn& attacker, const SpaceDamage& sd, int cause);
  void start_movement(const SkillEffect& effect, const SpaceDamage& sd);
  void update_stack();
  void update_weapon_anims();
  void update_tiles();
  uint64_t busy_tile_states();
  void update_pawn(int32_t uid);
  void walk_step(Pawn& pawn, PawnSim& ps);
  void register_tile_anim(const SpaceDamage& sd);
  void register_holes();
  void push_front(SkillEffect effect, float delay, Symbol shot, int cause);
  void push_back(SkillEffect effect, float delay, Symbol shot, int cause);

  // ---- push and death (push_death.cpp)
  void start_push(int32_t uid, Dir dir, int cause);
  void finish_push(Pawn& pawn, PawnSim& ps);
  bool push_blocked(Point p, Dir dir, int32_t pusher);
  void push_damage(Point target, Point from, Dir dir);
  void note_deaths();
  void note_death(const Pawn& pawn, PawnSim& ps);
  void process_death(Pawn& pawn, PawnSim& ps);
  void trigger_death_effect(Pawn& pawn, PawnSim& ps);
  bool is_exploding(const Pawn& pawn) const;
  void detonate_corpse(Pawn& pawn, PawnSim& ps);
  void update_leaders();
  void update_teleporters();
  // ---- kill credit and pilot experience (push_death.cpp, itb/pilot_xp.hpp)
  void credit_kill(const Pawn& victim);
  void update_kills(Pawn& pawn);
  void update_xp();
  void clear_last_shot();
  void void_shock(int32_t owner, Symbol shot, bool hit, Point point);
  bool removable(const Pawn& pawn, const PawnSim& ps) const;
  void remove(int32_t uid);
  void cancel_queued_shot(Pawn& pawn);

  // ---- helpers
  PawnSim& state(int32_t uid);
  const PawnSim* find_state(int32_t uid) const;
  const AnimTimeline* timeline(Symbol anim);
  const AnimTimeline* death_timeline(const Pawn& pawn);
  void log(ResolveEventType type, Point p = kInvalidPoint, int32_t uid = -1, int amount = 0);
  void flag(TimingKind kind, Point p, int32_t uid = -1);
  void analyze_timing();
  int new_cause() { return ++causes_; }

  Resolver& owner_;
  Board& board_;
  ResolveContext& ctx_;
  RulesContext& rules_;
  FrameClock clock_;
  Durations dur_;

  int64_t frame_ = 0;
  Phase phase_ = Phase::Input;
  std::vector<int32_t> p3_done_;  // pawns whose P3 update already ran this frame
  bool changed_ = false;           // sim state changed this frame (beyond timers)
  Board before_;                   // the board at the start of the frame (run)
  std::vector<int32_t> order_;     // P3 / P5 pawn order (run_frame)
  // busy_tile_states: the tiles it was computed for, and its result.
  std::array<Tile, kTileCount> busy_tiles_at_{};
  uint64_t busy_tiles_mask_ = 0;
  bool busy_tiles_valid_ = false;

  std::vector<StackEntry> stack_;
  std::vector<WeaponAnim> anims_;
  std::vector<TileAnim> tile_anims_;
  std::vector<TileAnim> holes_;  // deferred chasm bounces
  PawnSimTable pawns_;
  std::vector<std::pair<int32_t, bool>> noted_;  // the list at the last note_deaths pass

  // Last shot (EventSystem): owner, team and name of the effect last fired.
  int32_t last_owner_ = -1;
  Team last_team_ = Team::None;
  Symbol last_shot_ = kNoSymbol;
  // LastShot +0x28: the effect's target, or for a TC_Enrage shot the first
  // entry's tile with a pawn. Its HitSomething flag is rules_.shot_hit.
  Point last_point_ = kInvalidPoint;
  // EventSystem data a death leaves for Pawn::UpdateKills, per shooter uid:
  // XP ("xp_<id>"), XP-giving kills ("kill_<id>") and every enemy kill
  // ("any_kill_<id>"); and XP for the whole squad ("env_xp").
  struct KillCredit {
    int32_t uid = -1;
    int xp = 0;
    int kills = 0;
    int any_kills = 0;
  };
  std::vector<KillCredit> credits_;
  int env_xp_ = 0;

  bool track_leaders_ = false;  // the board's psion comes from leader pawns on it
  // Event 17 (grid left at 1) raised since the current frame started
  // (grid_critical); run_frame makes it current for the next frame.
  bool crit_pending_ = false;
  int causes_ = 0;

  // ctx.timelines when it matches this clock, durations and data, else a
  // cache of this simulation's own.
  TimelineCache own_timelines_;
  TimelineCache* timelines_ = nullptr;

  // Timing analysis.
  std::vector<Action> actions_;
  std::vector<int64_t> tile_anim_ends_;
  ResolveResult* result_ = nullptr;

  // The caller's hooks, restored when the simulation ends.
  FrameHooks* saved_frame_ = nullptr;
  std::function<bool(Point, int)> saved_resist_;
  std::function<void(Board&, const std::string&, Point)> saved_script_;
  bool script_replaced_ = false;
};

}  // namespace itb::detail
