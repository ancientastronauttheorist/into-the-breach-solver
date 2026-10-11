// Stage 7: the enemy phase driver (enemy_phase.hpp). Behaviour follows
// BoardPlayer::StartState/UpdateState/UpdateQueued, Board::UpdateDots /
// Pawn::UpdateDot and Board::UpdateSpawning (stage 7 spec sections 1 and 3).

#include <algorithm>
#include <set>
#include <string>

#include "engine_impl.hpp"
#include "itb/enemy_phase.hpp"
#include "itb/engine.hpp"
#include "itb/game_data.hpp"
#include "itb/movement.hpp"
#include "rules_detail.hpp"

namespace itb {

const char* to_string(PhaseEventType t) {
  switch (t) {
    case PhaseEventType::EndTurn: return "end turn";
    case PhaseEventType::WebsCleared: return "webs cleared";
    case PhaseEventType::StatusTick: return "status tick";
    case PhaseEventType::EnvStep: return "environment step";
    case PhaseEventType::EnvInexact: return "environment inexact";
    case PhaseEventType::EnvUnsupported: return "environment unsupported";
    case PhaseEventType::MissionHook: return "mission hook";
    case PhaseEventType::ShotFired: return "shot fired";
    case PhaseEventType::ShotFizzled: return "shot fizzled";
    case PhaseEventType::ShotCancelled: return "shot cancelled";
    case PhaseEventType::Burrowed: return "burrowed";
    case PhaseEventType::MissionEnd: return "mission end";
    case PhaseEventType::SpawnEmerged: return "spawn emerged";
    case PhaseEventType::SpawnBlocked: return "spawn blocked";
    case PhaseEventType::SpawnDropped: return "spawn dropped";
    case PhaseEventType::Thawed: return "thawed";
    case PhaseEventType::TempUnitDied: return "temp unit died";
  }
  return "?";
}

int PhaseResult::count(PhaseEventType type) const {
  return static_cast<int>(std::count_if(events.begin(), events.end(),
                                        [type](const PhaseEvent& e) { return e.type == type; }));
}

namespace {

// The environment's window on the resolution in progress.
class Host final : public EnvHost {
 public:
  Host(Resolver& r, RulesContext& rules, const GameData& data, const TurnContext& ctx, PhaseResult& out)
      : r_(r), rules_(rules), data_(data), ctx_(ctx), out_(out) {}

  Board& board() override { return r_.board(); }
  const GameData& data() const override { return data_; }
  RulesContext& rules() override { return rules_; }
  void add_effect(SkillEffect effect) override { r_.add_effect(std::move(effect)); }

  int choose(ChanceKind kind, int options, Point where, int amount) override {
    if (options <= 1) return 0;
    ChanceRecord node{kind, r_.frame(), where, amount, 0, options};
    int pick = ctx_.choose ? ctx_.choose(node) : 0;
    node.outcome = std::clamp(pick, 0, options - 1);
    out_.chances.push_back(node);
    return node.outcome;
  }

  void note(PhaseEventType type, std::string detail, Point where, int32_t uid, int amount) override {
    if (type == PhaseEventType::EnvInexact || type == PhaseEventType::EnvUnsupported) out_.exact = false;
    out_.events.push_back(PhaseEvent{type, where, uid, amount, std::move(detail)});
  }

  int32_t new_uid() override { return r_.board().next_uid++; }

  bool busy() const override { return r_.busy(); }
  int turn() const override { return r_.board().turn; }

