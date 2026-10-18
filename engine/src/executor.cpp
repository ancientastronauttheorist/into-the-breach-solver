// The SkillEffect executor (stage 4): Board::ApplyEffect, the stacked-effect
// queue, weapon animations, movement entries and the frame loop. Pushes and
// deaths (stage 3) are in push_death.cpp.

#include "itb/executor.hpp"

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "itb/game_data.hpp"
#include "itb/movement.hpp"
#include "rules_detail.hpp"
#include "simulation.hpp"

namespace itb {
namespace {

// Board::GetPawn on a const board: the first living occupant, else the last
// one that still counts as a pawn.
const Pawn* pawn_on(const Board& board, Point p) {
  if (!p.valid()) return nullptr;
  return board_pawn(const_cast<Board&>(board), p);
}

bool art_has(Symbol art, std::string_view part) {
  return symbol_name(art).find(part) != std::string_view::npos;
}

Dir direction_of(Point from, Point to) {
  const Point d = to - from;
  if (d.x == 0 && d.y == 0) return Dir::Up;
  if (std::abs(d.x) > std::abs(d.y)) return d.x > 0 ? Dir::Right : Dir::Left;
  return d.y > 0 ? Dir::Down : Dir::Up;
}

}  // namespace

// ---- Skill side ------------------------------------------------------------------

void prepare_effect(SkillEffect& effect, Point origin, Point target, Team team, Symbol explosion) {
  effect.origin = origin;
  effect.target = target;
  effect.team = team;
  for (auto* list : {&effect.effect, &effect.q_effect}) {
    for (SpaceDamage& sd : *list) {
      if (sd.animation == kNoSymbol) sd.animation = explosion;
      if (!sd.projectile_source.valid()) sd.projectile_source = origin;
      sd.owner_team = team;
    }
  }
}

bool is_boosted(const Board& board, const Pawn& pawn) {
  return pawn.boosted || mutation_affects(board, pawn, Leader::Boosted) ||
         (pawn.has_pilot(kPilotArrogantBoost) && pawn.hp >= pawn.max_hp);
}

void check_alterations(const Board& board, std::vector<SpaceDamage>& list, const Pawn* owner,
                       bool move_skill) {
  if (!owner) return;
  // Vek Hormones: enemy shots hitting enemy-team pawns, keyed on who stands
  // on each tile right now.
  if (owner->team == Team::Enemy) {
    int bonus = 0;
    if (board.has_passive(kPassiveFriendlyFireAB)) {
      bonus = 3;
    } else if (board.has_passive(kPassiveFriendlyFireA) || board.has_passive(kPassiveFriendlyFireB)) {
      bonus = 2;
    } else if (board.has_passive(kPassiveFriendlyFire)) {
      bonus = 1;
    }
    if (bonus > 0) {
      for (SpaceDamage& sd : list) {
        if (effective_damage(sd) <= 0 || sd.damage == kDamageDeath) continue;
        const Pawn* target = pawn_on(board, sd.loc);
        if (target && target->team == Team::Enemy) sd.damage += bonus;
      }
    }
  }
  // Boost, after Hormones.
  if (!move_skill && is_boosted(board, *owner)) {
    for (SpaceDamage& sd : list) boost_damage(sd);
  }
}

// ---- Resolver ----------------------------------------------------------------------

Resolver::Resolver(Board& board, ResolveContext& ctx)
    : sim_(std::make_unique<detail::Simulation>(*this, board, ctx)) {}
Resolver::~Resolver() = default;

ResolveResult Resolver::resolve(const SkillEffect& effect, const WeaponInfo& weapon) {
  return sim_->resolve(effect, weapon);
}
ResolveResult Resolver::settle() { return sim_->settle(); }
Board& Resolver::board() { return sim_->board(); }
int64_t Resolver::frame() const { return sim_->frame(); }
const FrameClock& Resolver::clock() const { return sim_->clock(); }
void Resolver::add_effect(SkillEffect effect) { sim_->add_effect(std::move(effect)); }
void Resolver::add_delay(float seconds) { sim_->add_delay(seconds); }
void Resolver::damage_space(const SpaceDamage& sd) { sim_->damage_space(sd); }
void Resolver::bounce(Point p, int amount) { sim_->bounce(p, amount); }
void Resolver::add_chance(const ChanceRecord& chance) { sim_->add_chance(chance); }
ResolveResult Resolver::apply(const std::function<void()>& edit) { return sim_->apply(edit); }
void Resolver::update_leaders() { sim_->leaders_now(); }
bool Resolver::busy() const { return sim_->is_busy(); }

ResolveResult resolve_effect(Board& board, const SkillEffect& effect, const WeaponInfo& weapon,
                             ResolveContext& ctx) {
  Resolver r(board, ctx);
  return r.resolve(effect, weapon);
}

ResolveResult resolve_effects(Board& board, const std::vector<FiredEffect>& effects,
                              ResolveContext& ctx) {
  Resolver r(board, ctx);
  ResolveResult total;
  total.start_frame = r.frame();
  for (const FiredEffect& f : effects) {
    ResolveResult one = r.resolve(f.effect, f.weapon);
    total.quiescent = total.quiescent && one.quiescent;
    total.timing.insert(total.timing.end(), one.timing.begin(), one.timing.end());
    total.chances.insert(total.chances.end(), one.chances.begin(), one.chances.end());
    total.end_frame = one.end_frame;
  }
  if (effects.empty()) total.end_frame = total.start_frame;
  return total;
}

namespace detail {

// ---- Construction ------------------------------------------------------------------

Simulation::Simulation(Resolver& owner, Board& board, ResolveContext& ctx)
    : owner_(owner), board_(board), ctx_(ctx), rules_(ctx.rules) {
  clock_ = FrameClock::steady(ctx.config.fps, ctx.config.speed_level);
  dur_ = ctx.durations ? *ctx.durations : Durations::from(ctx.data);
  TimelineCache* shared = ctx.timelines;
  if (shared && shared->ready && !(shared->data == ctx.data && shared->clock == clock_ && shared->durations == dur_)) {
    shared = nullptr;  // built for other settings: keep them, use our own
  }
  timelines_ = shared ? shared : &own_timelines_;
  if (!timelines_->ready) {
    timelines_->ready = true;
    timelines_->data = ctx.data;
    timelines_->clock = clock_;
    timelines_->durations = dur_;
    timelines_->default_death = AnimTimeline(clock_, dur_.default_death_frames, dur_.default_death_time,
                                              {}, false, dur_.death_fade_out);
  }
  if (!rules_.data) rules_.data = ctx.data;
  // The caller's hooks are moved aside (and back in the destructor); the
  // replacements reach them through `this`.
  saved_frame_ = rules_.frame;
  rules_.frame = this;
  if (ctx_.run_script) {
    saved_script_ = std::move(rules_.run_script);
    script_replaced_ = true;
    rules_.run_script = [this](Board&, const std::string& script, Point loc) {
      ctx_.run_script(owner_, script, loc);
    };
  }
  // Grid Defense rolls are the chance nodes of a resolution: log each one.
  saved_resist_ = std::move(rules_.grid_resist);
  rules_.grid_resist = [this](Point p, int amount) {
    const bool resisted = saved_resist_ ? saved_resist_(p, amount) : false;
    if (result_) {
      result_->chances.push_back(
          ChanceRecord{ChanceKind::GridDefense, frame_, p, amount, resisted ? 1 : 0, 2});
    }
    return resisted;
  };
  for (const Pawn& p : board_.pawns()) {
    if (p.leader != Leader::None) track_leaders_ = true;
    PawnSim& ps = state(p.uid);
    // Bodies already on the board: their death was handled long ago.
    if (!p.alive()) ps.dead = true;
  }
  // Pawns the Soldier psion or the Abomination affects already carry its +1.
  for (Pawn& p : board_.pawns()) {
    if (p.alive() && mutation_adds_health(board_.psion) && mutation_affects(board_, p, board_.psion)) {
      p.health_bonus = true;
    }
  }
}

Simulation::~Simulation() {
  // A grid hit no frame followed (every run ends with it answered): its
  // shields still go up.
  if (crit_pending_ && board_.has_passive(kPassiveCritDefense)) crit_defense_shields(board_, rules_);
  rules_.frame = saved_frame_;
  rules_.grid_resist = std::move(saved_resist_);
  if (script_replaced_) rules_.run_script = std::move(saved_script_);
}

ResolveResult Simulation::resolve(const SkillEffect& effect, const WeaponInfo& weapon) {
  ResolveResult result;
  result.start_frame = frame_;
  result_ = &result;
  phase_ = Phase::Input;
  changed_ = false;

  SkillEffect fx = effect;
  if (weapon.queued) {
    // Skill::FireQueued: the queued list replaces the instant one.
    fx.effect = std::move(fx.q_effect);
    fx.q_effect.clear();
  }
  if (weapon.queued) {
    // SkillManager::FireQueued: the stored attack is used up.
    if (Pawn* p = board_.find_pawn(fx.owner)) p->queued = QueuedShot{};
  }
  const Pawn* owner = board_.find_pawn(fx.owner);
  if (fx.team == Team::None && owner) fx.team = owner->team;
  if (!weapon.prepared) {
    prepare_effect(fx, fx.origin, fx.target, fx.team, weapon.explosion);
    check_alterations(board_, fx.effect, owner, weapon.move_skill);
  }
  const bool fired = !fx.effect.empty();
  const int cause = new_cause();
  log(ResolveEventType::ChunkApplied, fx.origin, fx.owner);
  apply_effect(fx, weapon.name, cause, ActionKind::Chunk);
  // Pawn::FireWeapon: Boost is spent right after the first chunk.
  if (fired && !weapon.move_skill) {
    if (Pawn* p = board_.find_pawn(fx.owner)) p->boosted = false;
  }
  run(result);
  result_ = nullptr;
  return result;
}

ResolveResult Simulation::settle() {
  ResolveResult result;
  result.start_frame = frame_;
  result_ = &result;
  phase_ = Phase::Input;
  run(result);
  result_ = nullptr;
  return result;
}

void Simulation::add_effect(SkillEffect effect) {
  push_back(std::move(effect), kFullDelay, last_shot_, new_cause());
}

void Simulation::add_delay(float seconds) {
  push_back(SkillEffect{}, seconds, last_shot_, new_cause());
}

void Simulation::damage_space(const SpaceDamage& sd) { apply_hit(sd, new_cause()); }

// Board::Bounce @0093ef10 -> BoardSpace::Bounce @00905f70 -> BounceAnim::Start:
// the tile's bounce tracker runs for 0.5 s (eased in and out for amounts 0
// and 100), updated in the tile's P2 pass, and BoardSpace::IsBusy is true
// while it runs (busy code 2). A small bounce (|amount| <= 9) is skipped on a
// tile whose first listed pawn, living or a corpse being present, spans
// several tiles. An off-board point bounces the dummy tile: nothing waits.
void Simulation::bounce(Point p, int amount) {
  if (!p.valid()) return;
  if (amount >= -9 && amount <= 9) {
    const Pawn* first = nullptr;
    bool present = false;
    for (const Pawn& pawn : board_.pawns()) {
      if (!pawn.occupies(p)) continue;
      if (pawn.alive() || is_corpse(board_, pawn)) present = true;
      if (!first || pawn.arrival < first->arrival) first = &pawn;
    }
    if (present && first && first->extra_tile().valid()) return;
  }
  const int n = amount == 0 || amount == 100
                    ? clock_.eased_tracker_updates(dur_.bounce, dur_.bounce_ease, dur_.bounce_ease)
                    : clock_.tracker_updates(dur_.bounce);
  tile_anims_.push_back(TileAnim{p, first_update(Phase::P2) + n - 1});
  changed_ = true;
}

void Simulation::add_chance(const ChanceRecord& chance) {
  if (result_) result_->chances.push_back(chance);
}

ResolveResult Simulation::apply(const std::function<void()>& edit) {
  ResolveResult result;
  result.start_frame = frame_;
  result_ = &result;
  phase_ = Phase::Input;
  changed_ = true;
  rules_.pushes.clear();
  if (edit) edit();
  note_deaths();
  register_holes();
  const std::vector<PendingPush> pushes = std::move(rules_.pushes);
  rules_.pushes.clear();
  const int cause = new_cause();
  for (const PendingPush& push : pushes) start_push(push.uid, push.dir, cause);
  run(result);
  result_ = nullptr;
  return result;
}

// ---- Frame loop ------------------------------------------------------------------------

void Simulation::run(ResolveResult& result) {
  int64_t f = frame_;
  const int64_t limit = f + ctx_.config.max_frames;
  for (;;) {
    if (f > limit) {
      result.quiescent = false;
      frame_ = f;
      break;
    }
    before_ = board_;  // reuses the buffers of the last frame
    changed_ = false;
    run_frame(f);
    const bool dirty = changed_ || !(board_ == before_);
    // Idle and a whole frame changed nothing: settled. An idle board that
    // still changed (e.g. a walker arriving on a pod, picked up by the next
    // frame's tile rules) runs on, as the game's frames do.
    if (idle() && !dirty) {
      frame_ = f + 1;
      break;
    }
    if (idle()) {
      f = f + 1;
      continue;
    }
    const int64_t next = dirty ? f + 1 : next_event(f);
    if (next <= f) {
      // Nothing left that could ever change the board, yet not idle (e.g. a
      // FULL_DELAY entry waiting on something that never ends).
      result.quiescent = false;
      frame_ = f + 1;
      break;
    }
    skip_frames(next - f - 1);
    f = next;
  }
  phase_ = Phase::Input;
  result.end_frame = frame_ - 1;
  analyze_timing();
}

void Simulation::run_frame(int64_t f) {
  frame_ = f;
  p3_done_.clear();

  // BoardPlayer::OnLoop starts with UpdateXP, before Board::OnLoop.
  update_xp();

  // EventSystem::OnLoop: the events raised before this frame become current.
  const bool crit_now = crit_pending_;
  crit_pending_ = false;

  phase_ = Phase::P1;
  update_weapon_anims();

  // Board::OnLoop, between the weapon animations and the tiles: Critical
  // Shields answer a grid hit of the previous frame that left the grid at 1.
  if (crit_now && board_.has_passive(kPassiveCritDefense)) crit_defense_shields(board_, rules_);

  phase_ = Phase::P2;
  update_tiles();

  phase_ = Phase::P3;
  // run_frame is never re-entered (hooks only queue effects or apply hits),
  // so one order buffer serves P3 and P5.
  std::vector<int32_t>& order = order_;
  order.clear();
  for (const Pawn& p : board_.pawns()) order.push_back(p.uid);
  for (int32_t uid : order) update_pawn(uid);

  phase_ = Phase::P4;
  update_stack();

  phase_ = Phase::P5;
  if (!effect_active()) {
    update_leaders();
    order.clear();
    for (const Pawn& p : board_.pawns()) order.push_back(p.uid);
    for (int32_t uid : order) {
      if (Pawn* p = board_.find_pawn(uid)) update_kills(*p);
      if (Pawn* p = board_.find_pawn(uid)) detonate_corpse(*p, state(uid));
    }
    update_teleporters();
    // Board::OnLoop ends the last shot once nothing is active any more.
    if (!effect_active()) clear_last_shot();
  }

  // The mission's per-frame Lua (BaseUpdate), with the frame's board.
  if (ctx_.frame_hook) {
    ctx_.frame_hook(owner_);
    note_deaths();
    register_holes();
  }

  // P6: death animations and XP popups advance; their state is a function of
  // the frame number (death_updates, xp_gone).
  phase_ = Phase::P6;
}

bool Simulation::idle() const {
  if (!stack_.empty() || !anims_.empty() || !tile_anims_.empty() || !holes_.empty() || crit_pending_) return false;
  for (const Pawn& p : board_.pawns()) {
    const PawnSim* ps = find_state(p.uid);
    if (!ps) {
      if (!p.alive() && !p.mech && !is_corpse(board_, p)) return false;
      continue;
    }
    if (pawn_busy(p, false) || ps->death_pending || ps->death_fx_pending) return false;
    // A body waiting for an explosion whose source is gone stays for good.
    if (ps->detonate_pending) {
      if (is_exploding(p)) return false;
      continue;
    }
    if (!p.alive() && !p.mech && !is_corpse(board_, p)) return false;
  }
  return true;
}

int64_t Simulation::next_event(int64_t f) const {
  int64_t best = std::numeric_limits<int64_t>::max();
  auto consider = [&](int64_t at) {
    if (at > f) best = std::min(best, at);
  };
  if (crit_pending_) consider(f + 1);
  for (const WeaponAnim& a : anims_) consider(a.end_frame);
  for (const TileAnim& t : tile_anims_) consider(t.end_frame);
  for (const TileAnim& h : holes_) consider(h.end_frame);
  for (const StackEntry& e : stack_) {
    if (e.delay > 0.0f) {
      consider(f + clock_.countdown_updates(e.delay));
    } else if (e.delay != kFullDelay && e.delay != kProjDelay) {
      consider(f + 1);
    }
  }
  pawns_.for_each([&](const PawnSim& ps) {
    if (ps.tracker.running) consider(ps.tracker.end_frame);
    if (ps.walking) consider(ps.walk_next);
    if (ps.flying) consider(ps.flight_end);
    if (ps.falling) consider(ps.fall_end);
    if (ps.teleport_phase) consider(ps.teleport_end);
    if (ps.burrow_phase) consider(ps.burrow_end);
    if (ps.dead && ps.death_anim && ps.frozen_updates < 0) {
      consider(ps.death_frame + ps.death_anim->total_updates());
    }
    if (ps.xp_gone >= 0) consider(ps.xp_gone);
  });
  return best == std::numeric_limits<int64_t>::max() ? -1 : best;
}

void Simulation::skip_frames(int64_t count) {
  if (count <= 0) return;
  // Idle frames: every positive countdown on the stack is decremented once
  // per frame (nothing fires, so the scan passes every entry).
  for (StackEntry& e : stack_) {
    if (!(e.delay > 0.0f)) continue;
    for (int64_t i = 0; i < count; ++i) e.delay = e.delay - clock_.step;
  }
}

int64_t Simulation::first_update(Phase updater) const {
  return phase_ < updater ? frame_ : frame_ + 1;
}

int64_t Simulation::first_pawn_update(int32_t uid) const {
  if (phase_ < Phase::P3) return frame_;
  if (phase_ > Phase::P3) return frame_ + 1;
  const bool done = std::find(p3_done_.begin(), p3_done_.end(), uid) != p3_done_.end();
  return done ? frame_ + 1 : frame_;
}

// ---- Busy state ------------------------------------------------------------------------

int Simulation::death_updates(const PawnSim& ps) const {
  if (!ps.death_anim) return 0;
  if (ps.frozen_updates >= 0) return ps.frozen_updates;
  // One update per render pass (P6) from the death frame on.
  const int64_t n = frame_ - ps.death_frame + (phase_ == Phase::P6 ? 1 : 0);
  return static_cast<int>(std::clamp<int64_t>(n, 0, ps.death_anim->total_updates()));
}

bool Simulation::death_anim_playing(const PawnSim& ps) const {
  if (!ps.dead || !ps.death_anim || ps.frozen_updates >= 0) return false;
  return death_updates(ps) < ps.death_anim->total_updates();
}

bool Simulation::pawn_busy(const Pawn& pawn, bool ignore_push) const {
  const PawnSim* ps = find_state(pawn.uid);
  if (!ps) return false;
  if (ps->tracker.running && !(ignore_push && !ps->tracker.has_melee)) return true;
  if (ps->walking || ps->flying || ps->falling || ps->teleport_phase || ps->burrow_phase) return true;
  // Enemy phase: a dying body is busy while its death animation plays.
  if (!board_.player_phase && !pawn.alive() && death_anim_playing(*ps) && pawn.pos.valid() &&
      !is_corpse(board_, pawn)) {
    return true;
  }
  return false;
}

bool Simulation::hole_ready(Point p) const {
  for (const TileAnim& h : holes_) {
    if (h.point == p) return h.end_frame <= frame_ && phase_ == Phase::P2;
  }
  return false;
}

// Board::GetBusyState, first match wins: 8 weapon animation, 2 tile, 1 pawn,
// 9 death effect pending, 6 stacked effects.
int Simulation::busy_state() const {
  if (!anims_.empty()) return 8;
  if (!tile_anims_.empty() || !holes_.empty()) return 2;
  for (const Pawn& p : board_.pawns()) {
    if (pawn_busy(p, false)) return 1;
  }
  bool fx_pending = false;
  pawns_.for_each([&](const PawnSim& ps) {
    if (ps.death_fx_pending && board_.find_pawn(ps.uid)) fx_pending = true;
  });
  if (fx_pending) return 9;
  if (!stack_.empty()) return 6;
  return 0;
}

bool Simulation::effect_active() const {
  const int s = busy_state();
  return s == 1 || s == 2 || s == 6 || s == 8;
}

// ---- Board::ApplyEffect -------------------------------------------------------------------

void Simulation::apply_effect(const SkillEffect& effect, Symbol shot, int cause, ActionKind kind) {
  // The last-shot record, unless this is a death/explosion follow-up
  // (EventSystem::SetLastEffect @00708240: an effect without an owner, -1,
  // leaves a recorded shooter in place).
  if (!effect.follow_up && !(effect.owner == -1 && last_owner_ != -1)) {
    // The same owner, team and name keep the record as it is (the rest of a
    // delayed effect): its HitSomething flag and its Enrage point stay. A new
    // record starts with no hit.
    if (effect.owner != last_owner_ || effect.team != last_team_ || shot != last_shot_) {
      rules_.shot_hit = false;
      last_point_ = effect.target;
      // Board::ApplyEffect: a TC_Enrage shot records the first entry's tile
      // that holds a pawn (the enraged unit), for Void Shocker (the only
      // reader, so skipped without it).
      if (board_.has_passive(kPassiveVoidShock) &&
          symbol_name(shot).find("TC_Enrage") != std::string_view::npos) {
        for (const SpaceDamage& sd : effect.effect) {
          if (sd.loc.valid() && has_pawn(board_, sd.loc)) {
            last_point_ = sd.loc;
            break;
          }
        }
      }
    }
    last_owner_ = effect.owner;
    last_team_ = effect.team;
    last_shot_ = shot;
  }
  rules_.current_shot = last_shot_;
  changed_ = true;

  std::vector<Point> touched;
  for (size_t i = 0; i < effect.effect.size(); ++i) {
    const SpaceDamage& sd = effect.effect[i];
    // Tiles hit now (flights land later and are tracked as impacts).
    const bool now = sd.projectile == ProjectileKind::None || sd.projectile == ProjectileKind::Laser;
    if (sd.loc.valid() && now) touched.push_back(sd.loc);
    if (sd.is_melee()) {
      if (Pawn* attacker = board_pawn(board_, sd.path.front())) {
        start_melee(*attacker, sd, cause);
      } else {
        apply_hit(sd, cause);  // no attacker: the hit lands at once
      }
    } else if (sd.projectile != ProjectileKind::None) {
      launch(sd, cause);
    } else {
      apply_hit(sd, cause);
    }
    if (sd.is_movement()) start_movement(effect, sd);

    // A delay applies after its own entry: the rest waits on the stack.
    if (sd.delay != kNoDelay) {
      SkillEffect rest;
      rest.effect.assign(effect.effect.begin() + static_cast<std::ptrdiff_t>(i) + 1,
                         effect.effect.end());
      rest.origin = effect.origin;
      rest.target = effect.target;
      rest.owner = effect.owner;
      rest.team = effect.team;
      rest.follow_up = effect.follow_up;
      push_front(std::move(rest), sd.delay, shot, cause);
      break;
    }
  }
  actions_.push_back(Action{kind, frame_, std::move(touched), cause, cause, false});
}

void Simulation::apply_hit(const SpaceDamage& sd, int cause) {
  rules_.pushes.clear();
  register_tile_anim(sd);
  apply_space_damage(board_, sd, rules_);
  changed_ = true;
  note_deaths();
  register_holes();
  // Pushes start once the hit is done, in the order the tile recorded them.
  const std::vector<PendingPush> pushes = std::move(rules_.pushes);
  rules_.pushes.clear();
  for (const PendingPush& push : pushes) start_push(push.uid, push.dir, cause);
}

void Simulation::register_tile_anim(const SpaceDamage& sd) {
  if (!sd.loc.valid() || sd.animation == kNoSymbol || !(sd.anim_flags & kAnimDelay)) return;
  const AnimTimeline* t = timeline(sd.animation);
  // Unknown animations finish at once; looping ones are never waited for.
  if (!t || t->total_updates() <= 0) return;
  const int64_t first = first_update(Phase::P2);
  tile_anims_.push_back(TileAnim{sd.loc, first + t->total_updates() - 1});
}

void Simulation::register_holes() {
  for (int i = 0; i < kTileCount; ++i) {
    const Point p = Point::from_index(i);
    if (!board_.tile(p).pending_hole) continue;
    const bool known = std::any_of(holes_.begin(), holes_.end(),
                                   [&](const TileAnim& h) { return h.point == p; });
    if (known) continue;
    const int n = clock_.eased_tracker_updates(dur_.hole, dur_.hole_ease, dur_.hole_ease);
    holes_.push_back(TileAnim{p, first_update(Phase::P2) + n - 1});
    changed_ = true;
  }
}

void Simulation::push_front(SkillEffect effect, float delay, Symbol shot, int cause) {
  stack_.insert(stack_.begin(), StackEntry{std::move(effect), delay, shot, cause});
  changed_ = true;
}

void Simulation::push_back(SkillEffect effect, float delay, Symbol shot, int cause) {
  stack_.push_back(StackEntry{std::move(effect), delay, shot, cause});
  changed_ = true;
}

// ---- Weapon animations ------------------------------------------------------------------------

void Simulation::launch(const SpaceDamage& sd, int cause) {
  WeaponAnim a;
  a.kind = sd.projectile;
  a.hit = sd;
  a.cause = cause;
  const int64_t first = first_update(Phase::P1);
  const Point from = sd.projectile_source;
  int n = 1;
  switch (sd.projectile) {
    case ProjectileKind::Artillery: {
      // "fireball" arcs start 0.25 to the left; the target shifts with them.
      const bool fireball = !art_has(sd.art, "missile") && art_has(sd.art, "fireball");
      n = arc_updates(clock_, from, sd.loc, dur_.y_velocity, dur_.gravity, dur_.arc_height,
                      fireball ? -0.25f : 0.0f);
      a.applies = true;
      break;
    }
    case ProjectileKind::Projectile: {
      float speed = dur_.x_velocity;
      if (art_has(sd.art, "shot_bigone")) speed = speed / 3.0f;
      if (art_has(sd.art, "shot_pierce")) speed = speed * 1.5f;
      n = projectile_updates(clock_, from, sd.loc, speed);
      a.applies = true;
      break;
    }
    case ProjectileKind::Laser:
      // The beam hits at once; the animation only keeps the board busy.
      apply_hit(sd, cause);
      n = clock_.tracker_updates(dur_.laser);
      break;
    case ProjectileKind::Pylon:
      n = clock_.tracker_updates(dur_.dropper);
      a.applies = true;
      break;
    case ProjectileKind::AirStrike:
    case ProjectileKind::ReverseAirStrike:
      n = clock_.tracker_updates(dur_.air_strike);
      break;
    case ProjectileKind::None:
      return;
  }
  a.end_frame = first + n - 1;
  anims_.push_back(std::move(a));
  changed_ = true;
}

void Simulation::update_weapon_anims() {
  for (size_t i = 0; i < anims_.size();) {
    if (anims_[i].end_frame > frame_) {
      ++i;
      continue;
    }
    WeaponAnim a = std::move(anims_[i]);
    anims_.erase(anims_.begin() + static_cast<std::ptrdiff_t>(i));
    changed_ = true;
    if (a.applies) {
      log(ResolveEventType::Impact, a.hit.loc);
      std::vector<Point> tiles;
      if (a.hit.loc.valid()) tiles.push_back(a.hit.loc);
      actions_.push_back(Action{ActionKind::Impact, frame_, std::move(tiles), a.cause, a.cause, false});
      apply_hit(a.hit, a.cause);
    }
  }
}

// ---- Tiles (P2) --------------------------------------------------------------------------------

// The 64 tiles in x-major order. Most tiles are quiet: nothing to prune,
// no body to drop, and settle_tile_frame would change nothing
// (detail::settle_tile_noop; for a tile with no pawn its state alone
// decides, webs included). A quiet tile changes no pawn, so the
// note_deaths after it is a no-op once one has run since the last change.
// Tiles that may need work are found with a mask (pawns, webs, tile states,
// finished animations), recomputed after every tile that changed something,
// so it always describes the board as it is; the others are skipped.
// Tiles whose state alone may make settle_tile_frame act
// (!detail::inert_tile_state), recomputed only when some tile changed since
// the last call (the tiles are compared bytewise: Tile has no padding).
uint64_t Simulation::busy_tile_states() {
  const Tile* tiles = &board_.tile(Point::from_index(0));
  if (busy_tiles_valid_ && std::memcmp(tiles, busy_tiles_at_.data(), sizeof(busy_tiles_at_)) == 0) {
    return busy_tiles_mask_;
  }
  std::memcpy(busy_tiles_at_.data(), tiles, sizeof(busy_tiles_at_));
  busy_tiles_valid_ = true;
  busy_tiles_mask_ = 0;
  for (int i = 0; i < kTileCount; ++i) {
    if (!detail::inert_tile_state(tiles[i])) busy_tiles_mask_ |= uint64_t{1} << i;
  }
  return busy_tiles_mask_;
}

void Simulation::update_tiles() {
  uint64_t occupied = 0;  // bit i: some pawn (fallen ones included) is on tile i
  uint64_t hot = 0;       // bit i: a pawn (not fallen) on tile i is a mech or on fire
  bool bodies = false;    // some dead non-mech pawn was seen dead (removable needs it)
  uint64_t work = 0;      // tiles that may not be quiet
  auto scan = [&] {
    occupied = 0;
    hot = 0;
    bodies = false;
    for (const Pawn& pawn : board_.pawns()) {
      if (pawn.pos.valid()) {
        // A multi-tile pawn is listed on its extra tile as well.
        uint64_t bits = uint64_t{1} << pawn.pos.index();
        if (const Point e = pawn.extra_tile(); e.valid()) bits |= uint64_t{1} << e.index();
        occupied |= bits;
        if (!pawn.fallen && (pawn.mech || pawn.fire)) hot |= bits;
      }
      if (!pawn.mech && !pawn.alive()) {
        const PawnSim* ps = find_state(pawn.uid);
        bodies = bodies || (ps && ps->dead);
      }
    }
  };
  auto plan = [&] {
    work = occupied | busy_tile_states();
    for (const TileAnim& a : tile_anims_) {
      if (a.end_frame <= frame_ && a.point.valid()) work |= uint64_t{1} << a.point.index();
    }
    for (const TileAnim& h : holes_) {
      if (h.end_frame <= frame_ && h.point.valid()) work |= uint64_t{1} << h.point.index();
    }
  };
  scan();
  plan();
  bool noted = false;  // note_deaths ran and nothing changed since
  std::vector<int32_t> gone;
  for (int i = 0;;) {
    const uint64_t ahead = i < kTileCount ? work & (~uint64_t{0} << i) : 0;
    const int next = ahead ? std::countr_zero(ahead) : kTileCount;
    if (next > i && !noted) {
      // Tile i is quiet: its note_deaths is the one that counts.
      note_deaths();
      noted = true;
      scan();
      continue;
    }
    if (next >= kTileCount) break;
    i = next;
    const Point p = Point::from_index(i);
    // A deferred chasm whose bounce was never seen starting (e.g. it was
    // pending when resolution began) starts now.
    if (board_.tile(p).pending_hole) register_holes();
    // Animations that finish this frame are pruned before the tile's rules.
    for (size_t k = 0; k < tile_anims_.size();) {
      if (tile_anims_[k].point == p && tile_anims_[k].end_frame <= frame_) {
        tile_anim_ends_.push_back(frame_);
        tile_anims_.erase(tile_anims_.begin() + static_cast<std::ptrdiff_t>(k));
        changed_ = true;
      } else {
        ++k;
      }
    }
    // The tile drops bodies that are done (BoardSpace::OnLoop removes them
    // from its occupant list before its pawn rules; the board list follows).
    bool acted = false;
    if (bodies && (occupied >> i & 1)) {
      gone.clear();
      for (const Pawn& pawn : board_.pawns()) {
        if (!pawn.occupies(p)) continue;
        if (const PawnSim* ps = find_state(pawn.uid); ps && removable(pawn, *ps)) gone.push_back(pawn.uid);
      }
      for (int32_t uid : gone) remove(uid);
      acted = !gone.empty();
    }
    bool quiet;
    if (hot >> i & 1) {
      quiet = detail::settle_tile_noop(board_, p);
    } else if (occupied >> i & 1) {
      quiet = detail::quiet_occupied_tile_state(board_.tile(p));
    } else {
      quiet = detail::inert_tile_state(board_.tile(p));
    }
    if (acted || !quiet) {
      settle_tile_frame(board_, p, rules_);
      acted = true;
    }
    for (size_t k = 0; k < holes_.size();) {
      if (holes_[k].point == p && holes_[k].end_frame <= frame_) {
        holes_.erase(holes_.begin() + static_cast<std::ptrdiff_t>(k));
        changed_ = true;
      } else {
        ++k;
      }
    }
    if (acted || !noted) {
      note_deaths();
      noted = true;
      scan();
      if (acted) plan();
    }
    ++i;
  }
}

// ---- Pawns (P3) ---------------------------------------------------------------------------------

void Simulation::update_pawn(int32_t uid) {
  Pawn* p = board_.find_pawn(uid);
  if (!p) return;
  PawnSim& ps = state(uid);
  if (removable(*p, ps)) {
    remove(uid);
    return;
  }
  p3_done_.push_back(uid);
  settle_pawn_frame(board_, *p, rules_);

  // The push / lunge tracker.
  if (ps.tracker.running && ps.tracker.end_frame <= frame_) {
    ps.tracker.running = false;
    changed_ = true;
    if (ps.tracker.has_melee) {
      // The lunge (or a push that replaced it) ends: the stored hit lands.
      const SpaceDamage hit = ps.tracker.melee;
      ps.tracker.has_melee = false;
      log(ResolveEventType::LungeHit, hit.loc, uid);
      apply_hit(hit, ps.tracker.cause);
    } else {
      finish_push(*p, ps);
    }
    p = board_.find_pawn(uid);
    if (!p) return;
  }

  // The fall animation ends: KillInstant, off the board.
  if (ps.falling && ps.fall_end <= frame_) {
    ps.falling = false;
    changed_ = true;
    finish_fall(board_, *p, rules_);
    note_deaths();
  }

  if (ps.death_pending && !ps.tracker.running) process_death(*p, ps);
  if (ps.death_fx_pending && !effect_active()) {
    trigger_death_effect(*p, ps);
    p = board_.find_pawn(uid);
    if (!p) return;
  }

  if (ps.walking && ps.walk_next <= frame_) {
    walk_step(*p, ps);
    p = board_.find_pawn(uid);
    if (!p) return;
  }

  // Own leap/charge lands: a chasm makes it fall; the rest waits for the
  // tile rules.
  if (ps.flying && ps.flight_end <= frame_) {
    ps.flying = false;
    changed_ = true;
    if (p->pos.valid() && board_.tile(p->pos).is_chasm()) fall_pawn(board_, *p, rules_);
    note_deaths();
  }

  if (ps.teleport_phase && ps.teleport_end <= frame_) {
    changed_ = true;
    if (ps.teleport_phase == 1) {
      // Out-animation over: leave the webs, relocate, start appearing.
      if (p->pos.valid()) detail::release_webs(board_, p->pos);
      shift_queued_shot(*p, ps.teleport_to - p->pos);
      set_space(board_, *p, ps.teleport_to, /*no_injury=*/true, &rules_);
      note_deaths();
      ps.teleport_phase = 2;
      ps.teleport_end = frame_ + clock_.tracker_updates(dur_.teleport_in);
    } else {
      ps.teleport_phase = 0;
      if (p->pos.valid()) check_terrain_dangers(board_, p->pos, rules_);
      note_deaths();
      add_delay(dur_.teleport_pause);
    }
    p = board_.find_pawn(uid);
    if (!p) return;
  }

  if (ps.burrow_phase && ps.burrow_end <= frame_) {
    changed_ = true;
    if (ps.burrow_phase == 1 && !ps.burrow_to.valid()) {
      // A hurt burrower's dive ends: SetSpace(-1, -1). It is underground
      // until the AI moves it (Pawn::GetMoveOrigin: from prev_pos). A pawn
      // that died while diving stays where it died [I].
      ps.burrow_phase = 0;
      if (p->alive() && p->pos.valid()) {
        const Point from = p->pos;
        p->queued = QueuedShot{};
        p->fire = false;
        set_space(board_, *p, kInvalidPoint, /*no_injury=*/true, &rules_);
        log(ResolveEventType::PawnUnderground, from, uid);
      }
    } else if (ps.burrow_phase == 1) {
      set_space(board_, *p, ps.burrow_to, /*no_injury=*/true, &rules_);
      ps.burrow_phase = 2;
      ps.burrow_end = frame_ + clock_.tracker_updates(dur_.burrow_emerge);
    } else {
      ps.burrow_phase = 0;
      if (p->pos.valid()) check_terrain_dangers(board_, p->pos, rules_);
    }
    note_deaths();
    p = board_.find_pawn(uid);
    if (!p) return;
  }

  cancel_queued_shot(*p);
}

// Pawn::OnLoop's last step: a queued attack is dropped by smoke, death,
// freezing or water.
void Simulation::cancel_queued_shot(Pawn& pawn) {
  if (!pawn.queued.active()) return;
  const bool busy = pawn_busy(pawn, false);
  const Tile* t = pawn.pos.valid() ? &board_.tile(pawn.pos) : nullptr;
  const bool smoked = t && t->smoke && !busy && !pawn.ignore_smoke &&
                      !pawn.has_pilot(kPilotDisableImmunity);
  const bool submerged = t && t->terrain == Terrain::Water && !busy && !is_flying(pawn);
  if (smoked || !pawn.alive() || pawn.frozen || submerged) {
    pawn.queued = QueuedShot{};
    changed_ = true;
  }
}

// ---- Movement entries ---------------------------------------------------------------------------

void Simulation::start_movement(const SkillEffect& effect, const SpaceDamage& sd) {
  // GetPawn(start, false): a body that died earlier in this effect (still on
  // its tile, its death not yet processed) is moved like a living pawn (live
  // 2026-10-12 m16: a Burnbug's grapple kills a BombRock and drags its body
  // one tile, where it then explodes).
  Pawn* pawn = listed_pawn(board_, sd.path.front());
  if (!pawn) return;  // nobody at the path start: the game only warns
  PawnSim& ps = state(pawn->uid);
  const bool forced = sd.path.front() == effect.origin;
  const Point to = sd.path.back();
  changed_ = true;
  switch (sd.move_kind) {
    case MoveKind::Burrow: {
      shift_queued_shot(*pawn, to - pawn->pos);
      set_pawn_fire(board_, *pawn, false);
      if (!pawn->pos.valid()) {
        set_space(board_, *pawn, to, false, &rules_);
        ps.burrow_phase = 2;
        ps.burrow_to = to;
        ps.burrow_end = first_pawn_update(pawn->uid) + clock_.tracker_updates(dur_.burrow_emerge) - 1;
      } else {
        ps.burrow_phase = 1;
        ps.burrow_to = to;
        ps.burrow_end = first_pawn_update(pawn->uid) + clock_.tracker_updates(dur_.burrow_dive) - 1;
      }
      break;
    }
    case MoveKind::Leap:
    case MoveKind::Charge: {
      if (!forced && !pawn->pushable) return;
      const Point from = pawn->pos;
      // Relocated before the flight starts: later entries see it there.
      shift_queued_shot(*pawn, to - from);
      set_space(board_, *pawn, to, pawn_busy(*pawn, false), &rules_);
      note_deaths();
      const int n = sd.move_kind == MoveKind::Leap
                        ? arc_updates(clock_, from, to, dur_.y_velocity, dur_.gravity, dur_.arc_height)
                        : projectile_updates(clock_, from, to, dur_.x_velocity);
      ps.flying = true;
      ps.flight_end = first_pawn_update(pawn->uid) + n - 1;
      break;
    }
    case MoveKind::Teleport:
      ps.teleport_phase = 1;
      ps.teleport_to = to;
      ps.teleport_end = first_pawn_update(pawn->uid) + clock_.tracker_updates(dur_.teleport_out) - 1;
      break;
    case MoveKind::Walk:
    case MoveKind::Melee: {
      if (!forced && !pawn->pushable) return;
      ps.walk_queue.assign(sd.path.begin(), sd.path.end());
      if (!ps.walk_queue.empty() && ps.walk_queue.front() == pawn->pos) {
        ps.walk_queue.erase(ps.walk_queue.begin());
      }
      if (ps.walk_queue.empty()) return;
      ps.walking = true;
      walk_step(*pawn, ps);  // the first step is taken at once
      break;
    }
  }
}

// Pawn::Burrow(-1, -1, forced) from ModifyHealth (stage 2 H4): the queued
// shot and the fire are already gone; the dive animation starts and the pawn
// stays on its tile (hit, blocking, counted) until it ends. A new hit during
// the dive starts it again. An underground pawn stays underground.
void Simulation::start_dive(Pawn& pawn) {
  if (!pawn.pos.valid()) return;
  PawnSim& ps = state(pawn.uid);
  ps.burrow_phase = 1;
  ps.burrow_to = kInvalidPoint;
  ps.burrow_end = first_pawn_update(pawn.uid) + clock_.tracker_updates(dur_.burrow_dive) - 1;
  changed_ = true;
}

// EventSystem::AddEvent(17) from PowerBar::Damage: pending until the next
// frame starts (EventSystem::OnLoop), current for that frame only.
void Simulation::grid_critical() {
  crit_pending_ = true;
  changed_ = true;
}

// Pawn::UpdatePath / Pawn::Move: one step, then the step timer restarts
// unless the path is done and the walker is a player unit.
void Simulation::walk_step(Pawn& pawn, PawnSim& ps) {
  changed_ = true;
  if (ps.walk_queue.empty()) {
    ps.walking = false;
    return;
  }
  const Point target = ps.walk_queue.front();
  ps.walk_queue.erase(ps.walk_queue.begin());
  const Point delta = target - pawn.pos;
  if (delta.x * delta.x + delta.y * delta.y > 1) {
    // Off the path: re-path from here and walk that instead.
    std::vector<Point> path = find_path(board_, pawn.pos, target, path_profile(board_, pawn));
    if (!path.empty()) path.erase(path.begin());
    ps.walk_queue = std::move(path);
    if (ps.walk_queue.empty()) {
      ps.walking = false;
      return;
    }
    walk_step(pawn, ps);
    return;
  }
  const bool last = ps.walk_queue.empty();
  // After the last step a player unit's step timer stops; anyone else's
  // runs once more (still busy, so the tile rules pick the dangers up later).
  const bool stops = last && pawn.team == Team::Player;
  if (stops) ps.walking = false;
  if (!last || final_step_allowed(board_, target)) {
    const int32_t uid = pawn.uid;
    shift_queued_shot(pawn, delta);
    // Injured costs 1 HP per step (spec stage 5 open question 2).
    set_space(board_, pawn, target, false, &rules_);
    if (last) check_terrain_dangers(board_, target, rules_);
    note_deaths();
    if (!board_.find_pawn(uid)) return;
  }
  if (stops) return;
  ps.walk_next = first_pawn_update(pawn.uid) + clock_.tracker_updates(dur_.walk_step) - 1;
  if (last) ps.walk_queue.clear();
}

void Simulation::start_melee(Pawn& attacker, const SpaceDamage& sd, int cause) {
  PawnSim& ps = state(attacker.uid);
  ps.tracker.running = true;
  ps.tracker.dir = direction_of(attacker.pos, sd.loc);
  ps.tracker.has_melee = true;
  ps.tracker.melee = sd;
  ps.tracker.from = attacker.pos;
  ps.tracker.cause = cause;
  ps.tracker.end_frame = first_pawn_update(attacker.uid) + clock_.tracker_updates(dur_.lunge) - 1;
  changed_ = true;
}

// ---- The stacked-effect queue (P4) -------------------------------------------------------------

void Simulation::update_stack() {
  const int state = busy_state();
  const bool proj_busy = state == 8;
  const bool busy = state == 1 || state == 2;
  if (stack_.empty()) return;
  for (size_t i = 0; i < stack_.size();) {
    StackEntry& e = stack_[i];
    bool fire = false;
    ActionKind kind = ActionKind::Chunk;
    if (e.delay > 0.0f) {
      e.delay = e.delay - clock_.step;
      fire = e.delay <= 0.0f;
      kind = ActionKind::DelayedChunk;
    } else if (e.delay == kFullDelay) {
      fire = !busy && !proj_busy;
      kind = ActionKind::GatedChunk;
    } else if (e.delay == kProjDelay) {
      fire = !proj_busy;
      kind = ActionKind::ProjChunk;
    } else {
      fire = true;
    }
    if (!fire) {
      ++i;
      continue;
    }
    StackEntry entry = std::move(stack_[i]);
    stack_.erase(stack_.begin() + static_cast<std::ptrdiff_t>(i));
    changed_ = true;
    // Empty entries are dropped without using up the frame.
    if (entry.effect.effect.empty()) continue;
    if (kind == ActionKind::GatedChunk) {
      // The gate opened now: was it a push and an animation ending together?
      int64_t push_end = -1;
      bool hazard = false;
      for (const Action& a : actions_) {
        if (a.kind == ActionKind::PushEnd && a.frame >= frame_ - 1) {
          push_end = std::max(push_end, a.frame);
          hazard = hazard || a.hazard;
        }
      }
      int64_t anim_end = -1;
      for (int64_t f : tile_anim_ends_) {
        if (f >= frame_ - 1) anim_end = std::max(anim_end, f);
      }
      if (hazard && push_end >= 0 && anim_end >= 0 && std::max(push_end, anim_end) == frame_) {
        flag(TimingKind::GateVsAnimation, entry.effect.origin);
      }
    }
    log(ResolveEventType::ChunkApplied, entry.effect.origin, entry.effect.owner);
    apply_effect(entry.effect, entry.shot, new_cause(), kind);
    return;
  }
}

// ---- Helpers --------------------------------------------------------------------------------

PawnSim& Simulation::state(int32_t uid) {
  return pawns_.get(uid);
}

const PawnSim* Simulation::find_state(int32_t uid) const {
  return pawns_.find(uid);
}

const AnimTimeline* Simulation::timeline(Symbol anim) {
  if (anim == kNoSymbol) return nullptr;
  std::map<Symbol, AnimTimeline>& cache = timelines_->anims;
  if (auto it = cache.find(anim); it != cache.end()) {
    return it->second.valid() ? &it->second : nullptr;
  }
  const AnimDef* def = ctx_.data ? ctx_.data->animation(anim) : nullptr;
  AnimTimeline t = def ? AnimTimeline(clock_, *def) : AnimTimeline();
  auto& stored = cache.emplace(anim, std::move(t)).first->second;
  return stored.valid() ? &stored : nullptr;
}

// The pawn's death animation ("<Image>d"), with the fade-out
// Pawn::SetupAnimation gives it (corpses, which skip it, never use the
// timeline's end). Pawn types the game data does not know get the default;
// known types without one have none.
const AnimTimeline* Simulation::death_timeline(const Pawn& pawn) {
  const PawnDef* def = ctx_.data ? ctx_.data->pawn(pawn.type) : nullptr;
  if (!def) return &timelines_->default_death;
  std::map<Symbol, AnimTimeline>& cache = timelines_->deaths;
  if (auto it = cache.find(pawn.type); it != cache.end()) {
    return it->second.valid() ? &it->second : nullptr;
  }
  const AnimDef* anim = ctx_.data->animation(def->image + "d");
  AnimTimeline t = anim ? AnimTimeline(clock_, *anim, dur_.death_fade_out) : AnimTimeline();
  auto& stored = cache.emplace(pawn.type, std::move(t)).first->second;
  return stored.valid() && stored.total_updates() > 0 ? &stored : nullptr;
}

void Simulation::log(ResolveEventType type, Point p, int32_t uid, int amount) {
  if (ctx_.log) ctx_.log->push_back(ResolveEvent{type, frame_, p, uid, amount});
}

void Simulation::flag(TimingKind kind, Point p, int32_t uid) {
  if (!result_) return;
  for (const TimingFlag& t : result_->timing) {
    if (t.kind == kind && t.frame == frame_ && t.point == p && t.uid == uid) return;
  }
  result_->timing.push_back(TimingFlag{kind, frame_, p, uid});
  log(ResolveEventType::TimingSensitive, p, uid, static_cast<int>(kind));
}

// Frame-rate sensitivity between independent clocks: a push ending within a
// frame of a delayed chunk or an impact that touches the same tiles could
// land on the other side of it at another frame rate.
void Simulation::analyze_timing() {
  for (const Action& push : actions_) {
    if (push.kind != ActionKind::PushEnd) continue;
    for (const Action& other : actions_) {
      if (other.kind != ActionKind::DelayedChunk && other.kind != ActionKind::Impact) continue;
      if (other.cause == push.origin) continue;  // the hit that started this push
      if (std::abs(other.frame - push.frame) > 1) continue;
      for (Point t : other.tiles) {
        if (std::find(push.tiles.begin(), push.tiles.end(), t) == push.tiles.end()) continue;
        const int64_t saved = frame_;
        frame_ = other.frame;
        flag(other.kind == ActionKind::Impact ? TimingKind::ImpactVsPush : TimingKind::DelayVsPush, t);
        frame_ = saved;
        break;
      }
    }
  }
  actions_.clear();
  tile_anim_ends_.clear();
}

}  // namespace detail
}  // namespace itb
