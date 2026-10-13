#include "itb/movement.hpp"

#include "itb/pilot_xp.hpp"

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <functional>

namespace itb {
namespace {

// The A* heuristic weight, 1.01 in single precision (0x3f8147ae). Path
// choice depends on the exact float arithmetic: the engine is built with
// -ffp-contract=off so h * w + g is two roundings, as in the game.
constexpr float kHeuristicWeight = 1.01f;
static_assert(std::bit_cast<uint32_t>(kHeuristicWeight) == 0x3f8147aeu);

// Enough heap room for any search: a node is expanded at most once and
// pushes at most 4 neighbours, plus the start.
constexpr int kHeapCapacity = 4 * kTileCount + 1;

int type_of(Pathing pr) { return static_cast<int>(pr.type); }

bool is_conveyor(const Tile& t) {
  if (t.conveyor >= 0) return true;
  return t.custom_tile != kNoSymbol &&
         symbol_name(t.custom_tile).find("conveyor") != std::string_view::npos;
}

// What the pathing rules need to know about a tile's occupant list.
struct Occupant {
  bool listed = false;    // some pawn stands here (alive or not)
  bool occupied = false;  // IsPawnSpace: some pawn here is alive or a corpse
  int team = static_cast<int>(Team::None);  // first listed pawn's team
  bool non_grid = false;  // first listed pawn is NonGrid
  uint32_t arrival = 0;   // the first listed pawn's tile order
};

void note_pawn(const Board& b, const Pawn& p, Occupant& o) {
  if (!o.listed || p.arrival < o.arrival) {
    o.listed = true;
    o.team = static_cast<int>(p.team);
    o.non_grid = p.non_grid;
    o.arrival = p.arrival;
  }
  if (p.alive() || is_corpse(b, p)) o.occupied = true;
}

Occupant occupant_at(const Board& b, Point at) {
  Occupant o;
  for (const Pawn& p : b.pawns()) {
    if (p.occupies(at)) note_pawn(b, p, o);
  }
  return o;
}

using Occupancy = std::array<Occupant, kTileCount>;

Occupancy occupancy(const Board& b) {
  Occupancy occ{};
  for (const Pawn& p : b.pawns()) {
    if (p.pos.valid()) note_pawn(b, p, occ[p.pos.index()]);
    if (const Point e = p.extra_tile(); e.valid()) note_pawn(b, p, occ[e.index()]);
  }
  return occ;
}

// BoardSpace::GetPawnTeam: TEAM_NONE when unoccupied, else the first listed
// pawn's team (even if that pawn is a dead non-corpse).
int tile_team(const Occupant& o) { return o.occupied ? o.team : static_cast<int>(Team::None); }

// BoardSpace::IsWalkable.
bool passable(const Tile& tile, const Occupant& o, Pathing pr) {
  const int t = type_of(pr);
  // Flyers, teleporters, jumpers and burrowers cross anything.
  if (t == 1 || t == 5 || t == 6 || t == 7) return true;
  const Terrain terrain = tile.terrain;
  // Road_Runner ignores pawns entirely, and wades.
  if (t == 4) {
    return terrain != Terrain::Building && terrain != Terrain::Mountain &&
           terrain != Terrain::Hole;
  }
  if (t == 3) {
    if (terrain == Terrain::Building || terrain == Terrain::Mountain) return false;
    return !o.occupied;
  }
  // Ground, massive, phasing and the internal final-step profile.
  if (o.occupied && pr.team != tile_team(o)) return false;
  if (o.occupied && o.non_grid) return false;
  if (terrain == Terrain::Building) return false;
  if (terrain == Terrain::Water && t != 2) return false;  // only massive wades (lava too)
  return terrain != Terrain::Mountain && terrain != Terrain::Hole;
}

// BoardSpace::IsTerrainBlocking.
bool terrain_blocking(const Tile& tile, Pathing pr) {
  const int t = type_of(pr);
  if (t == 7 && is_conveyor(tile)) return true;
  switch (tile.terrain) {
    case Terrain::Building:
      return t != 9;
    case Terrain::Water:  // raw terrain, so lava too
      return t == 0 || t == 7;
    case Terrain::Mountain:
      return true;
    case Terrain::Ice:
      return t == 7;
    case Terrain::Hole:
      if (t == 3 || t == 8 || t == 9) return false;
      return (t & 0xB) != 1;  // only flyers and teleporters hover over a chasm
    default:
      return false;
  }
}

// Board::IsBlocked for a valid tile.
bool blocked(const Tile& tile, const Occupant& o, Pathing pr) {
  if (terrain_blocking(tile, pr)) return true;
  if (!o.occupied) return false;
  // Only an exact PATH_PHASING (team 0) may end on an occupied tile, and then
  // only if it is not a Vek's.
  if (pr.raw() == static_cast<int>(PathProfile::Phasing)) {
    return tile_team(o) == static_cast<int>(Team::Enemy);
  }
  return true;
}

// Board::IsWall: walls stop everything except flyers, projectiles,
// teleporters and jumpers.
bool wall_stops(const Tile& from, Dir d, Pathing pr) {
  if (!from.has_wall(d)) return false;
  const int t = type_of(pr);
  return !((t & 0xD) == 1 || t == 5 || t == 6);
}

int enter_cost(const Tile& tile, Pathing pr) {
  return pr.raw() == static_cast<int>(PathProfile::Projectile) &&
                 tile.terrain == Terrain::Building
             ? 1000
             : 1;
}

// A fixed-capacity binary min-heap of packed keys. Keys pack the comparison
// tuple with the tile index in the low 6 bits, so ties break on (x, y).
template <typename Key>
class MinHeap {
 public:
  bool empty() const { return size_ == 0; }
  void push(Key k) {
    data_[size_++] = k;
    std::push_heap(data_.begin(), data_.begin() + size_, std::greater<Key>());
  }
  Key pop() {
    std::pop_heap(data_.begin(), data_.begin() + size_, std::greater<Key>());
    return data_[--size_];
  }

