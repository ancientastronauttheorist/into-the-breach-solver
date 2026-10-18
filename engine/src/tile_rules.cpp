// The damage pipeline: Board::DamageSpace, BoardSpace::DamageSpace,
// Pawn::Damage and Pawn::ModifyHealth.

#include "itb/tile_rules.hpp"

#include <algorithm>
#include <cstdlib>

#include "itb/game_data.hpp"
#include "rules_detail.hpp"

namespace itb {
namespace {

using detail::emit;

bool is_train_shot(Symbol shot) {
  const std::string_view name = symbol_name(shot);
  return name == "Train_Move" || name == "Armored_Train_Move";
}

// Pawn::Damage @0088b9a0 raises HitSomething for every pawn type but these
// three (rock walls and thrown rocks).
bool counts_as_shot_hit(const Pawn& pawn) {
  static const Symbol wall = intern("Wall");
  static const Symbol rock = intern("RockThrown");
  static const Symbol bomb_rock = intern("BombRock");
  return pawn.type != wall && pawn.type != rock && pawn.type != bomb_rock;
}

// D6d: a damaging hit on a building. Populated buildings roll Grid Defense
// first; a resisted hit leaves the building untouched.
void damage_building(Board& board, Point p, int damage, RulesContext& ctx) {
  Tile& t = board.tile(p);
  const int n = std::min<int>(damage, t.hp);
  if (n <= 0) return;
  const bool grid_hit = is_populated(t) && !ctx.freeze_events;
  if (grid_hit) {
    if (ctx.grid_resist && ctx.grid_resist(p, n)) {
      emit(ctx, RulesEventType::GridResisted, p, -1, n);
      return;
    }
    board.grid_power = std::max(0, board.grid_power - n);
    emit(ctx, RulesEventType::GridDamaged, p, -1, n);
  }
  t.hp = static_cast<int8_t>(t.hp - n);
  // EventSystem::HitSomething (BoardSpace::DamageSpace): any HP a building
  // loses, populated or not. A Grid Defense resist above is no hit.
  ctx.shot_hit = true;
  emit(ctx, RulesEventType::BuildingDamaged, p, -1, n);
  if (t.hp == 0) emit(ctx, RulesEventType::BuildingDestroyed, p);
  if (board.has_passive(kPassiveAutoShield) && t.hp > 0) set_tile_shield(board, p, true, ctx);
  // PowerBar::Damage: an unresisted hit that leaves the grid at exactly 1
  // raises event 17, which Board::OnLoop answers in the next frame with
  // Critical Shields. Without the frame-exact executor they go up at once.
  if (grid_hit && board.grid_power == 1 && board.has_passive(kPassiveCritDefense)) {
    if (ctx.frame) {
      ctx.frame->grid_critical();
    } else {
      crit_defense_shields(board, ctx);
    }
  }
}

// D6c: what a damaging hit does to the terrain itself. Bumps only reach
// mountains and buildings.
void damage_terrain_step(Board& board, Point p, const SpaceDamage& sd, int damage, bool bump,
                         RulesContext& ctx) {
  switch (board.tile(p).terrain) {
    case Terrain::Mountain:
      damage_terrain(board, p, damage, ctx);
      return;
    case Terrain::Building:
      damage_building(board, p, damage, ctx);
      return;
    case Terrain::Ice:
      if (!bump) damage_terrain(board, p, damage, ctx);
      return;
    case Terrain::Forest:
      // A hit that also sets the terrain does not ignite the forest.
      if (!bump && sd.terrain == kNoTerrainChange) set_tile_fire(board, p, true, ctx);
      return;
    case Terrain::Sand:
      if (!bump) {
        set_tile_smoke(board, p, true, ctx);
        set_terrain(board, p, Terrain::Road, ctx);
      }
      return;
    default:
      return;
  }
}

// Pawn::Retreat (bEvacuate, end-of-mission only). The retreat mark is the
// minor flag, so a pawn that is already minor (Lua Minor, or retreated
// before) is killed instead, unless it is a mission pawn.
void retreat(Board& board, Pawn& pawn, RulesContext& ctx) {
  if (pawn.frozen || pawn.team == Team::Player) return;
  if (pawn.minor) {
    if (!pawn.mission_critical) kill_pawn(board, pawn, ctx);
    return;
  }
  pawn.minor = true;
  // Bots power down where they stand.
  if (pawn.faction == Faction::Bots) {
    pawn.movement.powered = false;
    return;
  }
  // Flyers fly off: over a chasm or water natively that is a fall-state
  // animation; over ground only an animation plays. Not modelled further.
  if (is_flying(pawn)) return;
  if (pawn.pos.valid() && board.tile(pawn.pos).terrain == Terrain::Water) {
    kill_pawn_instant(board, pawn, ctx);
  } else {
    pawn.hp = 0;  // burrows out: gone without a death
  }
}

void burrow_dive(Board& board, Pawn& pawn, RulesContext& ctx) {
  pawn.queued = QueuedShot{};
  set_pawn_fire(board, pawn, false);
  ctx.burrow_dives.push_back(pawn.uid);
  emit(ctx, RulesEventType::PawnBurrowed, pawn.pos, pawn.uid);
  // The dive animation; SetSpace(-1, -1) when it ends.
  if (ctx.frame) ctx.frame->start_dive(pawn);
}

int32_t next_uid(Board& board) { return board.next_uid++; }

// B6: sPawn. Kills whatever stands there, then creates the pawn unless the
// tile is blocked for ground units. Water and chasms always accept it; the
// pawn then drowns or falls at settle.
void spawn_from_damage(Board& board, const SpaceDamage& sd, RulesContext& ctx) {
  const Point p = sd.loc;
  if (has_pawn(board, p)) {
    for (Pawn* pawn : occupants(board, p)) kill_pawn(board, *pawn, ctx);
  }
  const Terrain t = board.tile(p).terrain;
  const bool terrain_blocks = t == Terrain::Building || t == Terrain::Mountain ||
                              t == Terrain::Water || t == Terrain::Hole;
  const bool blocked = terrain_blocks || has_pawn(board, p);
  if (blocked && t != Terrain::Water && t != Terrain::Hole) return;

  const PawnDef* def = ctx.data ? ctx.data->pawn(sd.spawn_pawn) : nullptr;
  if (!def) {
    ctx.spawns.push_back(SpawnRequest{sd.spawn_pawn, p, sd.spawn_team});
    return;
  }
  Pawn pawn = ctx.data->make_pawn(*def, next_uid(board), p);
  // iPawnTeam left at TEAM_NONE means the type's default team.
  if (sd.spawn_team != Team::None) pawn.team = sd.spawn_team;
  if (detail::type_is(pawn, "SpiderlingEgg1")) pawn.active = false;
  const Pawn& added = board.add_pawn(pawn);
  emit(ctx, RulesEventType::PawnSpawned, p, added.uid, 0, added.type);
}

}  // namespace

void retreat_pawn(Board& board, Pawn& pawn, RulesContext& ctx) { retreat(board, pawn, ctx); }

Pawn* board_pawn(Board& board, Point p) {
  if (!has_pawn(board, p)) return nullptr;
  const std::vector<Pawn*> occ = occupants(board, p);
  for (Pawn* pawn : occ) {
    if (pawn->alive()) return pawn;
  }
  return occ.back();
}

Pawn* listed_pawn(Board& board, Point p) {
  if (!p.valid()) return nullptr;
  const std::vector<Pawn*> occ = occupants(board, p);
  if (occ.empty()) return nullptr;
  for (Pawn* pawn : occ) {
    if (pawn->alive()) return pawn;
  }
  return occ.back();
}

int effective_damage(const SpaceDamage& sd) {
  return sd.damage == kDamageZero ? 0 : sd.damage;
}

void apply_space_damage(Board& board, const SpaceDamage& sd, RulesContext& ctx) {
  // B1: the script runs first, even for an off-board location.
  if (!sd.script.empty()) {
    if (ctx.run_script) {
      ctx.run_script(board, sd.script, sd.loc);
    } else {
      ctx.scripts.push_back(sd.script);
    }
  }
  if (!sd.loc.valid()) return;
  damage_tile(board, sd.loc, sd, DamageMode::Weapon, ctx);
  if (sd.spawn_pawn != kNoSymbol) spawn_from_damage(board, sd, ctx);
}

void damage_tile(Board& board, Point p, int damage, DamageMode mode, RulesContext& ctx) {
  SpaceDamage sd;
  sd.loc = p;
  sd.damage = damage;
  damage_tile(board, p, sd, mode, ctx);
}

void damage_tile(Board& board, Point p, SpaceDamage sd, DamageMode mode, RulesContext& ctx) {
  if (!p.valid()) return;
  // D0: the explosion override makes the tile treat the hit as a weapon hit
  // and hands pawns explosion mode.
  const bool explosion = sd.mode_override == kModeExplosion;
  const bool bump = mode == DamageMode::Push && !explosion;
  const DamageMode pawn_mode = explosion ? DamageMode::Explosion : mode;
  Tile& t = board.tile(p);

  // D2: shield field.
  if (sd.shield > 0) {
    set_tile_shield(board, p, true, ctx);
  } else if (sd.shield < 0) {
    set_tile_shield(board, p, false, ctx);
  }

  // D3: one shield (else the frozen state) absorbs a damaging hit. The SD
  // itself is zeroed, so the later terrain steps see no damage either. A
  // shield that is still up afterwards blocks the freeze.
  {
    const Pawn* occ0 = has_pawn(board, p) ? first_occupant(board, p) : nullptr;
    const bool turn_shield = occ0 && is_turn_shielded(board, *occ0);
    if (tile_shielded(board, p) || tile_frozen(board, p) || turn_shield) {
      if (effective_damage(sd) > 0) {
        if (tile_shielded(board, p)) {
          set_tile_shield(board, p, false, ctx);
        } else {
          set_tile_frozen(board, p, false, ctx);
        }
        if (sd.damage != kDamageDeath) sd.damage = 0;
      }
      if (tile_shielded(board, p)) sd.frozen = StatusChange::None;
    }
  }

  // D4: every occupant picks up tile hazards (with the post-absorption shield
  // state), then takes its own copy of the hit.
  bool had_pawn = false;
  if (has_pawn(board, p)) {
    for (Pawn* pawn : occupants(board, p)) {
      check_acid_fire(board, p, *pawn, ctx);
      damage_pawn(board, *pawn, sd, pawn_mode, ctx);
    }
    had_pawn = true;
  }

  // D5: weapon damage sets off items (except Repair Mines) and breaks pods.
  const int d = effective_damage(sd);
  if (!bump && d > 0) {
    static const Symbol repair_mine = intern("Item_Repair_Mine");
    if (t.item != repair_mine) trigger_item(board, p, ctx);
    if (t.pod == PodState::Present) {
      t.pod = PodState::Destroyed;
      emit(ctx, RulesEventType::PodDestroyed, p);
    }
  }

  // D6: vines, crack collapse, terrain and buildings.
  if (d > 0) {
    if (!bump && t.vines) t.vines = false;
    if (!bump && t.cracked) {
      if (!is_train_shot(ctx.current_shot)) set_terrain(board, p, Terrain::Hole, ctx);
    } else {
      damage_terrain_step(board, p, sd, d, bump, ctx);
    }
  }

  // D7: a plain building at 0 HP becomes rubble; unique buildings stay.
  if (t.is_building() && t.hp == 0 && t.unique_building == kNoSymbol) {
    set_terrain(board, p, Terrain::Rubble, ctx);
  }

  // D8: iTerrain.
  if (sd.terrain != kNoTerrainChange) {
    if (sd.terrain == static_cast<int>(Terrain::Hole)) {
      // Opening a chasm is animated; settle completes it.
      if (t.terrain != Terrain::Hole) {
        t.pending_hole = true;
      } else {
        set_terrain(board, p, Terrain::Hole, ctx);
      }
    } else if (sd.terrain == static_cast<int>(Terrain::Building)) {
      // Natively this loops "kill occ[0] while the tile has a pawn", which
      // never ends on a corpse. Kill each pawn once instead.
      for (Pawn* pawn : occupants(board, p)) {
        if (counts_as_pawn(board, *pawn)) kill_pawn(board, *pawn, ctx);
      }
      add_building(board, p, ctx);
    } else {
      set_terrain(board, p, sd.terrain, ctx);
    }
  }

  // D9
  if (sd.crack) set_cracked(board, p, true, ctx);

  // D10: fire sets off any item (Repair Mines too) and ignites every occupant,
  // whatever the terrain.
  if (sd.fire == StatusChange::Apply) {
    trigger_item(board, p, ctx);
    set_tile_fire(board, p, true, ctx);
    for (Pawn* pawn : occupants(board, p)) set_pawn_fire(board, *pawn, true);
  } else if (sd.fire == StatusChange::Remove) {
    set_tile_fire(board, p, false, ctx);
  }

  // D11: acid lands on the tile only when no pawn took it.
  if (sd.acid == StatusChange::Apply && (!had_pawn || tile_shielded(board, p))) {
    set_tile_acid(board, p, true, ctx);
  }

  // D12: the push starts now and resolves in stage 3, on the last occupant
  // (which may have just died above).
  if (sd.push != Dir::None && !t.is_building()) {
    const std::vector<Pawn*> occ = occupants(board, p);
    if (!occ.empty()) {
      ctx.pushes.push_back(PendingPush{occ.back()->uid, p, sd.push});
      if (sd.push == Dir::Flip) detail::release_webs(board, p);
    }
  }

  // D13, D14 (iFrozen may have been cleared by D3).
  if (sd.smoke == StatusChange::Apply) {
    set_tile_smoke(board, p, true, ctx);
  } else if (sd.smoke == StatusChange::Remove) {
    set_tile_smoke(board, p, false, ctx);
  }
  if (sd.frozen == StatusChange::Apply) {
    set_tile_frozen(board, p, true, ctx);
  } else if (sd.frozen == StatusChange::Remove) {
    set_tile_frozen(board, p, false, ctx);
  }

  // D15
  if (sd.evacuate && has_pawn(board, p)) {
    for (Pawn* pawn : occupants(board, p)) retreat(board, *pawn, ctx);
  }

  // D16: BoardSpace::SetGrappled(dir toward the grapple point). This tile
  // (the webber's, Lua AddGrapple's first point) emits a web to its
  // neighbour in that direction; if the neighbour is grappleable (a building,
  // or a living pawn or corpse) it holds one web more and its first occupant
  // is webbed (add_web).
  if (sd.grapple_source.x >= 0 && has_pawn(board, p)) {
    const Point d = sd.grapple_source - p;
    const Dir dir = std::abs(d.x) > std::abs(d.y) ? (d.x > 0 ? Dir::Right : Dir::Left)
                                                  : (d.y > 0 ? Dir::Down : Dir::Up);
    add_web(board, p, dir);
  }

  // D17
  if (sd.item != kNoSymbol) t.item = sd.item;
}

void damage_pawn(Board& board, Pawn& pawn, int damage, DamageMode mode, RulesContext& ctx) {
  SpaceDamage sd;
  sd.loc = pawn.pos;
  sd.damage = damage;
  damage_pawn(board, pawn, sd, mode, ctx);
}

void damage_pawn(Board& board, Pawn& pawn, SpaceDamage sd, DamageMode mode, RulesContext& ctx) {
  // P1: dead pawns ignore hits, except mech corpses (which ignore ACID).
  const bool was_dead = !pawn.alive();
  if (was_dead) {
    if (!pawn.mech) return;
    sd.acid = StatusChange::None;
  }

  // P2: second line of absorption (other occupants, direct callers).
  if (pawn.shield && effective_damage(sd) > 0) {
    if (sd.damage != kDamageDeath) sd.damage = 0;
    set_pawn_shield(board, pawn, false);
  } else if (pawn.frozen && effective_damage(sd) > 0) {
    if (sd.damage != kDamageDeath) sd.damage = 0;
    set_pawn_frozen(board, pawn, false);
  }

  // P3: weapon hits: armor, then ACID doubling.
  int x = effective_damage(sd);
  if (mode == DamageMode::Weapon && x >= 1) {
    if (is_armored(board, pawn)) --x;
    if (pawn.acid) x *= 2;
    pawn.infected = false;
  }
  // P4: Force Amp adds 1 to bumps on Vek (never to explosions).
  if (x > 0 && mode == DamageMode::Push) {
    pawn.infected = false;
    if (board.has_passive(kPassiveForceAmp) && is_vek(pawn)) ++x;
  } else if (x > 0 && mode == DamageMode::Explosion) {
    pawn.infected = false;
  }

  // P5: fire and injury land before the HP change.
  if (sd.fire == StatusChange::Apply) {
    set_pawn_fire(board, pawn, true);
  } else if (sd.fire == StatusChange::Remove) {
    set_pawn_fire(board, pawn, false);
  }
  if (sd.injure == StatusChange::Apply) {
    set_pawn_injured(pawn, true);
  } else if (sd.injure == StatusChange::Remove) {
    set_pawn_injured(pawn, false);
  }

  // EventSystem::HitSomething (Pawn::Damage): the hit takes HP from a living
  // pawn (Networked Shielding zeroes it), unless the pawn is a Wall,
  // RockThrown or BombRock. Mech corpses have no HP to lose.
  if (x > 0 && pawn.alive() && !is_turn_shielded(board, pawn) && counts_as_shot_hit(pawn)) ctx.shot_hit = true;

  // P6
  modify_health(board, pawn, -x, mode, ctx);

  // P7: ACID lands after, so a hit never doubles itself.
  if (sd.acid == StatusChange::Apply) {
    set_pawn_acid(board, pawn, true);
  } else if (sd.acid == StatusChange::Remove) {
    set_pawn_acid(board, pawn, false);
  }

  // P8: a heal clears fire, frozen, infection and ACID.
  if (x < 0) {
    set_pawn_fire(board, pawn, false);
    set_pawn_frozen(board, pawn, false);
    pawn.infected = false;
    set_pawn_acid(board, pawn, false);
  }

  // P9
  if (!was_dead && !pawn.alive() && !pawn.dying) {
    pawn.dying = true;
    emit(ctx, RulesEventType::PawnKilled, pawn.pos, pawn.uid);
  }
}

void modify_health(Board& board, Pawn& pawn, int delta, DamageMode mode, RulesContext& ctx) {
  if (!pawn.alive() && !is_corpse(board, pawn)) return;  // H1
  if (delta < 0 && is_turn_shielded(board, pawn)) return;  // H2

  // H3: HP clamps to [0, max]; healing a corpse above 0 revives it.
  const bool was_dead = !pawn.alive();
  const int before = pawn.hp;
  const int after = std::clamp(before + delta, 0, std::max<int>(pawn.max_hp, 0));
  pawn.hp = static_cast<int8_t>(after);
  if (after < before) emit(ctx, RulesEventType::PawnDamaged, pawn.pos, pawn.uid, before - after);
  if (after > before) emit(ctx, RulesEventType::PawnHealed, pawn.pos, pawn.uid, after - before);
  if (was_dead && pawn.alive()) {
    pawn.dying = false;
    emit(ctx, RulesEventType::PawnRevived, pawn.pos, pawn.uid);
  }

  // H4: a hurt burrower dives, unless a weapon hit it on a cracked tile.
  if (pawn.burrows && delta < 0 && pawn.alive()) {
    const bool cracked = pawn.pos.valid() && board.tile(pawn.pos).cracked;
    if (!(cracked && mode != DamageMode::Push)) burrow_dive(board, pawn, ctx);
  }

  // H5: Retaliation hits every adjacent enemy-team pawn (bots too) for 1, in
  // weapon mode, before the triggering hit continues.
  if (delta < 0 && pawn.has_pilot(kPilotRetaliation) && pawn.alive() && pawn.pos.valid()) {
    const Point origin = pawn.pos;
    for (int d = 0; d < 4; ++d) {
      const Point q = step(origin, static_cast<Dir>(d));
      if (!q.valid()) continue;
      const Pawn* target = board_pawn(board, q);
      if (!target || target->team != Team::Enemy) continue;
      SpaceDamage hit;
      hit.loc = q;
      hit.damage = 1;
      apply_space_damage(board, hit, ctx);
    }
  }
}

void kill_pawn(Board& board, Pawn& pawn, RulesContext& ctx) {
  (void)board;
  if (!pawn.alive()) return;
  pawn.dying = true;
  pawn.queued = QueuedShot{};
  pawn.hp = 0;
  pawn.infected = false;
  emit(ctx, RulesEventType::PawnKilled, pawn.pos, pawn.uid);
}

void kill_pawn_instant(Board& board, Pawn& pawn, RulesContext& ctx) {
  kill_pawn(board, pawn, ctx);
  if (ctx.frame) ctx.frame->instant_kill(pawn);
}

void fall_pawn(Board& board, Pawn& pawn, RulesContext& ctx) {
  if (is_flying(pawn) && !pawn.frozen) return;
  if (pawn.fallen) return;
  if (ctx.frame && ctx.frame->start_fall(pawn)) return;
  finish_fall(board, pawn, ctx);
}

void finish_fall(Board& board, Pawn& pawn, RulesContext& ctx) {
  if (pawn.fallen) return;
  pawn.fallen = true;
  emit(ctx, RulesEventType::PawnFell, pawn.pos, pawn.uid);
  kill_pawn_instant(board, pawn, ctx);
}

}  // namespace itb
