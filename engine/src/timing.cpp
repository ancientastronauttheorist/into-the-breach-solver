// Frame counts for the game's float32 timers. Built with -ffp-contract=off:
// each accumulation below is rounded the way the game rounds it.

#include "itb/timing.hpp"

#include <algorithm>
#include <cmath>

#include "itb/game_data.hpp"

namespace itb {
namespace {

// Far beyond any real timer (minutes of game time); stops runaway loops on
// nonsensical input.
constexpr int kMaxUpdates = 1 << 20;

// AnimTracker::GetProgress: elapsed / length, clamped to [0, 1].
float tracker_progress(float elapsed, float length) {
  const float p = elapsed / length;
  if (p > 0.0f) return p >= 1.0f ? 1.0f : p;
  return 0.0f;
}

int sign_of(float d) {
  if (d == 0.0f) return 0;
  return d > 0.0f ? 1 : -1;
}

// ProjectileAnimation::Update's movement and impact test, from `pos` towards
// `target` at velocity (vx, vy) per speed unit.
int fly(const FrameClock& clock, float px, float py, float tx, float ty, float vx, float vy) {
  const float s = clock.speed;
  const float reach = s * std::sqrt(vy * vy + vx * vx);
  for (int n = 1; n <= kMaxUpdates; ++n) {
    px = px + vx * s;
    py = py + s * vy;
    const float dx = tx - px;
    const float dy = ty - py;
    if (std::sqrt(dx * dx + dy * dy) <= reach) return n;
  }
  return kMaxUpdates;
}

}  // namespace

FrameClock FrameClock::steady(double fps, int speed_level) {
  FrameClock c;
  c.fps = fps;
  c.speed_level = speed_level;
  // CFPS::OnLoop: real seconds since the last frame times (16 + 7 * level),
  // clamped to [0, 0.5]; the speed factor is the mean of the last five.
  // The frame it measures runs from its second clock read of the previous
  // frame to its first read of this one, so each frame misses CFPS's own
  // bookkeeping between the two reads (kCfpsMeasureGap): at a steady rate
  // the step is a hair below 1 / fps.
  const double seconds = 1.0 / fps - kCfpsMeasureGap;
  float raw = static_cast<float>(seconds * static_cast<double>(
                                               static_cast<float>(speed_level * 7) + 16.0f));
  raw = std::clamp(raw, 0.0f, 0.5f);
  float sum = 0.0f;
  for (int i = 0; i < 5; ++i) sum = sum + raw;
  c.speed = sum / 5.0f;
  c.step = c.speed * 0.0625f;
  return c;
}

int FrameClock::tracker_updates(float length) const {
  float elapsed = 0.0f;
  for (int n = 1; n <= kMaxUpdates; ++n) {
    elapsed = 1.0f * step + elapsed;
    if (length < elapsed) return n;
  }
  return kMaxUpdates;
}

int FrameClock::eased_tracker_updates(float length, float ease_in, float ease_out) const {
  float elapsed = 0.0f;
  for (int n = 1; n <= kMaxUpdates; ++n) {
    const float p = tracker_progress(elapsed, length);
    float factor = 1.0f;
    if (p < ease_in) {
      factor = std::max(p / ease_in, 0.1f);
    } else if (p > 1.0f - ease_out) {
      factor = std::max((1.0f - p) / ease_out, 0.1f);
    }
    elapsed = factor * step + elapsed;
    if (length < elapsed) return n;
  }
  return kMaxUpdates;
}

int FrameClock::delay_updates(float delay) const {
  float remaining = delay;
  for (int n = 1; n <= kMaxUpdates; ++n) {
    remaining = remaining - step;
    if (remaining <= 0.0f) return n;
  }
  return kMaxUpdates;
}

// ---- AnimTimeline -----------------------------------------------------------

AnimTimeline::AnimTimeline(const FrameClock& clock, const AnimDef& def, float fade_out)
    : AnimTimeline(clock, def.num_frames, def.time, def.lengths, def.loop, fade_out) {}

AnimTimeline::AnimTimeline(const FrameClock& clock, int num_frames, float time,
                           const std::vector<float>& lengths, bool loop, float fade_out)
    : num_frames_(num_frames) {
  if (num_frames_ <= 0) {
    total_ = 0;
    return;
  }
  auto frame_length = [&](int i) {
    return i >= 0 && i < static_cast<int>(lengths.size()) ? lengths[static_cast<size_t>(i)] : time;
  };
  // Animation::Start: frame 0, its tracker restarted.
  State s{0, 0.0f, frame_length(0)};
  states_.push_back(s);
  if (loop) {
    // A looping animation never finishes; keep one cycle for progress.
    total_ = -1;
    return;
  }
  // Animation::Update @00795010: the frame tracker advances by one step; once
  // past the frame's length it loops (elapsed - length) and the next frame
  // starts from 0 (the overshoot is dropped). On the last frame a fade-out
  // first holds the frame: the countdown is set to `fade_out` and, like on
  // every later update, decremented by the step; the frame only advances on
  // the update that takes it to <= 0, the tracker looping on meanwhile.
  // Done once the frame index reaches the frame count.
  float countdown = 0.0f;
  for (int n = 1; n <= kMaxUpdates && s.frame < num_frames_; ++n) {
    s.elapsed = 1.0f * clock.step + s.elapsed;
    const bool done = s.length < s.elapsed;
    if (done) s.elapsed = s.elapsed - s.length;
    bool advance = false;
    if (done && fade_out != 0.0f && countdown == 0.0f && s.frame == num_frames_ - 1) {
      countdown = fade_out;
    }
    if (countdown > 0.0f) {
      countdown = countdown - clock.step;
      advance = !(0.0f < countdown);
    } else {
      advance = done;
    }
    if (advance) {
      ++s.frame;
      s.length = frame_length(s.frame);
      s.elapsed = s.length <= 0.0f ? s.length : 0.0f;
    }
    states_.push_back(s);
  }
  total_ = static_cast<int>(states_.size()) - 1;
}

float AnimTimeline::progress(int n) const {
  if (states_.empty()) return std::nanf("");
  const State& s = states_[static_cast<size_t>(std::clamp(n, 0, static_cast<int>(states_.size()) - 1))];
  const float p = tracker_progress(s.elapsed, s.length);
  return (p * s.length + static_cast<float>(s.frame) * s.length) /
         (static_cast<float>(num_frames_) * s.length);
}

// ---- Flights ------------------------------------------------------------------

int projectile_updates(const FrameClock& clock, Point from, Point to, float tiles_per_unit) {
  const float px = static_cast<float>(from.x) + 0.5f;
  const float py = static_cast<float>(from.y) + 0.5f;
  const float tx = static_cast<float>(to.x) + 0.5f;
  const float ty = static_cast<float>(to.y) + 0.5f;
  // StartProjectile: the shot flies along one axis only, x for left/right.
  const Point d = to - from;
  const bool horizontal = std::abs(d.x) > std::abs(d.y);
  float vx = 0.0f;
  float vy = 0.0f;
  if (horizontal) {
    vx = tiles_per_unit * static_cast<float>(sign_of(tx - px));
  } else {
    vy = tiles_per_unit * static_cast<float>(sign_of(ty - py));
  }
  return fly(clock, px, py, tx, ty, vx, vy);
}

int arc_updates(const FrameClock& clock, Point from, Point to, float y_velocity, float gravity,
                float start_height, float x_offset) {
  const float px = static_cast<float>(from.x) + (0.5f + x_offset);
  const float py = static_cast<float>(from.y) + 0.5f;
  const float tx = (0.5f + x_offset) + static_cast<float>(to.x);
  const float ty = static_cast<float>(to.y) + 0.5f;
  const float dx = tx - px;
  const float dy = ty - py;
  const float dist = std::sqrt(dx * dx + dy * dy);
  if (dist == 0.0f) return 1;
  // Flight time: the later root of height(t) = 0 (StartArc's SolveQuadratic).
  const float a = gravity * -0.5f;
  const float disc = y_velocity * y_velocity - 4.0f * a * start_height;
  const float root = std::sqrt(disc);
  const float t1 = (-y_velocity + root) / (2.0f * a);
  const float t2 = (-y_velocity - root) / (2.0f * a);
  const float flight = std::max(t1, t2);
  // Constant horizontal speed covering the distance in that time.
  const float speed = dist / flight;
  const float vx = (dx / dist) * speed;
  const float vy = (dy / dist) * speed;
  return fly(clock, px, py, tx, ty, vx, vy);
}

// ---- Durations ------------------------------------------------------------------

Durations Durations::from(const GameData* data) {
  Durations d;
  if (!data) return d;
  d.x_velocity = data->value("x_velocity", d.x_velocity);
  d.y_velocity = data->value("y_velocity", d.y_velocity);
  d.gravity = data->value("gravity", d.gravity);
  d.laser = data->value("laser_length", d.laser);
  return d;
}

}  // namespace itb