 private:
  std::array<Key, kHeapCapacity> data_{};
  int size_ = 0;
};

DistanceMap distances(const Board& b, const Occupancy& occ, Point start, Pathing pr,
                      int max_dist) {
  DistanceMap dist;
  dist.fill(kUnreached);
  if (!start.valid()) return dist;
  dist[start.index()] = 0;
  // Native: an ordered set of (cost, x, y). Stale entries are erased there;
  // here they are skipped on pop, which is equivalent.
  MinHeap<uint32_t> open;
  open.push(static_cast<uint32_t>(start.index()));
  while (!open.empty()) {
    const uint32_t key = open.pop();
    const int ci = static_cast<int>(key & 63u);
    const int cd = static_cast<int>(key >> 6);
    if (cd != dist[ci]) continue;
    // The whole search stops at the first node at or beyond max_dist, so
    // nodes at exactly max_dist are labelled but never expanded.
    if (max_dist <= cd) break;
    const Point cur = Point::from_index(ci);
    const Tile& cur_tile = b.tile(cur);
    for (int d = 0; d < 4; ++d) {
      const Point nb = cur + kDirVectors[d];
      if (wall_stops(cur_tile, static_cast<Dir>(d), pr)) continue;
      if (!nb.valid() || !passable(b.tile(nb), occ[nb.index()], pr)) continue;
      // The step out of the start always costs 1.
      const int c = cd + (cur == start ? 1 : enter_cost(b.tile(nb), pr));
      if (c < dist[nb.index()]) {
        dist[nb.index()] = c;
        open.push((static_cast<uint32_t>(c) << 6) | static_cast<uint32_t>(nb.index()));
      }
    }
  }
  return dist;
}

TileMask reachable_mask(const Board& b, const Occupancy& occ, Point start, int max_dist,
                        Pathing pr) {
  if (!start.valid()) return 0;
  const DistanceMap dist = distances(b, occ, start, pr, max_dist);
  TileMask out = 0;
  for (int i = 0; i < kTileCount; ++i) {
    if (dist[i] <= max_dist && !blocked(b.tile(Point::from_index(i)), occ[i], pr)) {
      out |= TileMask{1} << i;
    }
  }
  return out;
}

TileMask reachable_list_mask(const Board& b, const Occupancy& occ, Point start, int max_dist,
                             Pathing pr) {
  TileMask pts = reachable_mask(b, occ, start, max_dist, pr);
  if (pr.raw() == static_cast<int>(PathProfile::Projectile)) return pts;
  // Jumpers may not choose to land in water or lava. (The IsBlocked re-check
  // the game does here is already part of reachable_mask.)
  if (pr.type == PathProfile::Jumper) {
    for (int i = 0; i < kTileCount; ++i) {
      if (b.tile(Point::from_index(i)).terrain == Terrain::Water) pts &= ~(TileMask{1} << i);
    }
  }
  return pts;
}

void step_to(Board& b, Pawn& p, Point to, RulesContext* ctx, bool no_injury = false) {
  shift_queued_shot(p, to - p.pos);
  set_space(b, p, to, no_injury, ctx);
}

Point settled(Board& b, Point end, RulesContext* ctx) {
  if (ctx && end.valid()) settle_at(b, end, *ctx);
  return end;
}

}  // namespace

std::vector<Point> mask_points(TileMask mask) {
  std::vector<Point> out;
  out.reserve(static_cast<size_t>(std::popcount(mask)));
  while (mask != 0) {
    const int i = std::countr_zero(mask);
    out.push_back(Point::from_index(i));
    mask &= mask - 1;
  }
  return out;
}

// --- Pawn-level facts ----------------------------------------------------


Pathing path_profile(const Board& b, const Pawn& p) {
  const int team = static_cast<int>(p.team);
  const bool alive = p.alive();
  if (p.teleporter && alive) return {PathProfile::Teleporter, team};
  if (p.jumper && alive) return {PathProfile::Jumper, team};
  if (p.burrows) return {PathProfile::Burrower, team};
  if ((p.flying || p.has_pilot(kPilotFlying)) && alive) return {PathProfile::Flyer, team};
  if (p.has_pilot(kPilotRoadRunner)) return {PathProfile::RoadRunner, team};
  const bool massive = alive ? p.massive : (p.massive && is_corpse(b, p));
  if (massive) return {PathProfile::Massive, team};
  return {PathProfile::Ground, team};
}

// --- Move budget ---------------------------------------------------------

int live_move_modifiers(const Board& b, const Pawn& p) {
  int move = 0;
  if (p.movement.turn_count <= 1 && p.has_pilot(kPilotYouthMove)) move += 3;
  // Level-up skills (Move, Skilled, Opener, Closer, Pain); Adrenaline's kill
  // count lives in pilot_bonus.
  move += level_skill_move(b, p);
  if (p.has_pilot(kPilotArrogantBoost) && p.hp < p.max_hp) move -= 1;
  return move;
}

int base_move(const Board& b, const Pawn& p) {
  const MoveState& m = p.movement;
  int move = p.move + m.pilot_bonus + m.kickoff_bonus + (m.move_upgrade ? 1 : 0);
  move += live_move_modifiers(b, p);
  if (m.reset_bonus) move += 2;
  return std::max(move, 0);
}

int move_speed(const Board& b, const Pawn& p) {
  if (!p.alive()) return 0;
  if (p.webbed) return 0;
  if (!p.movement.powered) return 0;
  if (p.movement.bonus_shift > 0) return p.movement.bonus_shift;
  return base_move(b, p);
}

bool can_move(const Pawn& p) {
  if (p.frozen) return false;
  if (!(p.active && p.alive() && p.movement.powered)) return false;
  if (p.webbed) return false;
  if (p.moved && p.movement.bonus_shift <= 0) return false;
  // The raw MoveSpeed, not the budget: base 0 plus a bonus still can't move.
  return p.move > 0;
}

bool is_moved(const Pawn& p) { return p.moved && p.movement.bonus_shift < 1; }

bool can_act(const Board& b, const Pawn& p) {
  if (!(p.active && p.alive() && p.movement.powered)) return false;
  if (p.movement.bonus_shift > 0) return false;  // a pending bonus move: move only
  if (!p.pos.valid()) return true;
  const Tile& t = b.tile(p.pos);
  // Pawn::IsSmoked / IsSubmerged with nothing busy (tile_status.cpp).
  if (t.smoke && !p.ignore_smoke && !p.has_pilot(kPilotDisableImmunity)) return false;
  if (t.terrain == Terrain::Water && !is_flying(p)) return false;
  return true;
}

void on_skill_fired(Board& b, Pawn& p, bool move_skill) {
  MoveState& m = p.movement;
  if (move_skill) {
    if (!p.moved) {
      p.moved = true;
      if (m.bonus_shift > 0) {  // a bonus move ends the pawn's turn
        m.bonus_shift = 0;
        p.active = false;
      }
      return;
    }
    if (m.bonus_shift > 0) {
      m.bonus_shift = 0;
      p.active = false;
      return;
    }
    // Moving again with no bonus left is booked like a weapon (never happens
    // through the UI).
  }
  // Any other skill ends the pawn's actions, then pilots may grant more.
  p.boosted = false;
  p.active = false;
  if (p.team == Team::Player) {
    for (Pawn& other : b.pawns()) other.movement.undo_ready = false;
  }
  if (p.has_pilot(kPilotShifty)) {
    m.bonus_shift = 1;
    p.active = true;
  } else if (p.has_pilot(kPilotDoubleShot) && !p.moved) {
    p.moved = true;  // may fire again but no longer move
    p.active = true;
  } else if (p.has_pilot(kPilotPostMove)) {
    m.bonus_shift = static_cast<int8_t>(base_move(b, p));
    p.active = true;
  }
}

void start_turn_movement(Board& b, int32_t uid) {
  Pawn* p = b.find_pawn(uid);
  if (!p) return;
  MoveState& m = p->movement;
  ++m.turn_count;
  p->moved = false;
  m.bonus_shift = 0;
  m.reset_bonus = false;
  if (!p->mech || !b.has_passive(kPassiveKickoff)) return;
  const int8_t bonus = b.has_passive(kPassiveKickoffUpgraded) ? 2 : 1;
  for (Point d : kDirVectors) {
    const Point nb = p->pos + d;
    if (!nb.valid() || !tile_occupied(b, nb)) continue;
    // The neighbour gets the bonus. It is assigned, so two adjacent mechs
    // still give +1.
    Pawn* q = b.pawn_at(nb);
    if (q && q->mech) q->movement.kickoff_bonus = bonus;
  }
}

void end_turn_movement(Pawn& p) { p.movement.kickoff_bonus = 0; }

// --- Tile predicates -----------------------------------------------------

bool tile_occupied(const Board& b, Point p) { return p.valid() && occupant_at(b, p).occupied; }

bool wall_blocks(const Board& b, Point from, Dir d, Pathing pr) {
  return from.valid() && is_cardinal(d) && wall_stops(b.tile(from), d, pr);
}

bool can_pass(const Board& b, Point p, Pathing pr) {
  return p.valid() && passable(b.tile(p), occupant_at(b, p), pr);
}

bool terrain_blocks(const Board& b, Point p, Pathing pr) {
  return p.valid() && terrain_blocking(b.tile(p), pr);
}

bool is_blocked(const Board& b, Point p, Pathing pr) {
  return !p.valid() || blocked(b.tile(p), occupant_at(b, p), pr);
}

int step_cost(const Board& b, Point p, Pathing pr) { return enter_cost(b.tile(p), pr); }

// --- Searches ------------------------------------------------------------

DistanceMap distance_map(const Board& b, Point start, Pathing pr, int max_dist) {
  return distances(b, occupancy(b), start, pr, max_dist);
}

TileMask reachable_points(const Board& b, Point start, int max_dist, Pathing pr) {
  return reachable_mask(b, occupancy(b), start, max_dist, pr);
}

TileMask reachable_list(const Board& b, Point start, int max_dist, Pathing pr) {
  return reachable_list_mask(b, occupancy(b), start, max_dist, pr);
}

TileMask move_area(const Board& b, const Pawn& p) {
  return reachable_list(b, p.pos, move_speed(b, p), path_profile(b, p));
}

std::vector<Point> reachable(const Board& b, const Pawn& p) { return mask_points(move_area(b, p)); }

std::vector<Point> ai_move_candidates(const Board& b, const Pawn& p) {
  // Pawn::GetMoveOrigin: an underground burrower moves from where it dived.
  const Point origin = (p.burrows && !p.pos.valid()) ? p.movement.prev_pos : p.pos;
  std::vector<Point> out =
      mask_points(reachable_points(b, origin, move_speed(b, p), path_profile(b, p)));
  out.push_back(p.pos);
  return out;
}

std::vector<Point> find_path(const Board& b, Point from, Point to, Pathing pr) {
  if (from == to || !from.valid() || !to.valid()) return {};
  const Occupancy occ = occupancy(b);
  std::array<int, kTileCount> g{};
  std::array<int8_t, kTileCount> came;
  came.fill(-1);
  TileMask opened = TileMask{1} << from.index();
  TileMask closed = 0;
  // Native: a binary heap whose top is the smallest (f, x, y), f in float32.
  // f >= 0, so the float's bit pattern orders like the value. A node popped
  // again after closing would re-expand without effect, so it is skipped.
  MinHeap<uint64_t> open;
  open.push(static_cast<uint64_t>(from.index()));
  while (!open.empty()) {
    const int ci = static_cast<int>(open.pop() & 63u);
    const Point cur = Point::from_index(ci);
    if (cur == to) {
      std::vector<Point> path;
      for (int i = ci; i != -1 && i != from.index(); i = came[i]) {
        path.push_back(Point::from_index(i));
      }
      path.push_back(from);
      std::reverse(path.begin(), path.end());
      return path;
    }
    const TileMask cur_bit = TileMask{1} << ci;
    if (closed & cur_bit) continue;
    closed |= cur_bit;
    const Tile& cur_tile = b.tile(cur);
    for (int d = 0; d < 4; ++d) {
      const Point nb = cur + kDirVectors[d];
      if (wall_stops(cur_tile, static_cast<Dir>(d), pr)) continue;
      // The goal itself may be impassable (e.g. an attack target).
      if (nb != to && (!nb.valid() || !passable(b.tile(nb), occ[nb.index()], pr))) continue;
      const int ni = nb.index();
      const TileMask nb_bit = TileMask{1} << ni;
      if (closed & nb_bit) continue;
      const int tg = g[ci] + (cur == from ? 1 : enter_cost(b.tile(nb), pr));
      if ((opened & nb_bit) && g[ni] <= tg) continue;
      came[ni] = static_cast<int8_t>(ci);
      const int h = std::abs(to.x - nb.x) + std::abs(to.y - nb.y);
      const float weighted_h = static_cast<float>(h) * kHeuristicWeight;
      const float f = weighted_h + static_cast<float>(tg);
      open.push((static_cast<uint64_t>(std::bit_cast<uint32_t>(f)) << 6) |
                static_cast<uint64_t>(ni));
      opened |= nb_bit;
      g[ni] = tg;
    }
  }
  return {};
}

// --- Executing moves -----------------------------------------------------

void shift_queued_shot(Pawn& p, Point delta) {
  if (!p.queued.active()) return;
  if (p.queued.origin.valid()) p.queued.origin = p.queued.origin + delta;
  p.queued.target = p.queued.target + delta;
}

void set_space(Board& b, Pawn& p, Point to, bool no_injury, RulesContext* ctx) {
  // Pawn::SetSpace (0087dcb0) moves the ExtraSpaces tiles with the main one
  // (Board::MovePawn per tile); here they follow `pos` (Pawn::extra_tile).
  const Point from = p.pos;
  if (p.pos.valid()) p.movement.prev_pos = p.pos;
  // Any relocation frees a webbed pawn (the flag only: the webs stay on the
  // tiles until the emitters' web check sees the held tile empty).
  p.webbed = false;
  p.pos = to;
  if (to != from) b.stamp_arrival(p);
  // AE Injured: 1 HP per tile change while not busy. Whether every step of a
  // walk counts is unconfirmed in game (spec open question 2); this follows
  // the code, which charges each step.
  if (!no_injury && p.injured && to != p.movement.prev_pos && p.hp > 0) {
    if (ctx) {
      // Pawn::ModifyHealth(-1): turn shield, burrow dive and Retaliation apply.
      modify_health(b, p, -1, DamageMode::Weapon, *ctx);
      if (!p.alive() && !p.dying) {
        p.dying = true;
        if (ctx->events) ctx->events->push_back({RulesEventType::PawnKilled, p.pos, p.uid});
      }
    } else {
      p.hp = static_cast<int8_t>(p.hp - 1);
      if (p.hp <= 0) p.dying = true;
    }
  }
}

bool final_step_allowed(const Board& b, Point to) {
  return !is_blocked(b, to, Pathing::lua(PathProfile::FinalStep)) && !tile_occupied(b, to);
}

Point walk_path(Board& b, Pawn& p, const std::vector<Point>& path, bool forced,
                RulesContext* ctx) {
  // A pawn that holds its ground (Pushable = false) ignores unforced walks.
  if (!forced && !p.pushable) return p.pos;
  size_t i = (!path.empty() && path.front() == p.pos) ? 1 : 0;
  for (; i < path.size(); ++i) {
    const Point target = path[i];
    const Point delta = target - p.pos;
    if (delta.x * delta.x + delta.y * delta.y > 1) {
      // A non-adjacent step makes the game re-path from where it stands and
      // drop the rest of the queue. Board paths are always contiguous, so
      // this is only an approximation of that fallback.
      return walk_path(b, p, find_path(b, p.pos, target, path_profile(b, p)), true, ctx);
    }
    const bool last = i + 1 == path.size();
    // The final step re-checks its tile: no mountain/building and nobody
    // there (water and chasms are fine). If refused, the pawn stays put, even
    // on a friendly's tile it was passing through.
    if (last && !final_step_allowed(b, target)) break;
    step_to(b, p, target, ctx);
    // Only the tile where movement ends sees hazards: the final step checks
    // its terrain dangers at once (fire and acid wait for the tile rules).
    if (last && ctx) check_terrain_dangers(b, target, *ctx);
  }
  return p.pos;
}

Point leap(Board& b, Pawn& p, Point to, bool forced, RulesContext* ctx) {
  if (!forced && !p.pushable) return p.pos;
  // The pawn is on `to` at once; the arc is animation. Landing in a chasm
  // falls via the stage 2 rules.
  step_to(b, p, to, ctx);
  return p.pos;
}

Point charge(Board& b, Pawn& p, Point to, bool forced, RulesContext* ctx) {
  return leap(b, p, to, forced, ctx);
}

Point teleport(Board& b, Pawn& p, Point to, RulesContext* ctx) {
  // (Board::ClearGrapple here only affects grapple visuals.)
  step_to(b, p, to, ctx);
  return p.pos;
}

Point burrow(Board& b, Pawn& p, Point to, bool ai, RulesContext* ctx) {
  if (ai) {
    p.queued = QueuedShot{};
  } else {
    shift_queued_shot(p, to - p.pos);
  }
  p.fire = false;  // burrowing puts the fire out
  // Underground pawns relocate at once; surfaced ones dive first and are
  // relocated while still busy (so Injured does not apply).
  const bool underground = !p.pos.valid();
  set_space(b, p, to, /*no_injury=*/!underground, ctx);
  return p.pos;
}

Point move_pawn(Board& b, int32_t uid, Point dest, RulesContext* ctx) {
  Pawn* p = b.find_pawn(uid);
  if (!p) return kInvalidPoint;
  const Pathing pr = path_profile(b, *p);
  if (pr.type == PathProfile::Jumper) return settled(b, leap(b, *p, dest, true, ctx), ctx);
  if (pr.type == PathProfile::Teleporter) return settled(b, teleport(b, *p, dest, ctx), ctx);
  // Burrowers walk too: only the AI's ManualMove burrows.
  return settled(b, walk_path(b, *p, find_path(b, p->pos, dest, pr), true, ctx), ctx);
}

std::optional<MoveUndo> player_move(Board& b, int32_t uid, Point dest, RulesContext* ctx) {
  Pawn* p = b.find_pawn(uid);
  if (!p || !can_move(*p) || !mask_has(move_area(b, *p), dest)) return std::nullopt;
  MoveUndo u;
  u.uid = uid;
  u.from = p->pos;
  u.to = dest;
  u.to_tile = b.tile(dest);
  u.hp = p->hp;
  u.bonus_shift = p->movement.bonus_shift;
  u.fire = p->fire;
  u.frozen = p->frozen;
  u.acid = p->acid;
  u.shield = p->shield;
  u.boosted = p->boosted;
  u.injured = p->injured;
  u.infected = p->infected;
  p->movement.undo_ready = true;
  u.end = move_pawn(b, uid, dest, ctx);
  on_skill_fired(b, *p, /*move_skill=*/true);
  return u;
}

bool undo_move(Board& b, const MoveUndo& u, RulesContext* ctx) {
  Pawn* p = b.find_pawn(u.uid);
  if (!p || !p->movement.undo_ready || p->team != Team::Player || p->neutral) return false;
  // The pawn ends up active whatever happened (a bonus move that ended its
  // turn is handed back below via the saved bonus shift).
  p->active = true;
  p->hp = u.hp;
  p->dying = p->hp <= 0;
  p->fire = u.fire;
  p->frozen = u.frozen;
  p->acid = u.acid;
  p->shield = u.shield;
  p->boosted = u.boosted;
  p->injured = u.injured;
  p->infected = u.infected;
  // Natively the relocation back is a plain SetSpace after the statuses are
  // restored, so an Injured pawn pays 1 HP for the undo too (unverified).
  set_space(b, *p, u.from, false, ctx);
  b.tile(u.to) = u.to_tile;
  p->moved = false;
  p->movement.undo_ready = false;
  p->movement.bonus_shift = u.bonus_shift;
  return true;
}

Point ai_move(Board& b, int32_t uid, Point dest, RulesContext* ctx) {
  Pawn* p = b.find_pawn(uid);
  if (!p) return kInvalidPoint;
  if (!can_move(*p)) return p->pos;
  const Pathing pr = path_profile(b, *p);
  if (p->burrows) return settled(b, burrow(b, *p, dest, /*ai=*/true, ctx), ctx);
  if (p->jumper && p->alive()) return settled(b, leap(b, *p, dest, true, ctx), ctx);
  return settled(b, walk_path(b, *p, find_path(b, p->pos, dest, pr), true, ctx), ctx);
}

}  // namespace itb
