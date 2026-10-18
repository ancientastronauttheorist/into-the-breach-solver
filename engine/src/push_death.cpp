// Stage 3: pushes (Pawn::Push, FinishPush, Board::IsPushBlocked, PushDamage)
// and the death pipeline (ProcessDeath, TriggerDeathEffect, DetonateCorpse,
// psion leaders, removal of bodies).

#include <algorithm>
#include <bit>
#include <string>

#include "itb/game_data.hpp"
#include "itb/movement.hpp"
#include "itb/pilot_xp.hpp"
#include "rules_detail.hpp"
#include "simulation.hpp"

namespace itb::detail {
namespace {

// The tile's occupant list as the push obstacle test sees it: every pawn
// there (dying and fallen bodies included), in arrival order.
const Pawn* first_listed(const Board& board, Point p) {
  const Pawn* first = nullptr;
  for (const Pawn& pawn : board.pawns()) {
    if (pawn.occupies(p) && (!first || pawn.arrival < first->arrival)) first = &pawn;
  }
  return first;
}

// Pawn::FlipQueued: the attack is mirrored through its origin.
void flip_queued(Pawn& pawn) {
  if (!pawn.queued.active()) return;
  const Point origin = pawn.queued.origin.valid() ? pawn.queued.origin : pawn.pos;
  pawn.queued.target = origin * 2 - pawn.queued.target;
}

SpaceDamage explosion_hit(Point p, std::string_view anim) {
  SpaceDamage sd = space_damage(p, 1);
  sd.animation = intern(anim);
  sd.mode_override = kModeExplosion;
  return sd;
}

}  // namespace

// ---- Pushes ---------------------------------------------------------------------

// Pawn::Push.
void Simulation::start_push(int32_t uid, Dir dir, int cause) {
  Pawn* pawn = board_.find_pawn(uid);
  if (!pawn) return;
  if (dir == Dir::Flip) {
    // DIR_FLIP only mirrors the queued attack of a living, flippable pawn.
    if (!pawn->ignore_flip && pawn->alive()) {
      flip_queued(*pawn);
      changed_ = true;
    }
    return;
  }
  if (!pawn->pushable || !is_cardinal(dir)) return;
  // A push towards the edge does nothing at all (it is never "blocked").
  if (!step(pawn->pos, dir).valid()) return;
  // Dead, frozen and massive pawns are pushed too. A running push or lunge
  // is restarted with the new direction.
  PawnSim& ps = state(uid);
  ps.tracker.running = true;
  ps.tracker.dir = dir;
  ps.tracker.from = pawn->pos;
  ps.tracker.cause = cause;
  ps.tracker.end_frame = first_pawn_update(uid) + clock_.tracker_updates(dur_.push) - 1;
  changed_ = true;
  log(ResolveEventType::PushStarted, pawn->pos, uid, static_cast<int>(dir));
}

// Board::IsPushBlocked(p, d): terrain, the tile's first occupant, then the
// wall on the edge the pawn would enter through.
bool Simulation::push_blocked(Point p, Dir dir, int32_t pusher) {
  if (!p.valid()) return false;
  const Tile& t = board_.tile(p);
  if (t.is_building() || t.is_mountain()) return true;
  if (const Pawn* first = first_listed(board_, p)) {
    if (first->alive() || is_corpse(board_, *first)) return true;
    // A dying body blocks while its death animation is under half done
    // (frozen where KillInstant stopped it).
    const PawnSim& ps = state(first->uid);
    if (ps.death_anim) {
      const int n = death_updates(ps);
      const bool blocked = ps.death_anim->progress(n) < 0.5f;
      if (ps.frozen_updates < 0 && death_anim_playing(ps)) {
        const bool earlier = n > 0 && ps.death_anim->progress(n - 1) < 0.5f;
        const bool later = ps.death_anim->progress(n + 1) < 0.5f;
        if (earlier != blocked || later != blocked) flag(TimingKind::DyingBlocker, p, pusher);
      }
      if (blocked) return true;
    }
  }
  return t.has_wall(opposite(dir));
}

// BoardSpace::PushDamage: the obstacle tile takes a 1-damage bump (pawns,
// buildings with their grid roll, mountains), then the wall between the two
// tiles is knocked down.
void Simulation::push_damage(Point target, Point from, Dir dir) {
  rules_.pushes.clear();
  damage_tile(board_, target, 1, DamageMode::Push, rules_);
  rules_.pushes.clear();
  note_deaths();
  register_holes();
  Tile& a = board_.tile(from);
  Tile& b = board_.tile(target);
  if (a.has_wall(dir) || b.has_wall(opposite(dir))) {
    a.walls = static_cast<uint8_t>(a.walls & ~(1u << static_cast<int>(dir)));
    b.walls = static_cast<uint8_t>(b.walls & ~(1u << static_cast<int>(opposite(dir))));
  }
}

// Pawn::FinishPush, on the frame the tracker passes its length.
void Simulation::finish_push(Pawn& pawn, PawnSim& ps) {
  const int32_t uid = pawn.uid;
  const Dir dir = ps.tracker.dir;
  const Point from = pawn.pos;
  const Point target = step(from, dir);
  Action rec{ActionKind::PushEnd, frame_, {from, target}, new_cause(), ps.tracker.cause, false};
  if (push_blocked(target, dir, uid)) {
    log(ResolveEventType::PushBlocked, target, uid);
    // The pushed pawn takes its bump first (dead non-mechs ignore it), then
    // the obstacle.
    damage_pawn(board_, pawn, 1, DamageMode::Push, rules_);
    note_deaths();
    push_damage(target, from, dir);
  } else {
    // Pawn::MoveDir: relocate (Injured applies: the push no longer makes it
    // busy), carry the queued attack along, then the new tile's dangers at once.
    set_space(board_, pawn, target, pawn_busy(pawn, false), &rules_);
    if (Pawn* p = board_.find_pawn(uid)) shift_queued_shot(*p, dir_vector(dir));
    note_deaths();
    check_terrain_dangers(board_, target, rules_);
    note_deaths();
    log(ResolveEventType::PushMoved, target, uid);
    if (const Pawn* p = board_.find_pawn(uid)) {
      const Tile& t = board_.tile(target);
      rec.hazard = t.on_fire() || t.acid || t.lava || t.spikes || t.smoke ||
                   (p->fire && t.terrain == Terrain::Water);
    }
  }
  actions_.push_back(std::move(rec));
  changed_ = true;
}

// ---- Deaths ---------------------------------------------------------------------------

void Simulation::note_deaths() {
  // After a pass every pawn has an entry with dead == !alive, and nothing
  // else changes `dead` or drops an entry without the pawn leaving the list.
  // So while the list (uids, alive) is the one of the last pass, a pass is a
  // no-op.
  const std::vector<Pawn>& list = board_.pawns();
  if (noted_.size() == list.size()) {
    bool same = true;
    for (size_t i = 0; i < list.size() && same; ++i) {
      same = noted_[i].first == list[i].uid && noted_[i].second == list[i].alive();
    }
    if (same) return;
  }
  for (const Pawn& p : list) {
    PawnSim& ps = state(p.uid);
    if (!ps.dead && !p.alive()) {
      note_death(p, ps);
    } else if (ps.dead && p.alive()) {
      // A mech corpse healed back to life.
      ps.dead = ps.death_pending = ps.death_fx_pending = ps.detonate_pending = false;
      ps.frozen_updates = -1;
      ps.death_anim = nullptr;
      ps.xp_gone = -1;
      changed_ = true;
    }
  }
  noted_.clear();
  for (const Pawn& p : board_.pawns()) noted_.emplace_back(p.uid, p.alive());
}

void Simulation::note_death(const Pawn& pawn, PawnSim& ps) {
  ps.dead = true;
  ps.death_pending = true;
  // The death animation advances in render passes from this frame on.
  ps.death_frame = frame_;
  ps.frozen_updates = -1;
  ps.death_anim = death_timeline(pawn);
  changed_ = true;
}

void Simulation::instant_kill(const Pawn& pawn) {
  PawnSim& ps = state(pawn.uid);
  if (!ps.dead && !pawn.alive()) note_death(pawn, ps);
  if (ps.dead && ps.death_anim && ps.frozen_updates < 0) {
    ps.frozen_updates = death_updates(ps);
    changed_ = true;
  }
}

bool Simulation::start_fall(Pawn& pawn) {
  PawnSim& ps = state(pawn.uid);
  // Pawn::Fall waits for a running push (it is asked again later); a fall
  // already under way is not restarted.
  if (ps.falling || (ps.tracker.running && !ps.tracker.has_melee)) return true;
  ps.falling = true;
  ps.fall_end = first_pawn_update(pawn.uid) + clock_.tracker_updates(dur_.fall) - 1;
  changed_ = true;
  return true;
}

// Pawn::ProcessDeath, once no push or lunge is running on the pawn.
void Simulation::process_death(Pawn& pawn, PawnSim& ps) {
  ps.death_pending = false;
  ps.death_fx_pending = true;
  if (pawn.team == Team::Player) set_pawn_fire(board_, pawn, false);
  // The XP popup over a dead enemy keeps the body on the board.
  if (pawn.team == Team::Enemy && !pawn.mech && !pawn.minor) {
    ps.xp_gone = frame_ + clock_.tracker_updates(dur_.xp_popup);
  }
  credit_kill(pawn);
  // EVENT_ACID_DESTROYED: ACID as the death is processed, wherever it came
  // from (this turn's shot, a pool, acid water), minor pawns included.
  if (pawn.team == Team::Enemy && !pawn.mech && pawn.acid) ++board_.acid_destroyed;
  changed_ = true;
  log(ResolveEventType::DeathProcessed, pawn.pos, pawn.uid);
}

// Pawn::IsExploding: Explodes / Burns, or the Blast, boss, spider or fire
// psion's mutation.
bool Simulation::is_exploding(const Pawn& pawn) const {
  return pawn.explodes || pawn.burns || mutation_affects(board_, pawn, Leader::Explode) ||
         mutation_affects(board_, pawn, Leader::Boss) ||
         mutation_affects(board_, pawn, Leader::Spider) ||
         mutation_affects(board_, pawn, Leader::Fire);
}

// Pawn::TriggerDeathEffect: [ACID pool, Fast Decay forest, Lua entries],
// appended to the stack as a FULL_DELAY follow-up effect (+0x60 set: the
// last-shot record, HitSomething flag included, stays as it is).
void Simulation::trigger_death_effect(Pawn& pawn, PawnSim& ps) {
  ps.death_fx_pending = false;
  const int32_t uid = pawn.uid;
  const Point tile = pawn.pos;
  SkillEffect fx;
  if (ctx_.death_effect) fx = ctx_.death_effect(owner_, pawn, tile);
  const Pawn* p = board_.find_pawn(uid);
  if (!p) return;
  std::vector<SpaceDamage> front;
  if (p->acid && !is_corpse(board_, *p)) {
    SpaceDamage acid = space_damage(tile);
    acid.acid = StatusChange::Apply;
    front.push_back(acid);
  }
  if (board_.has_passive(kPassiveFastDecay) && is_vek(*p) && tile.valid() &&
      board_.tile(tile).terrain != Terrain::Water && board_.tile(tile).terrain != Terrain::Hole) {
    SpaceDamage decay = space_damage(tile);
    decay.fire = StatusChange::Remove;
    decay.terrain = static_cast<int>(Terrain::Forest);
    front.push_back(decay);
  }
  fx.effect.insert(fx.effect.begin(), front.begin(), front.end());
  if (is_exploding(*p)) ps.detonate_pending = true;
  prepare_effect(fx, tile, tile, p->team, kNoSymbol);
  fx.owner = last_owner_ >= 0 ? last_owner_ : uid;
  fx.follow_up = true;
  push_back(std::move(fx), kFullDelay, last_shot_, new_cause());
  changed_ = true;
  log(ResolveEventType::DeathEffect, tile, uid);
}

// Pawn::DetonateCorpse (P5, no effect active): only if the explosion's source
// still holds. Otherwise the request stays pending, and the body with it.
void Simulation::detonate_corpse(Pawn& pawn, PawnSim& ps) {
  if (!ps.detonate_pending || !is_exploding(pawn)) return;
  const Point pos = pawn.pos;
  SkillEffect fx;
  const bool blast = pawn.explodes || mutation_affects(board_, pawn, Leader::Explode) ||
                     mutation_affects(board_, pawn, Leader::Boss);
  if (blast) {
    // Explosion mode (no armor/ACID, not a bump), the neighbours then the
    // pawn's own tile: left, down, right, up, self.
    fx.effect.push_back(explosion_hit(pos, "explo_fire1"));
    for (int d = 0; d < 4; ++d) {
      const Point q = step(pos, static_cast<Dir>(d));
      fx.effect.insert(fx.effect.begin(), explosion_hit(q, "exploout1_" + std::to_string(d)));
    }
  }
  if (mutation_affects(board_, pawn, Leader::Spider)) {
    // An egg on its own tile, or lobbed onto a free neighbour.
    static const Symbol egg = intern("SpiderlingEgg1");
    SpaceDamage spawn = space_damage(pos);
    spawn.spawn_pawn = egg;
    if (!is_blocked(board_, pos, Pathing::lua(PathProfile::Ground))) {
      fx.effect.push_back(spawn);
    } else {
      std::vector<Point> free;
      for (int dx = -1; dx <= 1; ++dx) {
        for (int dy = -1; dy <= 1; ++dy) {
          const Point q = pos + Point{dx, dy};
          if ((dx || dy) && q.valid() && !is_blocked(board_, q, Pathing::lua(PathProfile::Ground)) &&
              board_.tile(q).pod == PodState::None) {
            free.push_back(q);
          }
        }
      }
      if (!free.empty()) {
        int pick = ctx_.spider_egg ? ctx_.spider_egg(owner_, pawn, free) : 0;
        pick = std::clamp(pick, 0, static_cast<int>(free.size()) - 1);
        if (result_) {
          result_->chances.push_back(ChanceRecord{ChanceKind::SpiderEgg, frame_, free[static_cast<size_t>(pick)],
                                                  0, pick, static_cast<int>(free.size())});
        }
        spawn.loc = free[static_cast<size_t>(pick)];
        spawn.projectile = ProjectileKind::Artillery;
        spawn.art = intern("effects/shotup_spider.png");
        spawn.projectile_source = pos;
        spawn.delay = kFullDelay;
        fx.effect.push_back(spawn);
      }
    }
  }
  if ((mutation_affects(board_, pawn, Leader::Fire) || pawn.burns) && pos.valid() &&
      board_.tile(pos).terrain != Terrain::Water) {
    SpaceDamage burn = space_damage(pos);
    burn.fire = StatusChange::Apply;
    burn.animation = intern("explo_fire1");
    fx.effect.push_back(burn);
  }
  prepare_effect(fx, pos, pos, last_team_ != Team::None ? last_team_ : pawn.team, kNoSymbol);
  fx.owner = last_owner_ >= 0 ? last_owner_ : pawn.uid;
  // Unlike TriggerDeathEffect, DetonateCorpse never sets the follow-up flag
  // (+0x60) and its effect has no skill name. Board::ApplyEffect then records
  // it as a new last shot: the shooter's owner and team, an empty name. The
  // name differs from the shot that caused the death, so SetLastEffect
  // replaces the record and its HitSomething flag starts over. For Void
  // Shocker, the explosion (or spider egg) is judged on its own (fuzz
  // 20261018_082428 s7: a Scorpion2 whose sting killed a Burnbug under the
  // Arachnid Psion was shocked, because the egg it left hit nothing).
  fx.follow_up = false;
  push_back(std::move(fx), kFullDelay, kNoSymbol, new_cause());
  ps.detonate_pending = false;
  changed_ = true;
  log(ResolveEventType::CorpseExploded, pos, pawn.uid);
}

// Board::UpdateLeaders: the board psion is the leader type of the last
// living leader in list order (stage 7 spec 1.4). Every living pawn the psion
// affects gets Pawn::SetMutation (mutation 0, no psion left, reaches
// everyone); the rest keep their old mutation. Only the Soldier psion and the
// Psion Abomination change HP: SetMutation recomputes the maximum
// (ComputeHealthTotal @00878f30: +1 under mutation 1 or 7) and moves current
// HP by the same amount, so a pawn that only lived on the bonus dies, and the
// Abomination's death takes 1 HP from every Vek it buffed. The bonus is
// tracked per pawn (Pawn::health_bonus), which also gives it to pawns that
// appear while such a psion lives; a switch between the two keeps it.
void Simulation::update_leaders() {
  if (!track_leaders_) {
    // A psion that appears (a spawn, an emerging Vek) starts the tracking.
    track_leaders_ = std::any_of(board_.pawns().begin(), board_.pawns().end(),
                                 [](const Pawn& p) { return p.leader != Leader::None; });
    if (!track_leaders_) return;
  }
  Leader leader = Leader::None;
  for (const Pawn& p : board_.pawns()) {
    if (p.alive() && p.leader != Leader::None) leader = p.leader;
  }
  if (board_.psion != leader) changed_ = true;
  board_.psion = leader;
  const bool bonus = mutation_adds_health(leader);
  std::vector<int32_t> dying;
  for (Pawn& p : board_.pawns()) {
    if (!p.alive() || p.health_bonus == bonus) continue;
    if (leader != Leader::None && !mutation_affects(board_, p, leader)) continue;
    const int delta = bonus ? 1 : -1;
    p.health_bonus = bonus;
    p.max_hp = static_cast<int8_t>(std::max(0, p.max_hp + delta));
    p.hp = static_cast<int8_t>(std::max(0, p.hp + delta));
    changed_ = true;
    if (p.hp <= 0) dying.push_back(p.uid);
  }
  for (int32_t uid : dying) {
    Pawn* p = board_.find_pawn(uid);
    if (!p) continue;
    p->hp = 1;  // kill_pawn takes it from alive to dying
    kill_pawn(board_, *p, rules_);
  }
  note_deaths();
}

// Board::OnLoop's teleporter scan (P5, after the corpse explosions): the
// first pad holding a living pawn other than its recorded occupant warps it
// (Board::Teleport): nothing on water or chasm pads or cracked ones;
// otherwise the pawn and whatever stands on the partner pad (living, or a
// corpse) swap through an appended teleport effect.
void Simulation::update_teleporters() {
  std::vector<Point>& pads = board_.teleporters;
  std::vector<int32_t>& seen = board_.teleporter_occupants;
  if (pads.empty()) return;
  seen.resize(pads.size(), -1);
  auto living = [&](Point p) -> const Pawn* {
    const Pawn* q = has_pawn(board_, p) ? board_pawn(board_, p) : nullptr;
    return q && q->alive() ? q : nullptr;
  };
  for (size_t i = 0; i < pads.size(); ++i) {
    const Pawn* arrival = living(pads[i]);
    if (!arrival) {
      if (seen[i] != -1) changed_ = true;
      seen[i] = -1;
      continue;
    }
    if (arrival->uid == seen[i]) continue;
    const size_t j = i ^ 1u;
    if (j >= pads.size()) return;
    // Board::Teleport(i).
    seen[j] = arrival->uid;
    changed_ = true;
    const Tile& from = board_.tile(pads[i]);
    const Tile& to = board_.tile(pads[j]);
    if (from.is_chasm() || from.is_liquid() || to.is_chasm() || from.cracked || to.cracked) return;
    const Pawn* other = has_pawn(board_, pads[j]) ? board_pawn(board_, pads[j]) : nullptr;
    const bool swap = other && (other->alive() || is_corpse(board_, *other));
    seen[i] = swap ? other->uid : -1;
    SkillEffect fx;
    fx.add_teleport(pads[i], pads[j]);
    if (swap) fx.add_teleport(pads[j], pads[i]);
    prepare_effect(fx, pads[i], pads[j], Team::None, kNoSymbol);
    fx.owner = arrival->uid;
    fx.follow_up = true;
    push_back(std::move(fx), kFullDelay, last_shot_, new_cause());
    return;
  }
}

// Board::OnLoop's removal test, before the pawn's own update.
bool Simulation::removable(const Pawn& pawn, const PawnSim& ps) const {
  if (pawn.mech || pawn.alive() || !ps.dead) return false;
  if (death_anim_playing(ps)) return false;
  if (ps.xp_gone >= 0 && frame_ < ps.xp_gone) return false;
  if (ps.tracker.running || ps.death_pending || ps.death_fx_pending || ps.detonate_pending) return false;
  return !is_corpse(board_, pawn);
}

void Simulation::remove(int32_t uid) {
  const Pawn* p = board_.find_pawn(uid);
  log(ResolveEventType::PawnRemoved, p ? p->pos : kInvalidPoint, uid);
  board_.remove_pawn(uid);
  pawns_.erase(uid);
  changed_ = true;
}

// ---- Kill credit and pilot experience (itb/pilot_xp.hpp) ----------------------------

// Pawn::ProcessDeath @00886a80: a non-mech enemy-team pawn's death counts as a
// kill of the last shooter; unless it is Minor it gives XP equal to its max
// HP, to the shooter if its id is 0-2, else to the squad.
void Simulation::credit_kill(const Pawn& victim) {
  if (victim.team != Team::Enemy || victim.mech) return;
  const int32_t shooter = last_owner_;
  auto credit = [&](int32_t uid) -> KillCredit& {
    for (KillCredit& c : credits_) {
      if (c.uid == uid) return c;
    }
    credits_.push_back(KillCredit{uid});
    return credits_.back();
  };
  // "any_kill_<id>": only a mech's own entry is ever read.
  if (shooter >= 0) ++credit(shooter).any_kills;
  if (victim.minor) return;
  const int xp = std::max<int>(victim.max_hp, 0);
  if (shooter >= 0 && shooter < 3) {
    KillCredit& c = credit(shooter);
    c.xp += xp;
    ++c.kills;
  } else {
    env_xp_ += xp;
  }
}

// Pawn::UpdateKills @008816c0 (P5, no effect active, list order).
void Simulation::update_kills(Pawn& pawn) {
  if (credits_.empty()) return;
  auto it = std::find_if(credits_.begin(), credits_.end(),
                         [&](const KillCredit& c) { return c.uid == pawn.uid; });
  if (it == credits_.end()) return;
  const KillCredit c = *it;
  credits_.erase(it);
  // Pawn::IncreasePilotXp; Experienced pilots (Extra_XP) get 2 more per kill.
  const int xp = c.xp + (pawn.has_pilot(kPilotExtraXp) ? 2 * c.kills : 0);
  if (increase_pilot_xp(board_, pawn, xp)) {
    log(ResolveEventType::PilotLevelUp, pawn.pos, pawn.uid, pawn.pilot_level);
  }
  if (c.any_kills <= 0 || !pawn.mech) return;
  // The battle's kill count (+0x9e4), which Adrenaline adds to the move.
  if (pawn.has_level_skill(LevelSkill::Adrenaline)) {
    pawn.movement.pilot_bonus = static_cast<int8_t>(std::min(100, pawn.movement.pilot_bonus + c.any_kills));
  }
  // Viscera Nanobots: heal 1 (2 upgraded) per kill.
  if (board_.has_passive(kPassiveLeechKill)) {
    const int heal = board_.has_passive(kPassiveLeechKillA) ? 2 : 1;
    modify_health(board_, pawn, heal * c.any_kills, DamageMode::Weapon, rules_);
    state(pawn.uid).detonate_pending = false;
    note_deaths();
  }
  if (pawn.has_pilot(kPilotKoBoost)) pawn.boosted = true;
}

// BoardPlayer::UpdateXP @008a6a90: the squad's XP is split among the living
// mechs with a pilot, in list order: env_xp / n each, and the remainder +1
// each to a random subset. The subset is a chance node only when it decides a
// level-up; otherwise pilots whose XP no longer matters (top level or not
// recorded) take the +1s first, then list order.
void Simulation::update_xp() {
  if (env_xp_ <= 0) return;
  const int total = env_xp_;
  env_xp_ = 0;
  std::vector<Pawn*> mechs;
  for (Pawn& p : board_.pawns()) {
    if (p.mech && p.alive() && has_pilot_record(p)) mechs.push_back(&p);
  }
  if (mechs.empty()) return;
  const int n = static_cast<int>(mechs.size());
  const int share = total / n;
  int rest = total % n;
  std::vector<int> extra(mechs.size(), 0);
  if (rest > 0) {
    std::vector<size_t> decisive, plain;
    for (size_t i = 0; i < mechs.size(); ++i) {
      const Pawn& m = *mechs[i];
      (pilot_levels_up(m, share + 1) && !pilot_levels_up(m, share) ? decisive : plain).push_back(i);
    }
    std::stable_sort(plain.begin(), plain.end(),
                     [&](size_t a, size_t b) { return !tracks_pilot_xp(*mechs[a]) && tracks_pilot_xp(*mechs[b]); });
    if (!decisive.empty()) {
      // Distinct outcomes: which decisive mechs get a +1 (the rest go to the
      // plain ones), fewest level-ups first.
      const int nd = static_cast<int>(decisive.size());
      const int lo = std::max(0, rest - static_cast<int>(plain.size()));
      const int hi = std::min(rest, nd);
      std::vector<uint32_t> subsets;
      for (int k = lo; k <= hi; ++k) {
        for (uint32_t mask = 0; mask < (1u << nd); ++mask) {
          if (std::popcount(mask) == k) subsets.push_back(mask);
        }
      }
      int pick = 0;
      if (subsets.size() > 1) {
        ChanceRecord node{ChanceKind::XpSplit, frame_, mechs[decisive.front()]->pos, total, 0,
                          static_cast<int>(subsets.size())};
        pick = ctx_.choose ? ctx_.choose(node) : 0;
        pick = std::clamp(pick, 0, static_cast<int>(subsets.size()) - 1);
        node.outcome = pick;
        if (result_) result_->chances.push_back(node);
      }
      const uint32_t mask = subsets[static_cast<size_t>(pick)];
      for (int j = 0; j < nd; ++j) {
        if (mask & (1u << j)) {
          extra[decisive[static_cast<size_t>(j)]] = 1;
          --rest;
        }
      }
    }
    for (size_t i : plain) {
      if (rest <= 0) break;
      extra[i] = 1;
      --rest;
    }
  }
  for (size_t i = 0; i < mechs.size(); ++i) {
    Pawn& m = *mechs[i];
    if (increase_pilot_xp(board_, m, share + extra[i])) {
      log(ResolveEventType::PilotLevelUp, m.pos, m.uid, m.pilot_level);
    }
  }
}

// EventSystem::ClearLastEffect (LastShot::Clear): owner -1, no team, no shot.
// Board::OnLoop calls it only while a shooter is recorded.
void Simulation::clear_last_shot() {
  if (last_owner_ == -1) return;
  const int32_t owner = last_owner_;
  const Symbol shot = last_shot_;
  last_owner_ = -1;
  last_team_ = Team::None;
  last_shot_ = kNoSymbol;
  rules_.current_shot = kNoSymbol;
  // Board::OnLoop answers the cleared record with Void Shocker at once.
  void_shock(owner, shot, rules_.shot_hit, last_point_);
}

// Passive_VoidShock (Board::OnLoop @00955070, right after ClearLastEffect).
// The record's shooter is shocked when the shot hit nothing (no HitSomething:
// no unit or building lost HP) and it is not a move ("Move"), the shooter
// is enemy-team (Vek and bots alike), has no queued shot left (the instant
// part of a skill that telegraphs one is spared) and its type is not
// VoidShockImmune. A TC_Enrage shot (any owner) shocks the pawn on the
// recorded tile instead, whatever its team. The shock is a raw 1 HP loss
// (Pawn::ModifyHealth in weapon mode: shields, ice, armor and ACID do not
// matter) with a LightningBolt_Animated on the tile (ANIM_DELAY); a pawn it
// kills dies as any other (SetProcessDeath). One shock per shot: the passive
// has no upgrade, and a second carrier adds nothing.
void Simulation::void_shock(int32_t owner, Symbol shot, bool hit, Point point) {
  if (hit || !board_.has_passive(kPassiveVoidShock)) return;
  static const Symbol move = intern("Move");
  if (shot == move) return;
  const Pawn* shooter = board_.find_pawn(owner);
  if (!shooter) return;
  const bool enrage = symbol_name(shot).find("TC_Enrage") != std::string_view::npos;
  if (shooter->team != Team::Enemy && !enrage) return;
  if (shooter->queued.active()) return;
  if (rules_.data) {
    const PawnDef* def = rules_.data->pawn(shooter->type);
    if (def && def->void_shock_immune) return;
  }
  Pawn* target = enrage ? (point.valid() ? board_pawn(board_, point) : nullptr) : board_.find_pawn(owner);
  if (!target) return;
  const bool was_dead = !target->alive();
  const int before = target->hp;
  modify_health(board_, *target, -1, DamageMode::Weapon, rules_);
  if (target->hp < before) log(ResolveEventType::VoidShock, target->pos, target->uid, before - target->hp);
  if (target->pos.valid()) {
    SpaceDamage bolt;
    bolt.loc = target->pos;
    bolt.animation = intern("LightningBolt_Animated");
    bolt.anim_flags = kAnimDelay;
    register_tile_anim(bolt);
  }
  if (!was_dead && !target->alive() && !target->dying) {
    target->dying = true;
    target->queued = QueuedShot{};
    emit(rules_, RulesEventType::PawnKilled, target->pos, target->uid);
  }
  changed_ = true;
  note_deaths();
  register_holes();
}

}  // namespace itb::detail