 private:
  Resolver& r_;
  RulesContext& rules_;
  const GameData& data_;
  const TurnContext& ctx_;
  PhaseResult& out_;
};

void absorb(PhaseResult& out, const ResolveResult& r) {
  out.chances.insert(out.chances.end(), r.chances.begin(), r.chances.end());
  out.timing.insert(out.timing.end(), r.timing.begin(), r.timing.end());
  out.quiescent = out.quiescent && r.quiescent;
}

void absorb(PhaseResult& out, const ActionResult& diag) {
  out.lua_errors.insert(out.lua_errors.end(), diag.lua_errors.begin(), diag.lua_errors.end());
  out.unapplied.insert(out.unapplied.end(), diag.unapplied.begin(), diag.unapplied.end());
}

// Pawn::UpdateDot(phase) for one pawn (stage 7 spec 1.4). Every HP change is
// a raw ModifyHealth in mode 0, except shielded or frozen pawns under the
// Storm Generator and the Psion Tyrant, which take Pawn::Damage (the shield or
// ice absorbs it). Returns what it did ("" = nothing; the game then skips its
// 1.5 s pause).
std::string update_dot(Board& b, Pawn& p, int phase, RulesContext& rules) {
  // UpdateDot skips only pawns with no board pointer (stage 7 §1.4). A
  // burrower underground (dove, position (-1,-1) until the AI moves it) is
  // still on the board's list: it ticks too (live 2026-10-10: a Regen psion
  // healed an underground Burrower2). Tile-based ticks below check the tile.
  if (p.fallen || (!p.pos.valid() && !(p.burrows && p.alive()))) return "";
  std::string did;
  if (phase == 0 && !p.alive() && is_corpse(b, p) && mutation_affects(b, p, Leader::Necro)) {
    modify_health(b, p, 1, DamageMode::Weapon, rules);
    did = "necro +1";
  }
  if (!p.alive()) return "";
  switch (phase) {
    case 1:
      if (!p.fire) break;
      if (mutation_affects(b, p, Leader::Fire)) {
        did = "fire psion: no burn";
      } else if (p.has_pilot(kPilotPainImmunity)) {
        modify_health(b, p, 1, DamageMode::Weapon, rules);
        did = "fire +1 (Cauterize)";
      } else {
        modify_health(b, p, -1, DamageMode::Weapon, rules);
        did = "fire -1";
      }
      break;
    case 2: {
      if (p.team != Team::Enemy || !p.pos.valid() || !b.tile(p.pos).smoke || !b.has_passive(kPassiveElectricSmoke)) break;
      const int d = b.has_passive(kPassiveElectricSmokeA) ? 2 : 1;
      if (p.shield || p.frozen || is_turn_shielded(b, p)) {
        damage_pawn(b, p, d, DamageMode::Weapon, rules);
      } else {
        modify_health(b, p, -d, DamageMode::Weapon, rules);
      }
      did = "storm generator -" + std::to_string(d);
      break;
    }
    case 3:
      if (!mutation_affects(b, p, Leader::Tentacle)) break;
      if (!p.shield && !p.frozen && !is_armored(b, p)) {
        modify_health(b, p, -1, DamageMode::Weapon, rules);
      } else {
        damage_pawn(b, p, 1, DamageMode::Weapon, rules);
      }
      did = "psion tyrant -1";
      break;
    case 4:
      if ((mutation_affects(b, p, Leader::Regen) || mutation_affects(b, p, Leader::Boss)) && p.hp < p.max_hp) {
        modify_health(b, p, 1, DamageMode::Weapon, rules);
        did = "psion regeneration +1";
      }
      break;
    case 5:
      if (p.has_pilot(kPilotRegen) && p.hp < p.max_hp) {
        modify_health(b, p, 1, DamageMode::Weapon, rules);
        did = "Regen pilot +1";
      }
      break;
    default:
      break;
  }
  // UpdateDot's end: at 0 HP the pawn's death is pending.
  if (!p.alive() && !p.dying) {
    p.dying = true;
    p.queued = QueuedShot{};
    detail::emit(rules, RulesEventType::PawnKilled, p.pos, p.uid);
  }
  return did;
}

// Board::GetPawns(6) empty, no queued spawn, no queued shooter.
bool enemy_cleared(const Board& b) {
  if (!b.spawn_points.empty()) return false;
  for (const Pawn& p : b.pawns()) {
    if (p.team == Team::Enemy && (p.alive() || is_corpse(b, p) || p.mech)) return false;
    if (p.queued.active() && p.alive()) return false;
  }
  return true;
}

}  // namespace

PhaseResult Engine::end_turn(Board& board, const TurnContext& tc) {
  PhaseResult out;
  ActionResult diag;
  ActionOptions opts;
  opts.grid_resist = tc.grid_resist;
  opts.death_seed = tc.death_seed;
  opts.spider_egg = tc.spider_egg;
  opts.choose = tc.choose;
  opts.events = tc.events;
  opts.log = tc.log;
  ResolveContext ctx;
  impl_->wire(*this, ctx, opts, diag);
  std::unique_ptr<Environment> env = make_native_environment(tc.mission);
  out.environment = env->name();

  // ---- End of the player's turn (BoardPlayer::EndState(0)): Pawn::EndTurn on
  // every player-team pawn; the my-turn flag goes off (Networked Shielding),
  // then ClearUndo.
  board.player_phase = false;
  int ended = 0;
  for (Pawn& p : board.pawns()) {
    p.movement.undo_ready = false;
    if (p.team != Team::Player) continue;
    p.active = false;
    p.movement.bonus_shift = 0;
    end_turn_movement(p);
    ++ended;
  }
  out.events.push_back(PhaseEvent{PhaseEventType::EndTurn, kInvalidPoint, -1, ended, ""});

  Resolver r(board, ctx);
  Host host(r, ctx.rules, impl_->data, tc, out);
  ctx.frame_hook = [&env, &host](Resolver&) { env->update(host); };
  env->bind(board);
  env->begin(host);

  std::set<int32_t> queued_at_start;
  for (const Pawn& p : board.pawns()) {
    if (p.queued.active() && p.alive()) queued_at_start.insert(p.uid);
  }

  // Burrowers that dove (stage 2 H4) leave the board until the AI moves them:
  // the executor takes each off when its dive ends (Resolver: FrameHooks::
  // start_dive), so after a settle they are underground; this logs them (and
  // would take off any still on the board).
  auto burrow_away = [&]() {
    std::vector<int32_t> seen;  // a pawn hit twice dove once
    for (int32_t uid : ctx.rules.burrow_dives) {
      if (std::find(seen.begin(), seen.end(), uid) != seen.end()) continue;
      seen.push_back(uid);
      Pawn* p = board.find_pawn(uid);
      if (!p || !p->alive()) continue;
      if (!p->pos.valid()) {
        out.events.push_back(PhaseEvent{PhaseEventType::Burrowed, p->movement.prev_pos, uid, 0, ""});
        continue;
      }
      out.events.push_back(PhaseEvent{PhaseEventType::Burrowed, p->pos, uid, 0, ""});
      p->movement.prev_pos = p->pos;
      p->pos = kInvalidPoint;
      p->queued = QueuedShot{};
      p->fire = false;
    }
    ctx.rules.burrow_dives.clear();
  };
  // Board::OnLoop: queued spawns on water or chasm are dropped silently.
  auto drop_spawns = [&](std::vector<std::string>* types) {
    for (size_t i = 0; i < board.spawn_points.size();) {
      const Tile& t = board.tile(board.spawn_points[i]);
      if (t.terrain == Terrain::Water || t.terrain == Terrain::Hole) {
        out.events.push_back(PhaseEvent{PhaseEventType::SpawnDropped, board.spawn_points[i], -1, 0, ""});
        board.spawn_points.erase(board.spawn_points.begin() + static_cast<std::ptrdiff_t>(i));
        if (types && i < types->size()) types->erase(types->begin() + static_cast<std::ptrdiff_t>(i));
      } else {
        ++i;
      }
    }
  };

  // ---- State 1 entry: Board::NextTurn(6) releases every web.
  const int webs = clear_all_webs(board);
  if (webs > 0) out.events.push_back(PhaseEvent{PhaseEventType::WebsCleared, kInvalidPoint, -1, webs, ""});
  absorb(out, r.settle());
  burrow_away();
  // Mission:IsEnvironmentEffect(), evaluated once here.
  bool env_on = env->is_effect(host);

  // ---- Status ticks: six phases, one pawn at a time.
  for (int phase = 0; phase < 6; ++phase) {
    std::vector<int32_t> order;
    for (const Pawn& p : board.pawns()) order.push_back(p.uid);
    for (int32_t uid : order) {
      Pawn* p = board.find_pawn(uid);
      if (!p) continue;
      std::string did;
      // Cheap pre-check: ticks only touch burning, smoked, psion-affected,
      // Regen or necro pawns; skip the frame loop for everyone else.
      bool candidate = false;
      switch (phase) {
        case 0: candidate = !p->alive() && board.psion == Leader::Necro; break;
        case 1: candidate = p->fire; break;
        case 2: candidate = p->team == Team::Enemy && board.has_passive(kPassiveElectricSmoke); break;
        case 3: candidate = board.psion == Leader::Tentacle; break;
        case 4: candidate = board.psion == Leader::Regen || board.psion == Leader::Boss; break;
        default: candidate = p->has_pilot(kPilotRegen); break;
      }
      if (!candidate) continue;
      absorb(out, r.apply([&]() {
        Pawn* q = board.find_pawn(uid);
        if (!q) return;
        did = update_dot(board, *q, phase, ctx.rules);
        r.update_leaders();
        if (!did.empty()) r.add_delay(1.5f);
      }));
      if (!did.empty()) out.events.push_back(PhaseEvent{PhaseEventType::StatusTick, kInvalidPoint, uid, phase, did});
      burrow_away();
    }
  }

  // ---- Environment steps, each resolved to idle before the next.
  for (int step = 0; env_on && step < 64; ++step) {
    env_on = env->apply(host);
    out.events.push_back(PhaseEvent{PhaseEventType::EnvStep, kInvalidPoint, -1, step, env->name()});
    absorb(out, r.settle());
    burrow_away();
  }

  // ---- Queued shooters: the first pawn in list order with a queued shot.
  std::set<int32_t> fired;
  for (int guard = 0; guard < 256; ++guard) {
    Pawn* shooter = nullptr;
    for (Pawn& p : board.pawns()) {
      if (p.queued.active()) {
        shooter = &p;
        break;
      }
    }
    if (!shooter) break;
    const int32_t uid = shooter->uid;
    if (!shooter->alive() || !shooter->pos.valid()) {
      shooter->queued = QueuedShot{};
      continue;
    }
    const Point target = shooter->queued.target;
    fired.insert(uid);
    WeaponInfo info;
    ActionResult a = impl_->queued_effect(board, uid, info);
    absorb(out, a);
    if (a.ok()) {
      out.events.push_back(PhaseEvent{PhaseEventType::ShotFired, target, uid, 0, a.weapon});
      absorb(out, r.resolve(a.effect, info));
    } else {
      out.events.push_back(PhaseEvent{PhaseEventType::ShotFizzled, target, uid, 0, a.weapon});
      absorb(out, r.settle());
    }
    burrow_away();
  }
  for (int32_t uid : queued_at_start) {
    if (fired.count(uid)) continue;
    const Pawn* p = board.find_pawn(uid);
    out.events.push_back(PhaseEvent{PhaseEventType::ShotCancelled, p ? p->pos : kInvalidPoint, uid, 0, ""});
  }

  // ---- State 2: the victory check, then the mission's NextTurn, spawns.
  const bool final_turn = board.turn == board.total_turns;
  if (final_turn || (board.turn > board.total_turns && !env->end_blocked(host) && enemy_cleared(board))) {
    out.mission_ended = true;
    out.events.push_back(PhaseEvent{PhaseEventType::MissionEnd, kInvalidPoint, -1, board.turn, ""});
    absorb(out, diag);
    ctx.frame_hook = nullptr;
    return out;
  }
  env->next_turn(host);
  absorb(out, r.settle());
  burrow_away();

  // Spawns: one attempt per idle frame, 1 s apart, in queue order.
  std::vector<std::string> types = tc.spawn_types;
  types.resize(board.spawn_points.size());
  drop_spawns(&types);
  if (!tc.spawn_order_known && board.spawn_points.size() > 1) {
    int blocked = 0;
    for (Point p : board.spawn_points) blocked += is_blocked(board, p, Pathing::lua(PathProfile::Ground)) ? 1 : 0;
    if (blocked > 0) {
      host.note(PhaseEventType::EnvInexact,
                "spawn queue order unknown with a blocked spawn (bridge reports scan order)", kInvalidPoint, -1, 0);
    }
  }
  std::vector<Point> queue = board.spawn_points;
  std::vector<Point> kept;
  for (size_t i = 0; i < queue.size(); ++i) {
    const Point p = queue[i];
    const Tile& t = board.tile(p);
    if (t.terrain == Terrain::Water || t.terrain == Terrain::Hole) {
      out.events.push_back(PhaseEvent{PhaseEventType::SpawnDropped, p, -1, 0, ""});
      continue;
    }
    const bool blocked = is_blocked(board, p, Pathing::lua(PathProfile::Ground));
    bool stays = blocked;
    absorb(out, r.apply([&]() {
      if (blocked) {
        Pawn* occ = has_pawn(board, p) ? board_pawn(board, p) : nullptr;
        out.events.push_back(PhaseEvent{PhaseEventType::SpawnBlocked, p, occ ? occ->uid : -1, 0, ""});
        // Stabilizers spare mechs; everything else takes a push-mode hit
        // (BoardSpace::DamageSpace(1, mode 1)).
        if (!(occ && occ->mech && board.has_passive(kPassiveBurrows))) {
          damage_tile(board, p, 1, DamageMode::Push, ctx.rules);
        }
      } else {
        const PawnDef* def = types[i].empty() ? nullptr : impl_->data.pawn(types[i]);
        if (def) {
          Pawn pawn = impl_->data.make_pawn(*def, host.new_uid(), p);
          pawn.active = true;
          Pawn& added = board.add_pawn(pawn);
          board.stamp_arrival(added);
          out.events.push_back(PhaseEvent{PhaseEventType::SpawnEmerged, p, added.uid, 0, types[i]});
        } else {
          out.emerged_unknown.push_back(p);
          out.events.push_back(PhaseEvent{PhaseEventType::SpawnEmerged, p, -1, 0, types[i]});
        }
      }
      r.add_delay(1.0f);
    }));
    burrow_away();
    if (stays) kept.push_back(p);
  }
  board.spawn_points = kept;
  drop_spawns(nullptr);

  // ---- "Temp Units Die" (Board::KillTempUnits @009460b0). Board::SpawnQueued
  // arms it at the start of state 2 (so not when the mission ended above);
  // UpdateStackedEffects runs it on the first idle frame once the spawn cursor
  // is done, before AI planning. If any listed pawn's type has Lua
  // TempUnit = true (Board::IsTempUnit), a 1 s delay is added; then every such
  // pawn still alive gets Pawn::Kill in list order, whatever its team: an
  // ordinary death (death effect, ACID pool, Explodes and Corpse as for any
  // pawn; the shipped one, DeployUnit_Bomby, has none of them). It is not a
  // Vek kill and gives no XP (the pawn is player-team).
  auto temp_unit = [this](const Pawn& p) {
    const PawnDef* def = impl_->data.pawn(p.type);
    return def && def->temp_unit;
  };
  if (std::any_of(board.pawns().begin(), board.pawns().end(), temp_unit)) {
    absorb(out, r.apply([&]() {
      r.add_delay(1.0f);
      for (Pawn& p : board.pawns()) {
        if (!temp_unit(p) || !p.alive()) continue;
        out.events.push_back(
            PhaseEvent{PhaseEventType::TempUnitDied, p.pos, p.uid, 0, std::string(symbol_name(p.type))});
        kill_pawn(board, p, ctx.rules);
      }
    }));
    burrow_away();
  }
  absorb(out, diag);
  ctx.frame_hook = nullptr;
  return out;
}

TurnResult Engine::play_turn(Board& board, const std::vector<PlayerAction>& actions, const TurnContext& ctx) {
  TurnResult out;
  const ActionOptions opts = action_options(ctx);
  auto refused = [](const ActionResult& a) { return !a.ok() && a.status != ActionStatus::NoEffect; };
  for (size_t i = 0; i < actions.size(); ++i) {
    const PlayerAction& a = actions[i];
    const Pawn* pawn = board.find_pawn(a.uid);
    if (a.move.valid() && pawn && a.move != pawn->pos) {
      out.actions.push_back(move(board, a.uid, a.move, opts));
      if (refused(out.actions.back())) {
        out.refused = static_cast<int>(i);
        return out;
      }
    }
    if (a.kind == PlayerAction::Kind::Weapon) {
      out.actions.push_back(fire_weapon(board, a.uid, a.weapon, a.target, a.target2, opts));
    } else if (a.kind == PlayerAction::Kind::Repair) {
      out.actions.push_back(
          repair(board, a.uid, a.target, a.weapon.empty() ? "Skill_Repair" : a.weapon, opts));
    } else {
      continue;
    }
    if (refused(out.actions.back())) {
      out.refused = static_cast<int>(i);
      return out;
    }
  }
  out.enemy = end_turn(board, ctx);
  return out;
}

}  // namespace itb
