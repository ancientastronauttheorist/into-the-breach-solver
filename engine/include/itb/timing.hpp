// The game's clock, replayed exactly for a steady frame rate (stage 3/4).
//
// Every gameplay timer in the game advances by the same per-frame step, in
// float32. These helpers turn a duration into the number of frames the game
// needs to cross it, by replaying that float32 arithmetic for a constant step,
// so the executor can jump from event to event instead of stepping frames.
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "itb/core.hpp"

namespace itb {

struct AnimDef;
class GameData;

// Real seconds per frame that CFPS::OnLoop (@0079a2a0) does not see. It
// reads the clock once to measure the frame (against the time it stored the
// frame before) and again at its end to store the time for the next frame,
// so the measured frame time is the real one minus the bookkeeping between
// the two reads, well under a microsecond. Its size hardly matters (10 ns to
// 10 us give the same frame counts); its sign does. At a steady 60 fps the
// step is just below 1/60, so a timer that is a whole number of frames long
// takes one more update than float32(1/60), which rounds up, suggests: a
// 0.4 s push finishes on update 25, not 24, and a 0.3 s delay fires on the
// 19th countdown, not the 18th (live 2026-10-13 run 17, m08 turn 1: a Pierce
// Shot's delayed push of a mech into a dying Moth's tile went through).
inline constexpr double kCfpsMeasureGap = 1e-7;

// CFPS at a steady frame rate. `speed` is the speed factor s (16 units per
// game second), `step` the game seconds per frame h = s / 16.
struct FrameClock {
  double fps = 60.0;
  int speed_level = 0;  // CFPS speed level (0 = normal; up to 3)
  float speed = 0.0f;
  float step = 0.0f;

  // The clock the game settles into at a constant frame rate. Below 32 fps
  // the per-frame factor is clamped, so the game slows down instead.
  static FrameClock steady(double fps = 60.0, int speed_level = 0);

  // AnimTracker / TimerHelper: updates until the elapsed time (starting at 0,
  // +step per update) exceeds `length`.
  int tracker_updates(float length) const;
  // The same with the ease in/out the game uses for tile bounces (progress
  // below `ease_in` or above 1 - `ease_out` slows the tracker, never below 10%).
  int eased_tracker_updates(float length, float ease_in, float ease_out) const;
  // A positive stack delay: countdown passes (-step each) until it is <= 0.
  int delay_updates(float delay) const;
  // How many passes a countdown that has `remaining` left still needs.
  int countdown_updates(float remaining) const { return delay_updates(remaining); }

  bool operator==(const FrameClock&) const = default;
};

// Animation timing (Animation::Update / GetTotalProgress) for one animation
// definition, replayed update by update.
//
// `fade_out` is Animation::SetFadeOut (0 = none): once the last frame's
// tracker runs out, the animation holds that frame for `fade_out` seconds
// before it is done. The frame tracker loops (its loop flag is always set,
// Animation::LoadData), so during the hold it keeps running from its
// overshoot and GetTotalProgress drops back to (n - 1) / n and climbs again:
// a one-frame animation restarts from 0.
class AnimTimeline {
 public:
  AnimTimeline() = default;
  AnimTimeline(const FrameClock& clock, int num_frames, float time,
               const std::vector<float>& lengths = {}, bool loop = false,
               float fade_out = 0.0f);
  AnimTimeline(const FrameClock& clock, const AnimDef& def, float fade_out = 0.0f);

  bool valid() const { return num_frames_ > 0; }
  // Updates until IsDone (the animation then stops). Looping animations
  // never finish: returns -1.
  int total_updates() const { return total_; }
  // GetTotalProgress after `n` updates (n clamped to the finished state).
  float progress(int n) const;

 private:
  struct State {
    int frame = 0;
    float elapsed = 0.0f;
    float length = 0.0f;
  };
  int num_frames_ = 0;
  int total_ = 0;
  std::vector<State> states_;  // states_[n] = after n updates
};

// Flight of a weapon or pawn animation: the update (1-based) on which it
// impacts, replaying ProjectileAnimation::Update.
// Straight shot (projectiles, charges) at `tiles_per_unit` (x_velocity, /3
// for "shot_bigone", x1.5 for "shot_pierce").
int projectile_updates(const FrameClock& clock, Point from, Point to, float tiles_per_unit);
// Arc (artillery, leaps): launched upward at `y_velocity` from height
// `start_height` under `gravity`; `x_offset` shifts the launch point (the
// game moves "fireball" arcs 0.25 left).
int arc_updates(const FrameClock& clock, Point from, Point to, float y_velocity, float gravity,
                float start_height = 18.0f, float x_offset = 0.0f);

// ---- Durations -----------------------------------------------------------
//
// Every duration the executor uses, in game seconds. Values tagged "estimate"
// are not pinned down by the decompile (spec stage 3/4 §C.2, §G.1); they only
// shift when things happen, never what happens, and are kept here so they
// can be tuned in one place.
struct Durations {
  float push = 0.4f;          // Pawn::Push tracker
  float lunge = 0.15f;        // Pawn::Melee tracker
  float laser = 0.5f;         // laser_length (Values)
  float xp_popup = 1.0f;      // XpAnim tracker (constructor)
  float fall = 0.4f;          // Pawn::Fall tracker: the pawn's default tracker length
  float hole = 0.5f;          // deferred chasm bounce (BounceAnim, eased)
  float hole_ease = 0.15f;    // its ease in/out
  float teleport_out = 0.3f;  // estimate: teleport vanish animation
  float teleport_in = 0.3f;   // estimate: teleport appear animation
  float teleport_pause = 0.1f;  // AddDelay after a teleport lands
  float burrow_dive = 0.4f;   // estimate: burrow dive animation
  float burrow_emerge = 0.4f; // estimate: burrow emerge animation
  float walk_step = 0.15f;    // estimate: one walked tile (game: max(0.08, anim speed x setting))
  float air_strike = 1.0f;    // estimate: air strike plane pass
  float dropper = 1.0f;       // estimate: pylon drop
  // Death animation used when a pawn type has no definition at hand (spec
  // default: 8 frames of 0.14 s).
  int default_death_frames = 8;
  float default_death_time = 0.14f;
  // Pawn::SetupAnimation gives the death animation of a pawn that is not a
  // corpse SetFadeOut(0.5): its last frame is held 0.5 s longer (the body
  // stays, busy in the enemy phase, and its progress restarts; AnimTimeline).
  float death_fade_out = 0.5f;
  // Projectile physics (Values in game.lua).
  float x_velocity = 0.7f;
  float y_velocity = 18.0f;
  float gravity = 3.0f;
  float arc_height = 18.0f;  // ProjectileAnimation::StartArc start height

  // The defaults with the Values read from the game's scripts.
  static Durations from(const GameData* data);

  bool operator==(const Durations&) const = default;
};

}  // namespace itb
