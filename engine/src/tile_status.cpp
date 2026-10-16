// Terrain transitions, tile and pawn statuses, items, terrain dangers and the
// continuous settle rules.

#include <algorithm>

#include "itb/tile_rules.hpp"
#include "rules_detail.hpp"

namespace itb {
namespace {

using detail::emit;

const Pawn* first_occupant_const(const Board& board, Point p) {
  const Pawn* first = nullptr;
  for (const Pawn& pawn : board.pawns()) {
    if (pawn.occupies(p) && !pawn.fallen && (!first || pawn.arrival < first->arrival)) first = &pawn;
  }
  return first;
}

void destroy_pod(Tile& t, Point p, RulesContext& ctx) {
  if (t.pod != PodState::Present) return;
  t.pod = PodState::Destroyed;
  emit(ctx, RulesEventType::PodDestroyed, p);
}

bool busy(const RulesContext& ctx, const Pawn& pawn, bool ignore_push = false) {
  return ctx.frame && ctx.frame->pawn_busy(pawn, ignore_push);
}

// Pawn::IsSmoked: on smoke, not busy, and not immune to it.
bool is_smoked(const Board& board, const Pawn& pawn, const RulesContext& ctx) {
  return pawn.pos.valid() && board.tile(pawn.pos).smoke && !busy(ctx, pawn) &&
         !pawn.ignore_smoke && !pawn.has_pilot(kPilotDisableImmunity);
}

// Pawn::IsSubmerged: in water, not busy and not flying.
bool is_submerged(const Board& board, const Pawn& pawn, const RulesContext& ctx) {
  return pawn.pos.valid() && board.tile(pawn.pos).terrain == Terrain::Water &&
         !busy(ctx, pawn) && !is_flying(pawn);
}

}  // namespace

// A tile on which settle_tile_frame provably changes nothing: no pawn
// stands on it (fallen ones included, so every occupant loop below is empty
// and has_pawn / tile_frozen read the tile alone), no web hangs from it, and
// none of the tile rules below applies to its state. Each condition mirrors
// one branch of settle_tile_frame, in order.
bool detail::inert_tile_state(const Tile& t) {
  if (t.pending_hole) return false;
  if (t.terrain == Terrain::Hole && t.frozen) return false;
  if (t.lava && t.terrain != Terrain::Water && t.terrain != Terrain::Ice) return false;
  if ((t.terrain == Terrain::Hole || t.terrain == Terrain::Mountain) && ((t.acid && !t.lava) || t.cracked)) return false;
  if ((t.terrain == Terrain::Building || t.terrain == Terrain::Water) && t.cracked) return false;
  if (t.acid && (t.terrain == Terrain::Forest || t.terrain == Terrain::Sand || t.pod == PodState::Present)) return false;
  if (t.on_fire() && (t.terrain == Terrain::Sand || t.item != kNoSymbol)) return false;
  // check_webs: a web this tile emits breaks (nothing holds it).
  if (t.web_out != 0) return false;
  return true;
}

static bool inert_empty_tile(const Board& board, Point p) {
  if (!detail::inert_tile_state(board.tile(p))) return false;
  for (const Pawn& pawn : board.pawns()) {
    if (pawn.occupies(p)) return false;
  }
  return true;
}

// An occupied tile on which settle_tile_frame provably changes nothing, by
// the branches below in order:
//   - frozen chasm, lava flag, deferred chasm, cracks on chasms, mountains,
//     buildings and water, acid, fire (sand, ACID/fire pickup, items, Heat
//     Engines): ruled out by the tile's state;
//   - each occupant (busy or not, to keep it simple): no pod to pick up, no
//     Nanofilter smoke on a mech, no spikes, no burning pawn in smoke or on
//     a forest; drowning and falling need water or a chasm, mines an item;
//   - release_webs and check_webs need a web from or onto the tile.
bool detail::quiet_occupied_tile_state(const Tile& t) {
  if (t.terrain == Terrain::Hole || t.terrain == Terrain::Mountain || t.terrain == Terrain::Building ||
      t.terrain == Terrain::Water) {
    return false;
  }
  return !(t.lava || t.pending_hole || t.acid || t.on_fire() || t.spikes || t.item != kNoSymbol ||
           t.pod == PodState::Present || t.web_out != 0 || t.web_in != 0);
}

static bool quiet_occupied_tile(const Board& board, Point p) {
  const Tile& t = board.tile(p);
  if (!detail::quiet_occupied_tile_state(t)) return false;
  const bool healing_smoke = t.smoke && board.has_passive(kPassiveHealingSmoke);
  for (const Pawn& pawn : board.pawns()) {
    if (!pawn.occupies(p) || pawn.fallen) continue;
    if (healing_smoke && pawn.mech) return false;
    if (pawn.fire && (t.smoke || t.terrain == Terrain::Forest)) return false;
  }
  return true;
}

bool detail::settle_tile_noop(const Board& board, Point p) {
  return inert_empty_tile(board, p) || quiet_occupied_tile(board, p);
}

// One frame of BoardSpace::OnLoop, rule effects only.
void settle_tile_frame(Board& board, Point p, RulesContext& ctx) {
  if (inert_empty_tile(board, p) || quiet_occupied_tile(board, p)) return;
  Tile& t = board.tile(p);

  if (t.terrain == Terrain::Hole && tile_frozen(board, p) && !has_pawn(board, p)) t.frozen = false;
  if (t.lava && t.terrain != Terrain::Water && t.terrain != Terrain::Ice) t.lava = false;

  // A deferred chasm (iTerrain = HOLE) opens once its animation is over. Its
  // first pawn dies unless it flies (frozen flyers drop too).
  if (t.pending_hole && (!ctx.frame || ctx.frame->hole_ready(p))) {
    t.pending_hole = false;
    set_terrain(board, p, Terrain::Hole, ctx);
    if (has_pawn(board, p)) {
      Pawn* occ0 = first_occupant(board, p);
      if (!(is_flying(*occ0) && !occ0->frozen)) kill_pawn_instant(board, *occ0, ctx);
    }
  }

  if (t.terrain == Terrain::Hole || t.terrain == Terrain::Mountain) {
    if (!t.lava) t.acid = false;
    set_cracked(board, p, false, ctx);
  } else if (t.terrain == Terrain::Building || t.terrain == Terrain::Water) {
    set_cracked(board, p, false, ctx);
  }

  if (t.acid) {
    if (t.terrain == Terrain::Forest || t.terrain == Terrain::Sand) {
      set_terrain(board, p, Terrain::Road, ctx);
    }
    destroy_pod(t, p, ctx);
  }
  if (t.on_fire() && t.terrain == Terrain::Sand) set_terrain(board, p, Terrain::Road, ctx);

  for (Pawn* pawn : occupants(board, p)) {
    if (busy(ctx, *pawn)) continue;
    if (t.terrain == Terrain::Water && pawn->has_pilot(kPilotFreezeWalk)) {
      set_terrain(board, p, Terrain::Ice, ctx);
    }
    if (t.pod == PodState::Present && pawn->alive()) {
      if (pawn->team == Team::Player) {
        t.pod = PodState::Collected;
        emit(ctx, RulesEventType::PodCollected, p, pawn->uid);
      } else {
        destroy_pod(t, p, ctx);
      }
    }
    check_acid_fire(board, p, *pawn, ctx);
    if (t.smoke) set_pawn_fire(board, *pawn, false);
    if (pawn->fire && t.terrain == Terrain::Forest) set_tile_fire(board, p, true, ctx);
  }

  check_terrain_dangers(board, p, ctx);
  if (t.on_fire() && t.item != kNoSymbol) trigger_item(board, p, ctx);
  if (tile_frozen(board, p)) detail::release_webs(board, p);
  check_webs(board, p, ctx);
}

// SetGrappled(4) on p (@0091b300): p lets go of one web; with none left
// its first occupant is no longer webbed.
static void let_go_web(Board& board, Point p) {
  if (!p.valid()) return;
  Tile& t = board.tile(p);
  if (t.web_in > 1) {
    --t.web_in;
    return;
  }
  t.web_in = 0;
  if (has_pawn(board, p)) first_occupant(board, p)->webbed = false;
}

void add_web(Board& board, Point from, Dir d) {
  const Point to = step(from, d);
  if (!from.valid() || !to.valid()) return;
  // BoardSpace::IsGrappleable: a building, or a pawn (IsPawnSpace).
  if (!board.tile(to).is_building() && !has_pawn(board, to)) return;
  Tile& t = board.tile(from);
  const uint8_t bit = static_cast<uint8_t>(1u << static_cast<int>(d));
  // Natively a second SetGrappled(d) lists d twice and counts the web twice;
  // both entries always break together, so one bit stands for them.
  if (!(t.web_out & bit)) {
    t.web_out |= bit;
    Tile& held = board.tile(to);
    if (held.web_in < 255) ++held.web_in;
  }
  if (has_pawn(board, to)) first_occupant(board, to)->webbed = true;
}

uint8_t web_sources(const Board& board, Point p) {
  uint8_t out = 0;
  if (!p.valid()) return out;
  for (int d = 0; d < 4; ++d) {
    const Point n = step(p, static_cast<Dir>(d));
    if (n.valid() && (board.tile(n).web_out >> static_cast<int>(opposite(static_cast<Dir>(d)))) & 1) {
      out |= static_cast<uint8_t>(1u << d);
    }
  }
  return out;
}

int clear_all_webs(Board& board) {
  int webbed = 0;
  for (Pawn& p : board.pawns()) {
    webbed += p.webbed ? 1 : 0;
    p.webbed = false;
  }
  for (int i = 0; i < kTileCount; ++i) {
    Tile& t = board.tile(Point::from_index(i));
    t.web_out = 0;
    t.web_in = 0;
  }
  return webbed;
}

// BoardSpace::OnLoop's web part (after CheckTerrainDangers and the frozen
// release). Each web p emits breaks once p no longer holds a living pawn or
// corpse, or its first occupant is not on the enemy team (Pawn::IsEnemy), or
// the webbed tile is no longer grappleable (a building, or a living pawn /
// corpse on it), or the webbed tile's pawn is immune (IsGrappleImmunity:
// Disable_Immunity) and not busy. Then, while p holds a web, its first
// occupant is webbed once it is not busy (a pawn that arrives on a held tile
// is caught too).
void check_webs(Board& board, Point p, RulesContext& ctx) {
  Tile& t = board.tile(p);
  if (t.web_out != 0) {
    const bool emitter_ok = has_pawn(board, p) && first_occupant(board, p)->team == Team::Enemy;
    for (int d = 0; d < 4; ++d) {
      const uint8_t bit = static_cast<uint8_t>(1u << d);
      if (!(t.web_out & bit)) continue;
      const Point to = step(p, static_cast<Dir>(d));
      bool holds = emitter_ok && to.valid() && (board.tile(to).is_building() || has_pawn(board, to));
      if (holds && has_pawn(board, to)) {
        const Pawn& held = *first_occupant(board, to);
        if (held.has_pilot(kPilotDisableImmunity) && !busy(ctx, held)) holds = false;
      }
      if (!holds) {
        let_go_web(board, to);
        t.web_out &= static_cast<uint8_t>(~bit);
      }
    }
  }
  if (t.web_in > 0 && has_pawn(board, p)) {
    Pawn& pawn = *first_occupant(board, p);
    if (!busy(ctx, pawn)) pawn.webbed = true;
  }
}

// One frame of Pawn::OnLoop, rule effects only.
void settle_pawn_frame(Board& board, Pawn& pawn, RulesContext& ctx) {
  if (pawn.fallen) return;
  if (pawn.mech && pawn.fire && board.has_passive(kPassiveFireBoost)) {
    set_pawn_fire(board, pawn, false);
    pawn.boosted = true;
  }
  if (pawn.fire && is_smoked(board, pawn, ctx)) set_pawn_fire(board, pawn, false);
  if (pawn.fire && is_submerged(board, pawn, ctx) && !detail::is_lava(board.tile(pawn.pos))) {
    set_pawn_fire(board, pawn, false);
  }
}

namespace detail {

void release_webs(Board& board, Point p) {
  if (!p.valid()) return;
  Tile& t = board.tile(p);
  const uint8_t out = t.web_out;
  t.web_out = 0;
  for (int d = 0; d < 4; ++d) {
    if (out >> d & 1) let_go_web(board, step(p, static_cast<Dir>(d)));
  }
}

}  // namespace detail

// ---- Pawn predicates ---------------------------------------------------------

bool is_vek(const Pawn& pawn) {
  return pawn.team == Team::Enemy && pawn.faction == Faction::Default;
}

// Pawn::IsPsionAffected for the plain mutations: Vek (and mechs under Psion
// Leech) take the board's mutation; psions themselves and minor pawns
// (including retreated ones) do not. The Psion Tyrant (8) affects every
// player-team pawn and Vek only under Psion Leech; the fire psion always
// reaches spider eggs and the spider psion never reaches spiderlings.
bool mutation_affects(const Board& board, const Pawn& pawn, Leader mutation) {
  if (mutation == Leader::None || board.psion != mutation) return false;
  if (mutation == Leader::Tentacle) {
    if (pawn.team == Team::Player) return true;
    return is_vek(pawn) && board.has_passive(kPassivePsionLeech);
  }
  if (mutation == Leader::Fire && detail::type_is(pawn, "SpiderlingEgg1")) return true;
  if (mutation == Leader::Spider && detail::type_is(pawn, "Spiderling1")) return false;
  if (!is_vek(pawn) && !(pawn.mech && board.has_passive(kPassivePsionLeech))) return false;
  // Pawn::IsPsionAffected: never a Minor pawn (+0x10F0, the Lua Minor flag,
  // which Retreat also sets).
  return pawn.leader == Leader::None && !pawn.minor;
}

bool is_corpse(const Board& board, const Pawn& pawn) {
  return ((pawn.mech || pawn.corpse) && !pawn.fallen) ||
         mutation_affects(board, pawn, Leader::Necro);
}

bool counts_as_pawn(const Board& board, const Pawn& pawn) {
  return !pawn.fallen && (pawn.alive() || is_corpse(board, pawn));
}

bool is_flying(const Pawn& pawn) {
  return (pawn.flying || pawn.has_pilot(kPilotFlying)) && pawn.alive();
}

bool is_massive(const Board& board, const Pawn& pawn) {
  return pawn.massive && (pawn.alive() || is_corpse(board, pawn));
}

bool is_armored(const Board& board, const Pawn& pawn) {
  const bool armor = mutation_affects(board, pawn, Leader::Armor) || pawn.armor ||
                     pawn.has_pilot(kPilotArmored);
  return armor && !pawn.acid;
}

bool is_fire_immune(const Board& board, const Pawn& pawn) {
  return pawn.has_pilot(kPilotThick) || pawn.has_pilot(kPilotRockSkill) ||
         (pawn.mech && board.has_passive(kPassiveFlameImmune)) || pawn.ignore_fire;
}

// The "my turn" flag is modelled as the player phase.
bool is_turn_shielded(const Board& board, const Pawn& pawn) {
  return board.has_passive(kPassivePlayerTurnShield) && pawn.mech && board.player_phase;
}

// ---- Occupants ----------------------------------------------------------------

std::vector<Pawn*> occupants(Board& board, Point p) {
  std::vector<Pawn*> out;
  for (Pawn& pawn : board.pawns()) {
    if (pawn.occupies(p) && !pawn.fallen) out.push_back(&pawn);
  }
  if (out.size() > 1) {
    std::stable_sort(out.begin(), out.end(),
                     [](const Pawn* a, const Pawn* b) { return a->arrival < b->arrival; });
  }
  return out;
}

bool has_pawn(const Board& board, Point p) {
  return std::any_of(board.pawns().begin(), board.pawns().end(), [&](const Pawn& pawn) {
    return pawn.occupies(p) && counts_as_pawn(board, pawn);
  });
}

Pawn* first_occupant(Board& board, Point p) {
  return const_cast<Pawn*>(first_occupant_const(board, p));
}

// ---- Pawn statuses ----------------------------------------------------------

void set_pawn_fire(Board& board, Pawn& pawn, bool on) {
  if (pawn.shield) {
    if (on) return;
  } else if (on) {
    pawn.infected = false;
    const bool boost_overrides = pawn.mech && board.has_passive(kPassiveFireBoost);
    if (is_fire_immune(board, pawn) && !boost_overrides) return;
  }
  pawn.fire = on;
  if (on) {
    pawn.infected = false;
    set_pawn_frozen(board, pawn, false);
  }
}

void set_pawn_frozen(Board& board, Pawn& pawn, bool on) {
  pawn.frozen = on;
  if (on) {
    set_pawn_fire(board, pawn, false);
    pawn.infected = false;
  }
}

void set_pawn_acid(Board& board, Pawn& pawn, bool on) {
  (void)board;
  if (on) {
    if (pawn.shield) return;
    pawn.infected = false;
    if (pawn.has_pilot(kPilotThick)) return;
  }
  pawn.acid = on;
}

void set_pawn_shield(Board& board, Pawn& pawn, bool on) {
  if (on) {
    if (pawn.alive()) pawn.shield = true;
  } else if (!is_turn_shielded(board, pawn)) {
    pawn.shield = false;
  }
}

void set_pawn_injured(Pawn& pawn, bool on) { pawn.injured = on; }

// ---- Tile statuses ----------------------------------------------------------

bool tile_shielded(const Board& board, Point p) {
  if (board.tile(p).shield) return true;
  return has_pawn(board, p) && first_occupant_const(board, p)->shield;
}

bool tile_frozen(const Board& board, Point p) {
  if (board.tile(p).frozen) return true;
  return has_pawn(board, p) && first_occupant_const(board, p)->frozen;
}

void set_tile_shield(Board& board, Point p, bool on, RulesContext& ctx) {
  (void)ctx;
  Tile& t = board.tile(p);
  if (on) {
    if (t.is_building() || t.is_mountain()) {
      t.shield = true;
    } else if (has_pawn(board, p)) {
      set_pawn_shield(board, *first_occupant(board, p), true);
    }
    return;
  }
  t.shield = false;
  if (has_pawn(board, p)) set_pawn_shield(board, *first_occupant(board, p), false);
}

void crit_defense_shields(Board& board, RulesContext& ctx) {
  for (int i = 0; i < kTileCount; ++i) {
    const Point p = Point::from_index(i);
    const Tile& t = board.tile(p);
    if (t.is_building() && (t.hp > 0 || t.unique_building != kNoSymbol)) set_tile_shield(board, p, true, ctx);
  }
}

void set_tile_frozen(Board& board, Point p, bool on, RulesContext& ctx) {
  Tile& t = board.tile(p);
  if (on && t.lava) return;
  Pawn* occ0 = has_pawn(board, p) ? first_occupant(board, p) : nullptr;
  // Freezing never turns the water under a dam into ice.
  const bool dam = occ0 && detail::type_is(*occ0, "Dam_Pawn");
  if (!dam && on && t.terrain == Terrain::Water) set_terrain(board, p, Terrain::Ice, ctx);
  if (t.terrain == Terrain::Ice) t.hp = t.max_hp = 2;  // refilled, thawing included
  if (on) set_tile_fire(board, p, false, ctx);
  if (t.is_building() || t.is_mountain()) {
    t.frozen = on;
  } else if (occ0) {
    set_pawn_frozen(board, *occ0, on);
  }
}

void set_tile_fire(Board& board, Point p, bool on, RulesContext& ctx) {
  Tile& t = board.tile(p);
  if (!on) {
    t.fire = FireState::None;
    return;
  }
  if (t.terrain == Terrain::Hole || t.terrain == Terrain::Water || t.on_fire()) return;
  t.smoke = false;
  t.vines = false;
  set_tile_frozen(board, p, false, ctx);
  set_tile_acid(board, p, false, ctx);
  if (t.terrain == Terrain::Forest) {
    t.fire = FireState::BurningForest;
    set_terrain(board, p, Terrain::Road, ctx);
  } else if (t.terrain == Terrain::Ice) {
    set_terrain(board, p, Terrain::Water, ctx);  // melts; water does not burn
    t.fire = FireState::None;
  } else {
    t.fire = FireState::Burning;
  }
}

void set_tile_smoke(Board& board, Point p, bool on, RulesContext& ctx) {
  Tile& t = board.tile(p);
  t.smoke = on;
  if (!on) return;
  set_tile_fire(board, p, false, ctx);
  t.vines = false;
  detail::release_webs(board, p);
}

void set_tile_acid(Board& board, Point p, bool on, RulesContext& ctx) {
  Tile& t = board.tile(p);
  if (t.lava) return;
  t.acid = on;
  if (on) set_tile_fire(board, p, false, ctx);
}

// ---- Terrain ----------------------------------------------------------------

bool is_populated(const Tile& tile) {
  if (tile.unique_building != kNoSymbol) return tile.hp >= tile.max_hp;
  return tile.populated && tile.is_building();
}

bool is_crackable(const Board& board, Point p) {
  const Tile& t = board.tile(p);
  if (has_pawn(board, p) && detail::type_is(*first_occupant_const(board, p), "SatelliteRocket")) {
    return false;
  }
  if (symbol_name(t.special_tag) == "supervolcano") return false;
  if (t.teleporter) return false;
  if (symbol_name(t.custom_tile).find("conveyor") != std::string_view::npos) return false;
  if (t.terrain == Terrain::Water) return false;  // lava included
  return t.terrain != Terrain::Building && t.terrain != Terrain::Hole;
}

void set_cracked(Board& board, Point p, bool on, RulesContext& ctx) {
  Tile& t = board.tile(p);
  if (!on) {
    t.cracked = false;
    return;
  }
  if (!is_crackable(board, p)) return;
  trigger_item(board, p, ctx);
  destroy_pod(t, p, ctx);
  // Mountains and ice take a hit instead of cracking.
  if (t.is_mountain() || t.terrain == Terrain::Ice) {
    damage_terrain(board, p, 1, ctx);
    return;
  }
  if (t.terrain == Terrain::Forest || t.terrain == Terrain::Sand ||
      t.terrain == Terrain::Rubble) {
    set_terrain(board, p, Terrain::Road, ctx);
  }
  set_tile_acid(board, p, false, ctx);
  set_tile_fire(board, p, false, ctx);
  t.cracked = true;
}

void set_terrain(Board& board, Point p, int terrain, RulesContext& ctx) {
  Tile& t = board.tile(p);
  switch (terrain) {
    case static_cast<int>(Terrain::Lava):
      t.lava = true;
      set_terrain(board, p, Terrain::Water, ctx);
      t.fire = FireState::None;
      return;
    case static_cast<int>(Terrain::Fire):
      set_tile_fire(board, p, true, ctx);
      return;
    case static_cast<int>(Terrain::Acid):
      set_tile_acid(board, p, true, ctx);
      return;
    case 15:  // spikes
      t.spikes = true;
      return;
    case 16:  // crack
      set_cracked(board, p, true, ctx);
      return;
    default:
      break;
  }
  // Real terrain ids only; 17 ("looper", cosmetic) and unknown ids are ignored.
  if (terrain < 0 || terrain > static_cast<int>(Terrain::Hole) || terrain == 8) return;

  const Terrain old = t.terrain;
  Terrain next = static_cast<Terrain>(terrain);
  if (next == Terrain::Building && old == Terrain::Water) t.building_on_water = true;
  if (next == Terrain::Rubble) {
    if (old == Terrain::Building) {
      // A building raised on water leaves water behind.
      if (t.building_on_water) {
        next = Terrain::Water;
      }
    } else if (old != Terrain::Mountain) {
      t.hp = 0;
    }
  }
  if (old == Terrain::Mountain && t.frozen &&
      (next == Terrain::Hole || next == Terrain::Water)) {
    set_tile_frozen(board, p, false, ctx);
  }
  t.terrain = next;
  if (next == Terrain::Mountain || next == Terrain::Ice) t.hp = t.max_hp = 2;
  if (next == Terrain::Water || next == Terrain::Hole) {
    destroy_pod(t, p, ctx);
    set_tile_fire(board, p, false, ctx);
    set_cracked(board, p, false, ctx);
    t.teleporter = false;
  }
  if (next != old) emit(ctx, RulesEventType::TerrainChanged, p, -1, static_cast<int>(next));
}

void damage_terrain(Board& board, Point p, int damage, RulesContext& ctx) {
  Tile& t = board.tile(p);
  if (t.terrain == Terrain::Ice) {
    // Ice loses 1 HP per hit, whatever the damage.
    t.hp = static_cast<int8_t>(std::max(0, t.hp - 1));
    emit(ctx, RulesEventType::TerrainDamaged, p, -1, 1);
    if (t.hp == 0) set_terrain(board, p, Terrain::Water, ctx);
  } else if (t.is_mountain()) {
    t.hp = static_cast<int8_t>(std::max(0, t.hp - 1));
    emit(ctx, RulesEventType::TerrainDamaged, p, -1, 1);
    if (t.hp == 0 || damage == kDamageDeath) {
      set_terrain(board, p, t.acid ? Terrain::Road : Terrain::Rubble, ctx);
    }
  }
}

void add_building(Board& board, Point p, RulesContext& ctx) {
  Tile& t = board.tile(p);
  if (t.is_building()) {
    if (t.hp >= t.max_hp) ++t.max_hp;
    t.hp = static_cast<int8_t>(std::min<int>(t.hp + 1, t.max_hp));
  } else {
    t.hp = t.max_hp = 1;
    set_terrain(board, p, Terrain::Building, ctx);
  }
  t.populated = true;
}

// ---- Items ----------------------------------------------------------------------

// The four items in items.lua (no other script defines one).
SpaceDamage item_damage(Symbol item) {
  const std::string_view name = symbol_name(item);
  SpaceDamage sd;
  if (name == "Item_Mine") {
    sd.damage = kDamageDeath;
  } else if (name == "Item_Repair_Mine") {
    sd.damage = -10;
  } else if (name == "Freeze_Mine") {
    sd.frozen = StatusChange::Apply;
  }
  // Supply_Drop: SpaceDamage(0).
  return sd;
}

void trigger_item(Board& board, Point p, RulesContext& ctx) {
  Tile& t = board.tile(p);
  if (t.item == kNoSymbol) return;
  const Symbol item = t.item;
  t.item = kNoSymbol;
  emit(ctx, RulesEventType::ItemTriggered, p, -1, 0, item);
  SpaceDamage sd = item_damage(item);
  sd.loc = p;
  damage_tile(board, p, sd, DamageMode::Weapon, ctx);
}

// ---- Continuous rules ----------------------------------------------------------

void check_acid_fire(Board& board, Point p, Pawn& pawn, RulesContext& ctx) {
  Tile& t = board.tile(p);
  // Nanofilter Mending: smoke on a mech is spent to heal it by 1.
  if (t.smoke && pawn.mech && board.has_passive(kPassiveHealingSmoke)) {
    set_tile_smoke(board, p, false, ctx);
    set_tile_fire(board, p, false, ctx);
    damage_tile(board, p, -1, DamageMode::Weapon, ctx);
    set_pawn_fire(board, pawn, false);
  }
  if (!pawn.alive() || pawn.shield) return;
  const bool flying = is_flying(pawn);
  if (t.on_fire() || (t.lava && !flying)) {
    set_pawn_fire(board, pawn, true);
  } else if (t.acid && t.terrain != Terrain::Ice) {
    // Acid water spares flyers and is never used up.
    if (!(flying && t.terrain == Terrain::Water)) {
      set_pawn_acid(board, pawn, true);
      if (t.terrain != Terrain::Water && !t.lava) t.acid = false;
    }
  } else if (t.spikes) {
    set_pawn_injured(pawn, true);
  }
}

void check_terrain_dangers(Board& board, Point p, RulesContext& ctx) {
  Tile& t = board.tile(p);
  for (Pawn* pawn : occupants(board, p)) {
    // Busy pawns (other than mid-push) only see their bodies sink or drop.
    if (busy(ctx, *pawn, /*ignore_push=*/true)) {
      if (pawn->alive()) continue;
      if (t.terrain == Terrain::Water) {
        kill_pawn_instant(board, *pawn, ctx);
      } else if (t.terrain == Terrain::Hole && !detail::type_contains(*pawn, "Train_")) {
        fall_pawn(board, *pawn, ctx);
      }
      continue;
    }
    if (t.terrain == Terrain::Water) {
      if (is_flying(*pawn) && !pawn->frozen) continue;
      if (!is_massive(board, *pawn)) {
        // Drowns: the living, and the dying that leave no corpse, but only
        // once idle. A pawn still being pushed does not sink where it
        // stands: a hit that breaks cracked ice under a unit and pushes it
        // off sends it to the next tile alive.
        const bool alive = pawn->alive();
        if (!busy(ctx, *pawn) && (alive || !is_corpse(board, *pawn)) && (alive || pawn->dying)) {
          if (detail::type_contains(*pawn, "Train_")) {
            kill_pawn(board, *pawn, ctx);
            pawn->corpse = false;
          } else {
            kill_pawn_instant(board, *pawn, ctx);
          }
        }
      } else {
        if (pawn->frozen && !detail::type_is(*pawn, "Dam_Pawn")) {
          set_pawn_frozen(board, *pawn, false);
        }
        // An ACID-covered massive unit turns the water acidic.
        if (pawn->acid && !t.lava) {
          t.acid = true;
          set_tile_fire(board, p, false, ctx);
        }
      }
    } else if (t.terrain == Terrain::Hole) {
      if (detail::type_contains(*pawn, "Train_")) {
        kill_pawn(board, *pawn, ctx);
        pawn->corpse = false;
      } else {
        fall_pawn(board, *pawn, ctx);
      }
    } else if (t.item == kNoSymbol) {
      // Fire Boost (Heat Engines): a mech standing in fire absorbs it.
      if (pawn->mech && t.on_fire() && board.has_passive(kPassiveFireBoost)) {
        set_tile_fire(board, p, false, ctx);
        set_pawn_fire(board, *pawn, false);
        pawn->boosted = true;
        pawn->infected = false;
        set_pawn_frozen(board, *pawn, false);
      }
    } else {
      trigger_item(board, p, ctx);
    }
  }
}

int settle_at(Board& board, Point p, RulesContext& ctx) {
  constexpr int kMaxPasses = 16;
  for (int pass = 1; pass <= kMaxPasses; ++pass) {
    const Board before = board;
    settle_tile_frame(board, p, ctx);
    for (Pawn* pawn : occupants(board, p)) settle_pawn_frame(board, *pawn, ctx);
    if (board == before) return pass;
  }
  return kMaxPasses;
}

int settle(Board& board, RulesContext& ctx) {
  constexpr int kMaxPasses = 16;
  for (int pass = 1; pass <= kMaxPasses; ++pass) {
    const Board before = board;
    for (int i = 0; i < kTileCount; ++i) settle_tile_frame(board, Point::from_index(i), ctx);
    for (size_t i = 0; i < board.pawns().size(); ++i) settle_pawn_frame(board, board.pawns()[i], ctx);
    if (board == before) return pass;
  }
  return kMaxPasses;
}

}  // namespace itb
