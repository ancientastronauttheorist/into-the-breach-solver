// The C++ weapon ports (weapon_ports.hpp). Each mirrors one Lua function of
// the game's scripts, named by file and line, statement by statement: the
// comments give the Lua being mirrored in short. Lua errors (indexing nil,
// a binding called with the wrong types) are not reproduced: a port returns
// false wherever the Lua would raise, and the call then runs in Lua.
//
// Conventions: DIR_VECTORS[d] is only defined for d in 0..3 (any other index
// is nil, and arithmetic on nil raises); GetDirection returns 4 (DIR_NONE)
// for a zero vector; numbers concatenate as tostring formats them.

#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

#include "lua_native.hpp"
#include "weapon_ports.hpp"

extern "C" {
#include "lua.h"
}

namespace itb::lua {
namespace {

namespace n = native;
using SD = LuaSpaceDamage;
using SE = LuaSkillEffect;

constexpr int kTeamPlayer = 1;
constexpr int kTeamMech = 4;
constexpr int kTeamEnemy = 6;
constexpr int kEffectCreate = 1;
constexpr int kEffectRemove = 2;
constexpr int kPathGround = 0;
constexpr int kPathProjectile = 3;
constexpr int kPathPhasing = 9;
constexpr int kPathFlyer = 1;
constexpr int kDirFlip = 6;

bool is_dir(int d) { return d >= 0 && d <= 3; }
Point vec(int d) { return kDirVectors[static_cast<size_t>(d)]; }
std::string num(int v) { return n::number_string(static_cast<double>(v)); }

// ---- weapons_base.lua -------------------------------------------------------------

// Skill:GetTargetArea(point):
//   return Board:GetSimpleReachable(point, self.PathSize, self.CornersAllowed)
bool skill_area(PortContext& c, const Fields& f, Point point, std::vector<Point>& out) {
  out = n::simple_reachable(c.board, point, f[0].as_int(), f[1].b);
  return true;
}

// SelfTarget:GetTargetArea(point): { point }
bool self_target_area(PortContext&, const Fields&, Point point, std::vector<Point>& out) {
  out.assign(1, point);
  return true;
}

// Move:GetTargetArea(point):
//   return Board:GetReachable(point, Pawn:GetMoveSpeed(), Pawn:GetPathProf())
bool move_area(PortContext& c, const Fields&, Point point, std::vector<Point>& out) {
  if (!c.pawn) return false;
  out = n::reachable(c.board, point, n::move_speed(c.board, *c.pawn), n::path_prof(c.board, *c.pawn));
  return true;
}

// Move:GetSkillEffect(p1, p2): a leap (AddLeap(p1, p2, ...) has no such
// overload: Lua raises), a teleport or a walk along GetPath; then the Web_Vek
// and Adjacent_Heal pawn abilities.
bool move_effect(PortContext& c, const Fields&, Point p1, Point p2, SE& ret) {
  if (!c.pawn) return false;
  const Board& b = c.board;
  const Pawn& pawn = *c.pawn;
  if (n::is_jumper(pawn)) return false;
  if (n::is_teleporter(pawn)) {
    n::add_teleport(ret, p1, p2, kFullDelay);
  } else {
    n::add_move(ret, false, n::path(b, p1, p2, n::path_prof(b, pawn)), kFullDelay, 0);
  }
  if (n::is_ability(pawn, "Web_Vek")) {
    for (int i = 0; i <= 3; ++i) {
      const Point curr = p2 + vec(i);
      if (n::is_pawn_space(b, curr) && static_cast<int>(n::pawn_at(b, curr)->team) == kTeamEnemy) {
        n::add_grapple(ret, p2, curr, "hold");
      }
    }
  }
  if (n::is_ability(pawn, "Adjacent_Heal")) {
    for (int i = 0; i <= 3; ++i) {
      const Point curr = p2 + vec(i);
      if (n::is_pawn_space(b, curr) && static_cast<int>(n::pawn_at(b, curr)->team) == kTeamPlayer &&
          n::pawn_at(b, curr)->uid != pawn.uid) {
        n::add_damage(ret, n::space_damage(curr, -1));
      }
    }
  }
  return true;
}

// LineArtillery:GetTargetArea(point): along each direction from 2 to
// self.ArtillerySize tiles while on the board; with self.OnlyEmpty only the
// tiles not blocked for ground units.
bool line_artillery_area(PortContext& c, const Fields& f, Point point, std::vector<Point>& out) {
  const int size = f[0].as_int();
  const bool only_empty = f[1].truthy();
  for (int dir = 0; dir <= 3; ++dir) {
    for (int i = 2; i <= size; ++i) {
      const Point curr = point + n::mul(vec(dir), i);
      if (!curr.valid()) break;
      if (!only_empty || !n::is_blocked(c.board, curr, kPathGround)) out.push_back(curr);
    }
  }
  return true;
}

// ---- weapons_ranged.lua -----------------------------------------------------------

// Ranged_Rocket:GetSkillEffect(p1, p2): smoke behind the shooter, an
// artillery shot that pushes, two bounces.
bool rocket_effect(PortContext&, const Fields& f, Point p1, Point p2, SE& ret) {
  const int direction = n::direction(p2 - p1);
  if (!is_dir(direction)) return false;  // DIR_VECTORS[4] is nil
  n::add_bounce(ret, p1, 1);
  SD smoke = n::space_damage(p1 - vec(direction), 0);
  smoke.iSmoke = 1;
  smoke.sAnimation = "exploout0_" + num(n::direction(p1 - p2));
  n::add_damage(ret, smoke);
  SD damage = n::space_damage(p2, f[0].as_int());  // self.Damage
  damage.iPush = direction;
  damage.iSmoke = f[1].as_int();  // self.Smoke
  damage.sAnimation = "explopush2_" + num(direction);
  n::add_artillery(ret, kInvalidPoint, damage, f[2].s, kProjDelay);  // self.UpShot
  n::add_bounce(ret, p2, f[3].as_int());  // self.BounceAmount
  return true;
}

// ---- weapons_science.lua ----------------------------------------------------------

// Science_Repulse:GetTargetArea(point): the tile and its four neighbours.
bool repulse_area(PortContext&, const Fields&, Point point, std::vector<Point>& out) {
  out.push_back(point);
  for (int i = 0; i <= 3; ++i) out.push_back(point + vec(i));
  return true;
}

// Science_Repulse:GetSkillEffect(p1, p2): push all four neighbours out,
// shielding friendly ones (ShieldFriendly) and the shooter (ShieldSelf).
bool repulse_effect(PortContext& c, const Fields& f, Point p1, Point, SE& ret) {
  const bool shield_friendly = f[0].truthy();
  const bool shield_self = f[1].truthy();
  n::add_bounce(ret, p1, -2);
  for (int i = 0; i <= 3; ++i) {
    const Point curr = p1 + vec(i);
    SD space_damage = n::space_damage(curr, 0, i);
    if (shield_friendly && (n::is_building(c.board, curr) || n::pawn_team(c.board, curr) == kTeamPlayer)) {
      space_damage.iShield = 1;
    }
    space_damage.sAnimation = "airpush_" + num(i);
    n::add_damage(ret, space_damage);
    n::add_bounce(ret, curr, -1);
  }
  SD self_damage = n::space_damage(p1, 0);
  if (shield_self) self_damage.iShield = 1;
  self_damage.sAnimation = "ExploRepulse1";
  n::add_damage(ret, self_damage);
  return true;
}

// ---- advanced/ae_weapons_base.lua -------------------------------------------------

// Skill_Repair:GetSkillEffect(p1, p2): heal p2 and remove fire, ACID and
// Injured (Mass_Repair: every mech; Pulse: also push the neighbours).
bool repair_effect(PortContext& c, const Fields& f, Point p1, Point p2, SE& ret) {
  const bool pulse = f[1].truthy();
  SD damage = n::space_damage(p2, f[0].as_int());  // self.Amount
  damage.iFire = kEffectRemove;
  damage.iAcid = kEffectRemove;
  damage.iInjure = kEffectRemove;
  if (pulse) damage.sAnimation = "ExploRepulse1";
  n::add_damage(ret, damage);
  if (n::is_passive_skill(c.L, "Mass_Repair")) {
    // extract_table(Board:GetPawns(TEAM_MECH)), iterated in order.
    for (int id : n::pawn_ids(c.L, c.board, kTeamMech)) {
      const Pawn* m = c.board.find_pawn(id);
      const Point space = m ? m->pos : kInvalidPoint;  // Board:GetPawnSpace(id)
      if (space != p2) {
        damage.loc = space;
        n::add_damage(ret, damage);
      }
    }
  }
  if (pulse) {
    for (int i = 0; i <= 3; ++i) {
      SD space_damage = n::space_damage(p1 + vec(i), 0, i);
      space_damage.sAnimation = "airpush_" + num(i);
      n::add_damage(ret, space_damage);
    }
  }
  return true;
}

// ---- advanced/ae_weapons_enemy.lua ------------------------------------------------

// BouncerAtk1:GetSkillEffect(p1, p2): queued push of itself backwards, then
// a queued hit pushing the target away.
bool bouncer_effect(PortContext&, const Fields& f, Point p1, Point p2, SE& ret) {
  const int direction = n::direction(p2 - p1);
  const int dirback = n::direction(p1 - p2);
  SD damage = n::space_damage(p1, 0, dirback);
  damage.sAnimation = "airpush_" + num(dirback);
  n::add_queued_damage(ret, damage);
  damage = n::space_damage(p2, f[0].as_int(), direction);  // self.Damage
  damage.sAnimation = "SwipeClaw2";
  damage.sSound = f[1].s + "/attack";  // self.SoundBase
  n::add_queued_damage(ret, damage);
  return true;
}

// ---- shared helpers (weapons_base.lua, global.lua) ---------------------------------

// Lua's % on integers (floored).
int lmod(int a, int b) {
  const int r = a % b;
  return r != 0 && ((r < 0) != (b < 0)) ? r + b : r;
}

// GetProjectileEnd(p1, p2, profile) (weapons_base.lua:54): the first tile
// along GetDirection(p2 - p1) that blocks `profile`, or the last tile on the
// board. False where DIR_VECTORS[direction] is nil.
bool projectile_end(const Board& b, Point p1, Point p2, int profile, Point& out) {
  const int direction = n::direction(p2 - p1);
  if (!is_dir(direction)) return false;
  Point target = p1 + vec(direction);
  while (!n::is_blocked(b, target, profile)) target = target + vec(direction);
  if (!target.valid()) target = target - vec(direction);
  out = target;
  return true;
}

// The value of a field used as a number on the branch taken (the Lua raises
// for other types: the port gives up).
bool number(const FieldValue& v, double& out) {
  if (v.type != LUA_TNUMBER) return false;
  out = v.n;
  return true;
}

// ---- weapons_base.lua: TankDefault, ArtilleryDefault, lasers -----------------------

// TankDefault:GetTargetArea(p1): GetSimpleReachable, or with self.Phase the
// tiles up to 8 away until one blocks phasing.
// Fields: Phase, PathSize, CornersAllowed.
bool tank_area(PortContext& c, const Fields& f, Point p1, std::vector<Point>& out) {
  if (!f[0].truthy()) {
    if (f[1].type != LUA_TNUMBER || f[2].type != LUA_TBOOLEAN) return false;
    out = n::simple_reachable(c.board, p1, f[1].as_int(), f[2].b);
    return true;
  }
  for (int dir = 0; dir <= 3; ++dir) {
    for (int i = 1; i <= 8; ++i) {
      const Point curr = p1 + n::mul(vec(dir), i);
      if (!curr.valid()) break;
      out.push_back(curr);
      if (n::is_blocked(c.board, curr, kPathPhasing)) break;
    }
  }
  return true;
}

// TankDefault:GetSkillEffect(p1, p2): a projectile to GetProjectileEnd
// (phasing with self.Phase), with the optional push back of the shooter,
// flip, statuses, a back shot and building shields along the way.
// Fields: PushBack, SelfDamage, Phase, Damage, Flip, Push, Acid, Freeze, Fire,
// Shield, Explo, ProjectileArt, BackShot, PhaseShield.
bool tank_effect(PortContext& c, const Fields& f, Point p1, Point p2, SE& ret) {
  const Board& b = c.board;
  const int direction = n::direction(p2 - p1);
  if (f[0].equals(1)) {
    double self_damage = 0;
    if (!number(f[1], self_damage)) return false;
    n::add_damage(ret, n::space_damage(p1, n::to_int(self_damage), n::direction(p1 - p2)));
  }
  const int pathing = f[2].truthy() ? kPathPhasing : kPathProjectile;
  Point target;
  if (!projectile_end(b, p1, p2, pathing, target)) return false;
  SD damage = n::space_damage(target, f[3].as_int());
  if (f[4].equals(1)) damage = n::space_damage(target, f[3].as_int(), kDirFlip);
  if (f[5].equals(1)) damage.iPush = direction;
  damage.iAcid = f[6].as_int();
  damage.iFrozen = f[7].as_int();
  damage.iFire = f[8].as_int();
  damage.iShield = f[9].as_int();
  damage.sAnimation = f[10].s + num(direction);
  if (f[2].truthy() && n::is_building(b, target)) {
    damage.sAnimation = "";
    damage.iDamage = 0;
  }
  n::add_projectile(ret, kInvalidPoint, damage, f[11].s, kNoDelay);
  if (f[12].equals(1)) {
    const int backdir = n::direction(p1 - p2);
    if (!is_dir(backdir)) return false;
    Point target2;
    if (!projectile_end(b, p1, p1 + vec(backdir), kPathProjectile, target2)) return false;
    if (target2 != p1) {
      damage = n::space_damage(target2, f[3].as_int(), backdir);
      damage.sAnimation = f[10].s + num(backdir);
      n::add_projectile(ret, kInvalidPoint, damage, f[11].s, kProjDelay);
    }
  }
  if (f[13].truthy()) {
    Point temp = p1 + vec(direction);
    for (int guard = 0;; ++guard) {
      if (guard > 64) return false;  // never reaches target (the Lua would not end)
      if (n::is_building(b, temp)) {
        damage = n::space_damage(temp, 0);
        damage.iShield = 1;
        n::add_damage(ret, damage);
      }
      if (temp == target) break;
      temp = temp + vec(direction);
    }
  }
  return true;
}

// ArtilleryDefault:GetSkillEffect(p1, p2): artillery on p2 and the outer
// ring (pushed with self.Push == 1), buildings spared without
// self.BuildingDamage. Fields: DamageCenter, ExplosionCenter, BuildingDamage,
// UpShot, BounceAmount, DamageOuter, Push, OuterAnimation, BounceOuterAmount.
bool artillery_effect(PortContext& c, const Fields& f, Point p1, Point p2, SE& ret) {
  const Board& b = c.board;
  const bool building_damage = f[2].truthy();
  SD damage = n::space_damage(p2, f[0].as_int());
  damage.sAnimation = f[1].s;
  if (!building_damage && n::is_building(b, p2)) damage.iDamage = kDamageZero;
  n::add_bounce(ret, p1, 1);
  n::add_artillery(ret, kInvalidPoint, damage, f[3].s, kProjDelay);
  if (!f[4].equals(0)) {
    double bounce = 0;
    if (!number(f[4], bounce)) return false;
    n::add_bounce(ret, p2, n::to_int(bounce));
  }
  for (int dir = 0; dir <= 3; ++dir) {
    damage = n::space_damage(p2 + vec(dir), f[5].as_int());
    if (f[6].equals(1)) damage.iPush = dir;
    damage.sAnimation = f[7].s + num(dir);
    if (!building_damage && n::is_building(b, p2 + vec(dir))) {
      damage.iDamage = 0;
      damage.sAnimation = "airpush_" + num(dir);
    }
    n::add_damage(ret, damage);
    if (!f[8].equals(0)) {
      double bounce = 0;
      if (!number(f[8], bounce)) return false;
      n::add_bounce(ret, p2 + vec(dir), n::to_int(bounce));
    }
  }
  return true;
}

// Laser_Base:GetTargetArea(point): each direction up to the first mountain or
// building (included) or the edge.
bool laser_area(PortContext& c, const Fields&, Point point, std::vector<Point>& out) {
  for (int dir = 0; dir <= 3; ++dir) {
    Point curr = point + vec(dir);
    while (n::terrain(c.board, curr) != static_cast<int>(Terrain::Mountain) && !n::is_building(c.board, curr) &&
           curr.valid()) {
      out.push_back(curr);
      curr = curr + vec(dir);
    }
    if (curr.valid()) out.push_back(curr);
  }
  return true;
}

// The fields Laser_Base:AddLaser reads, in this order (laser_fields).
// MinDamage, Damage, FriendlyDamage, Smoke, Acid, Fire, Freeze, LaserArt.
constexpr size_t kLaserFields = 8;

// Laser_Base:AddLaser(ret, point, direction, queued) (weapons_base.lua:587):
// the beam from `point` on, losing 1 damage per tile down to MinDamage,
// ending (with the laser art) at a building, a mountain or the edge.
// `f` starts at the AddLaser fields.
bool add_laser(PortContext& c, const FieldValue* f, SE& ret, Point point, int direction, bool queued) {
  const Board& b = c.board;
  double min_damage = 1;
  if (f[0].truthy()) {
    if (!number(f[0], min_damage)) return false;  // compared with a number below
  }
  double damage = f[1].n;
  if (!is_dir(direction)) return false;
  const Point start = point - vec(direction);
  while (point.valid()) {
    double temp_damage = damage;
    if (!f[2].truthy() && n::is_pawn_team(c.L, b, point, kTeamPlayer)) temp_damage = kDamageZero;
    SD dam = n::space_damage(point, n::to_int(temp_damage));
    dam.iSmoke = f[3].as_int();
    dam.iAcid = f[4].as_int();
    dam.iFire = f[5].as_int();
    dam.iFrozen = f[6].as_int();
    // forced_end is nil here: `forced_end == point` is false.
    if (n::is_building(b, point) || n::terrain(b, point) == static_cast<int>(Terrain::Mountain) ||
        !(point + vec(direction)).valid()) {
      if (queued) {
        n::add_queued_projectile(ret, dam, f[7].s, kProjDelay);
      } else {
        n::add_projectile(ret, start, dam, f[7].s, kFullDelay);
      }
      break;
    }
    if (queued) {
      n::add_queued_damage(ret, dam);
    } else {
      n::add_damage(ret, dam);
    }
    damage = damage - 1;
    if (damage < min_damage) damage = min_damage;
    point = point + vec(direction);
  }
  return true;
}

// LaserDefault:GetSkillEffect(p1, p2): self:AddLaser from the next tile.
bool laser_effect(PortContext& c, const Fields& f, Point p1, Point p2, SE& ret) {
  const int direction = n::direction(p2 - p1);
  if (!is_dir(direction)) return false;
  return add_laser(c, f.data(), ret, p1 + vec(direction), direction, false);
}

// ---- weapons_snow.lua ---------------------------------------------------------------

// SnowlaserAtk1:GetTargetArea(point): the four neighbours on the board.
bool snowlaser_area(PortContext&, const Fields&, Point point, std::vector<Point>& out) {
  for (int dir = 0; dir <= 3; ++dir) {
    const Point curr = point + vec(dir);
    if (curr.valid()) out.push_back(curr);
  }
  return true;
}

// SnowlaserAtk1:GetSkillEffect(p1, p2): self:AddQueuedLaser (self.Queued) or
// self:AddLaser from the next tile. Fields: the AddLaser ones, then Queued.
bool snowlaser_effect(PortContext& c, const Fields& f, Point p1, Point p2, SE& ret) {
  const int direction = n::direction(p2 - p1);
  if (!is_dir(direction)) return false;
  return add_laser(c, f.data(), ret, p1 + vec(direction), direction, f[kLaserFields].truthy());
}

// ---- weapons_enemy.lua ----------------------------------------------------------------

// ScorpionAtk1:GetSkillEffect(p1, p2): a queued melee hit (pushing with
// Push == 1), webbing first with Web == 1. Fields: Push, Web, SoundBase,
// Damage, Acid.
bool scorpion_effect(PortContext&, const Fields& f, Point p1, Point p2, SE& ret) {
  const int direction = n::direction(p2 - p1);
  const int push = f[0].equals(1) ? direction : 4;
  if (f[1].equals(1)) {
    n::add_damage(ret, n::sound_effect(p2, f[2].s + "/attack_web"));
    n::add_grapple(ret, p1, p2, "hold");
  }
  SD damage = n::space_damage(p2, f[3].as_int(), push);
  damage.sAnimation = "SwipeClaw2";
  damage.iAcid = f[4].as_int();
  damage.sSound = f[2].s + "/attack";
  n::add_melee(ret, true, p1, damage, kFullDelay);
  return true;
}

// Burrower_Atk:GetSkillEffect(p1, p2): queued hits on p2 and its two side
// tiles. Fields: Damage, SoundBase.
bool burrower_effect(PortContext&, const Fields& f, Point p1, Point p2, SE& ret) {
  const int direction = n::direction(p2 - p1);
  SD damage = n::space_damage(p2, f[0].as_int());
  damage.sSound = f[1].s + "attack";
  n::add_queued_damage(ret, damage);
  const int side = lmod(direction + 1, 4);
  damage.loc = p2 + vec(side);
  n::add_queued_damage(ret, damage);
  damage.loc = p2 - vec(side);
  n::add_queued_damage(ret, damage);
  return true;
}

// HornetAtk1:GetSkillEffect(p1, p2): a queued melee hit (and the tile behind
// with TargetBehind). Fields: Damage, TargetBehind.
bool hornet_effect(PortContext&, const Fields& f, Point p1, Point p2, SE& ret) {
  const int direction = n::direction(p2 - p1);
  SD damage = n::space_damage(p2, f[0].as_int());
  damage.sAnimation = "explohornet_" + num(direction);
  n::add_melee(ret, true, p1, damage, 0.25f);
  if (f[1].truthy()) {
    if (!is_dir(direction)) return false;
    damage.loc = p2 + vec(direction);
    n::add_queued_damage(ret, damage);
  }
  return true;
}

// CrabAtk1:GetSkillEffect(p1, p2): queued artillery on p2 (and the tile
// behind for Type == 2). Fields: Damage, Projectile, Type.
bool crab_effect(PortContext&, const Fields& f, Point p1, Point p2, SE& ret) {
  const int dir = n::direction(p2 - p1);
  n::add_queued_artillery(ret, n::space_damage(p2, f[0].as_int()), f[1].s, kProjDelay);
  if (f[2].equals(2)) {
    if (!is_dir(dir)) return false;
    n::add_queued_damage(ret, n::space_damage(p2 + vec(dir), f[0].as_int()));
  }
  return true;
}

// FireflyAtk1:GetSkillEffect(p1, p2): a queued projectile to
// GetProjectileEnd. Fields: Push, Damage, Fire, Freeze, Acid, Projectile.
bool firefly_effect(PortContext& c, const Fields& f, Point p1, Point p2, SE& ret) {
  const int direction = n::direction(p2 - p1);
  Point target;
  if (!projectile_end(c.board, p1, p2, kPathProjectile, target)) return false;
  SD damage = f[0].equals(1) ? n::space_damage(target, f[1].as_int(), direction)
                             : n::space_damage(target, f[1].as_int());
  damage.iFire = f[2].as_int();
  damage.iFrozen = f[3].as_int();
  damage.iAcid = f[4].as_int();
  n::add_queued_projectile(ret, damage, f[5].s, kProjDelay);
  return true;
}

// ---- advanced/ae_weapons_enemy.lua ---------------------------------------------------

// BurnbugAtk1:GetSkillEffect(p1, p2): a queued grapple shot to the first
// blocking tile, pulling a pawn in (or itself to an obstacle); BossFire sets
// its neighbours on fire first. Fields: BossFire, Damage, PullSound.
bool burnbug_effect(PortContext& c, const Fields& f, Point p1, Point p2, SE& ret) {
  const Board& b = c.board;
  const int direction = n::direction(p2 - p1);
  if (!is_dir(direction)) return false;
  Point target = p1 + vec(direction);
  while (!n::is_blocked(b, target, kPathProjectile) && target.valid()) target = target + vec(direction);
  bool valid = true;
  if (!target.valid()) {
    valid = false;
    target = target - vec(direction);
  }
  if (f[0].truthy()) {
    for (int dir = 0; dir <= 3; ++dir) {
      SD fire_damage = n::space_damage(p1 + vec(dir), 0);
      fire_damage.iFire = 1;
      fire_damage.sAnimation = "explo_fire1";
      n::add_queued_damage(ret, fire_damage);
    }
  }
  SD damage = n::space_damage(target);
  damage.bHidePath = true;
  n::add_queued_projectile(ret, damage, "effects/shot_grapple", kProjDelay);
  damage = n::space_damage(target, f[1].as_int());
  damage.sSound = f[2].s;
  n::add_queued_damage(ret, damage);
  if (!valid || (n::is_pawn_space(b, target) && !n::is_guarding(*n::pawn_at(b, target)))) {
    n::add_move(ret, true, n::simple_path(b, target, p1 + vec(direction)), kFullDelay, 2);
  } else if (n::is_blocked(b, target, kPathGround)) {
    n::add_move(ret, true, n::simple_path(b, p1, target - vec(direction)), kFullDelay, 2);
  }
  return true;
}

// MothAtk1:GetSkillEffect(p1, p2): a queued push of itself backwards and a
// queued artillery shot pushing the target. Fields: Damage, Projectile.
bool moth_effect(PortContext&, const Fields& f, Point p1, Point p2, SE& ret) {
  const int dir = n::direction(p2 - p1);
  const int dirback = n::direction(p1 - p2);
  SD damage = n::space_damage(p1, 0, dirback);
  damage.sAnimation = "airpush_" + num(dirback);
  n::add_queued_damage(ret, damage);
  damage = n::space_damage(p2, f[0].as_int(), dir);
  n::add_queued_artillery(ret, damage, f[1].s, kProjDelay);
  return true;
}

// MosquitoAtk1:GetSkillEffect(p1, p2): smoke on the target now, a queued
// melee hit (webbing first with Webbing). Fields: SoundBase, Webbing, Damage.
bool mosquito_effect(PortContext&, const Fields& f, Point p1, Point p2, SE& ret) {
  const int direction = n::direction(p2 - p1);
  n::add_damage(ret, n::sound_effect(p2, f[0].s + "/attack_smoke"));
  if (f[1].truthy()) n::add_grapple(ret, p1, p2, "hold");
  SD damage = n::space_damage(p2, 0);
  damage.sAnimation = "";
  damage.iSmoke = 1;
  n::add_damage(ret, damage);
  damage = n::space_damage(p2, f[2].as_int());
  damage.sSound = f[0].s + "/attack";
  damage.sAnimation = "explomosquito_" + num(direction);
  n::add_melee(ret, true, p1, damage, kFullDelay);
  return true;
}

// ---- weapons_brute.lua -----------------------------------------------------------------

// Brute_Jetmech:GetTargetArea(point): MinMove..Range tiles away in each
// direction, where the shooter could land. Fields: MinMove, Range.
bool jetmech_area(PortContext& c, const Fields& f, Point point, std::vector<Point>& out) {
  if (!c.pawn) return false;
  const int prof = n::path_prof(c.board, *c.pawn);
  for (int i = 0; i <= 3; ++i) {
    for (double k = f[0].n; k <= f[1].n; k += 1) {
      const Point curr = n::mul(vec(i), n::to_int(k)) + point;
      if (!n::is_blocked(c.board, curr, prof)) out.push_back(curr);
    }
  }
  return true;
}

// Brute_Jetmech:GetSkillEffect(p1, p2): leap to p2, bombing the tiles
// flown over. Fields: Range, Damage, Smoke, Acid, AttackAnimation, BombSound,
// AnimDelay, DoubleAttack, Damage2.
bool jetmech_effect(PortContext&, const Fields& f, Point p1, Point p2, SE& ret) {
  const int dir = n::direction(p2 - p1);
  const std::vector<Point> move{p1, p2};
  const int distance = std::abs(p1.x - p2.x) + std::abs(p1.y - p2.y);
  n::add_bounce(ret, p1, 2);
  n::add_leap(ret, move, distance == 1 ? 0.5f : 0.25f);
  for (double k = 1; k <= f[0].n - 1; k += 1) {
    if (!is_dir(dir)) return false;
    const Point at = p1 + n::mul(vec(dir), n::to_int(k));
    if (at == p2) break;
    SD damage = n::space_damage(at, f[1].as_int());
    damage.iSmoke = f[2].as_int();
    damage.iAcid = f[3].as_int();
    damage.sAnimation = f[4].s;
    damage.sSound = f[5].s;
    if (k != 1) n::add_delay(ret, f[6].as_float());
    n::add_damage(ret, damage);
    n::add_bounce(ret, at, 3);
  }
  if (f[7].equals(1)) {
    if (!is_dir(dir)) return false;
    double damage2 = 0;
    if (!number(f[8], damage2)) return false;
    n::add_damage(ret, n::space_damage(p1 + n::mul(vec(dir), n::to_int(f[0].n + 1)), n::to_int(damage2)));
  }
  return true;
}

// Brute_Grapple:GetTargetArea(point): in each direction the free tiles up to
// the first blocking one, if that one is on the board and not adjacent.
bool grapple_area(PortContext& c, const Fields&, Point point, std::vector<Point>& out) {
  for (int dir = 0; dir <= 3; ++dir) {
    std::vector<Point> this_path;
    Point target = point + vec(dir);
    while (!n::is_blocked(c.board, target, kPathProjectile)) {
      this_path.push_back(target);
      target = target + vec(dir);
    }
    if (target.valid() && std::abs(target.x - point.x) + std::abs(target.y - point.y) > 1) {
      this_path.push_back(target);
      out.insert(out.end(), this_path.begin(), this_path.end());
    }
  }
  return true;
}

// Brute_Grapple:GetSkillEffect(p1, p2): a grapple shot pulling a pawn in, or
// pulling the shooter to an obstacle; shields for allies. Fields:
// ShieldAlly, Shield.
bool grapple_effect(PortContext& c, const Fields& f, Point p1, Point p2, SE& ret) {
  const Board& b = c.board;
  const int direction = n::direction(p2 - p1);
  if (!is_dir(direction)) return false;
  Point target = p1 + vec(direction);
  while (!n::is_blocked(b, target, kPathProjectile)) target = target + vec(direction);
  if (!target.valid()) return true;
  SD damage = n::space_damage(target);
  damage.bHidePath = true;
  n::add_projectile(ret, kInvalidPoint, damage, "effects/shot_grapple", kProjDelay);
  if (n::is_pawn_space(b, target) && !n::is_guarding(*n::pawn_at(b, target))) {
    n::add_move(ret, false, n::simple_path(b, target, p1 + vec(direction)), kFullDelay, 2);
    if (n::is_pawn_team(c.L, b, target, kTeamPlayer)) {
      SD shielddamage = n::space_damage(p1 + vec(direction), 0);
      shielddamage.iShield = f[0].as_int();
      n::add_damage(ret, shielddamage);
    }
    SD shield_self = n::space_damage(p1, 0);
    shield_self.iShield = f[1].as_int();
    n::add_damage(ret, shield_self);
  } else {
    if (!c.pawn) return false;
    if (n::is_blocked(b, target, n::path_prof(b, *c.pawn))) {
      n::add_move(ret, false, n::simple_path(b, p1, target - vec(direction)), kFullDelay, 2);
      SD shield_self = n::space_damage(target - vec(direction), 0);
      shield_self.iShield = f[1].as_int();
      n::add_damage(ret, shield_self);
      if (n::is_pawn_space(b, target) && n::is_pawn_team(c.L, b, target, kTeamPlayer)) {
        c.log("I am here");
        SD shielddamage = n::space_damage(target, 0);
        shielddamage.iShield = f[0].as_int();
        n::add_damage(ret, shielddamage);
      }
    }
  }
  return true;
}

// Brute_Beetle:GetSkillEffect(p1, p2): charge to the first blocking tile
// (flying, or with the shooter's pathing for Fly == 0) and ram it.
// Fields: Fly, BackSmoke, Damage, ImpactSound, SelfDamage.
bool beetle_effect(PortContext& c, const Fields& f, Point p1, Point p2, SE& ret) {
  const Board& b = c.board;
  const int direction = n::direction(p2 - p1);
  int pathing = kPathProjectile;
  if (f[0].equals(0)) {
    if (!c.pawn) return false;
    pathing = n::path_prof(b, *c.pawn);
  }
  bool do_damage = true;
  Point target;
  if (!projectile_end(b, p1, p2, pathing, target)) return false;
  const int distance = std::abs(p1.x - target.x) + std::abs(p1.y - target.y);
  if (!n::is_blocked(b, target, pathing)) {
    do_damage = false;
    target = target + vec(direction);
  }
  if (f[1].equals(1)) {
    SD smoke = n::space_damage(p1 - vec(direction), 0);
    smoke.iSmoke = 1;
    n::add_damage(ret, smoke);
  }
  SD damage = n::space_damage(target, f[2].as_int(), direction);
  damage.sAnimation = "ExploAir2";
  damage.sSound = f[3].s;
  if (distance == 1 && do_damage) {
    n::add_melee(ret, false, p1, damage, kNoDelay);
    n::add_damage(ret, n::space_damage(target - vec(direction), f[4].as_int()));
  } else {
    n::add_move(ret, false, n::simple_path(b, p1, target - vec(direction)), kNoDelay, 2);
    Point temp = p1;
    for (int guard = 0; temp != target; ++guard) {
      if (guard > 64) return false;
      n::add_bounce(ret, temp, -3);
      temp = temp + vec(direction);
      if (temp != target) n::add_delay(ret, 0.06f);
    }
    if (do_damage) {
      n::add_damage(ret, damage);
      n::add_damage(ret, n::space_damage(target - vec(direction), f[4].as_int()));
    }
  }
  return true;
}

// ---- weapons_ranged.lua ----------------------------------------------------------------

// Ranged_Rockthrow:GetSkillEffect(p1, p2): a rock on p2 (a RockThrown pawn on
// a free tile), pushing the two side tiles. Fields: Damage, BounceAmount.
bool rockthrow_effect(PortContext& c, const Fields& f, Point p1, Point p2, SE& ret) {
  const int dir = n::direction(p2 - p1);
  SD damage = n::space_damage(p2, f[0].as_int());
  if (p2.valid() && !n::is_blocked(c.board, p2, kPathProjectile)) {
    damage.sPawn = "RockThrown";
    damage.sAnimation = "";
    damage.iDamage = 0;
  } else {
    damage.sAnimation = "rock1d";
  }
  n::add_bounce(ret, p1, 1);
  n::add_artillery(ret, kInvalidPoint, damage, "effects/shotdown_rock.png", kProjDelay);
  n::add_bounce(ret, p2, f[1].as_int());
  n::add_board_shake(ret, 0.15f);
  const int a = lmod(dir + 1, 4), b = lmod(dir - 1, 4);
  SD damagepush = n::space_damage(p2 + vec(a), 0, a);
  damagepush.sAnimation = "airpush_" + num(a);
  n::add_damage(ret, damagepush);
  damagepush = n::space_damage(p2 + vec(b), 0, b);
  damagepush.sAnimation = "airpush_" + num(b);
  n::add_damage(ret, damagepush);
  return true;
}

// Ranged_Ignite:GetSkillEffect(p1, p2): burning artillery on p2 pushing its
// neighbours (and fire behind the shooter with Backhit == 1).
// Fields: Backhit, Damage, UpShot, BounceAmount.
bool ignite_effect(PortContext&, const Fields& f, Point p1, Point p2, SE& ret) {
  const int direction = n::direction(p1 - p2);
  n::add_bounce(ret, p1, 1);
  if (f[0].equals(1)) {
    if (!is_dir(direction)) return false;
    SD back = n::space_damage(p1 + vec(direction), 0);
    back.iFire = 1;
    n::add_damage(ret, back);
  }
  SD damage = n::space_damage(p2, f[1].as_int());
  damage.sAnimation = "ExploArt2";
  damage.iFire = 1;
  n::add_artillery(ret, kInvalidPoint, damage, f[2].s, kProjDelay);
  for (int dir = 0; dir <= 3; ++dir) {
    damage = n::space_damage(p2 + vec(dir), 0);
    damage.iPush = dir;
    damage.sAnimation = "airpush_" + num(dir);
    n::add_damage(ret, damage);
  }
  n::add_bounce(ret, p2, f[3].as_int());
  return true;
}

// Ranged_Ice:GetSkillEffect(p1, p2): freezing artillery; SelfFreeze == 1
// freezes the shooter first through the global `damage` (the Lua has no
// `local` there), which the port assigns the same way. Fields: SelfFreeze,
// Damage.
bool ice_effect(PortContext& c, const Fields& f, Point p1, Point p2, SE& ret) {
  n::add_bounce(ret, p1, 1);
  if (f[0].equals(1)) {
    SD self = n::space_damage(p1, 0);
    self.iFrozen = kEffectCreate;
    n::set_global_space_damage(c.L, "damage", self);
    n::add_damage(ret, self);
  }
  SD damage = n::space_damage(p2, f[1].as_int());
  damage.iFrozen = kEffectCreate;
  n::add_artillery(ret, kInvalidPoint, damage, "effects/shotup_ice.png", kProjDelay);
  n::add_bounce(ret, p2, 2);
  return true;
}

// ---- weapons_science.lua ----------------------------------------------------------------

// Science_Shield:GetSkillEffect(p1, p2): shield artillery on p2 and the tile
// beyond (all four neighbours with WideArea), the shooter with SelfShield.
// The one SpaceDamage is reused: later entries keep the moved loc and
// bHidePath. Fields: WideArea, SelfShield.
bool shield_effect(PortContext&, const Fields& f, Point p1, Point p2, SE& ret) {
  const int direction = n::direction(p2 - p1);
  SD damage = n::space_damage(p2, 0);
  damage.iShield = 1;
  n::add_artillery(ret, kInvalidPoint, damage, "effects/shot_pull_U.png", kNoDelay);
  for (int i = 0; i <= 3; ++i) {
    damage.loc = p2 + vec(i);
    damage.bHidePath = true;
    if (f[0].truthy() || i == direction) n::add_artillery(ret, kInvalidPoint, damage, "effects/shot_pull_U.png", kNoDelay);
  }
  if (f[1].equals(1)) {
    damage.loc = p1;
    n::add_damage(ret, damage);
  }
  return true;
}

// Science_Pullmech:GetSkillEffect(p1, p2): a pulling projectile to
// GetProjectileEnd (shield for a player pawn, ACID for an enemy).
// Fields: Damage, Shield, Acid.
bool pullmech_effect(PortContext& c, const Fields& f, Point p1, Point p2, SE& ret) {
  const Board& b = c.board;
  const int direction = n::direction(p2 - p1);
  Point target;
  if (!projectile_end(b, p1, p2, kPathProjectile, target)) return false;
  SD damage = n::space_damage(target, f[0].as_int(), n::direction(p1 - p2));
  if (n::is_pawn_team(c.L, b, target, kTeamPlayer)) {
    double shield = 0;
    if (!number(f[1], shield)) return false;
    damage.iShield = n::to_int(shield);
  } else if (n::is_pawn_team(c.L, b, target, kTeamEnemy)) {
    double acid = 0;
    if (!number(f[2], acid)) return false;
    damage.iAcid = n::to_int(acid);
  }
  n::add_projectile(ret, kInvalidPoint, damage, "effects/shot_pull", kNoDelay);
  Point temp = p1;
  for (int guard = 0; temp != target; ++guard) {
    if (guard > 64) return false;
    n::add_delay(ret, 0.05f);
    n::add_bounce(ret, temp, -1);
    temp = temp + vec(direction);
  }
  return true;
}

// ---- weapons_prime.lua -------------------------------------------------------------------

// Prime_Punchmech:GetSkillEffect(p1, p2): a melee punch at p2 pushing (or
// flipping) the target; Dash charges to the first obstacle first, Projectile
// throws the fist at range; Shield and PushBack add the extras.
// Fields: Flip, Damage, Shield, Dash, Projectile, PushBack.
bool punch_effect(PortContext& c, const Fields& f, Point p1, Point p2, SE& ret) {
  const Board& b = c.board;
  const int direction = n::direction(p2 - p1);
  bool do_damage = true;
  Point target;
  if (!projectile_end(b, p1, p2, kPathProjectile, target)) return false;
  const int push_damage = f[0].truthy() ? kDirFlip : direction;
  SD damage = n::space_damage(target, f[1].as_int(), push_damage);
  damage.sAnimation = "explopunch1_" + num(direction);
  if (f[0].truthy()) damage.sAnimation = "SwipeClaw2";
  if (f[2].truthy()) {
    SD shield = n::space_damage(p1, 0);
    shield.iShield = kEffectCreate;
    n::add_damage(ret, shield);
  }
  if (f[3].truthy()) {
    if (!n::is_blocked(b, target, kPathProjectile)) {
      do_damage = false;
      target = target + vec(direction);
    }
    n::add_move(ret, false, n::simple_path(b, p1, target - vec(direction)), kFullDelay, 2);
  } else if (f[4].truthy() && std::abs(target.x - p1.x) + std::abs(target.y - p1.y) != 1) {
    damage.loc = target;
    n::add_damage(ret, n::space_damage(p1, 0, lmod(direction + 2, 4)));
    n::add_projectile(ret, kInvalidPoint, damage, "effects/shot_fist", kProjDelay);
    do_damage = false;
  } else {
    target = p2;
  }
  if (do_damage) {
    damage.loc = target;
    n::add_melee(ret, false, p2 - vec(direction), damage, kFullDelay);
  }
  if (f[5].truthy()) n::add_damage(ret, n::space_damage(p1, 0, n::direction(p1 - p2)));
  return true;
}

// Prime_Lightning:GetSkillEffect(p1, p2): chain lightning: a depth-first walk
// (pop_back of a todo list) over connected pawns (and buildings with
// self.Buildings) from p2, hashing tiles as x + 10 y like the Lua does.
// Fields: Damage, Buildings, FriendlyDamage.
bool lightning_effect(PortContext& c, const Fields& f, Point p1, Point p2, SE& ret) {
  const Board& b = c.board;
  const bool buildings = f[1].truthy();
  auto hash = [](Point p) { return p.x + p.y * 10; };
  SD damage = n::space_damage(p2, f[0].as_int());
  std::unordered_map<int, bool> explored{{hash(p1), true}};
  std::vector<Point> todo{p2};
  std::unordered_map<int, Point> origin{{hash(p2), p1}};
  if (n::is_pawn_space(b, p2) || (buildings && n::is_building(b, p2))) n::add_animation(ret, p1, "Lightning_Hit", 2);
  while (!todo.empty()) {
    const Point current = todo.back();
    todo.pop_back();
    if (explored.count(hash(current))) continue;
    explored[hash(current)] = true;
    if (!(n::is_pawn_space(b, current) || (buildings && n::is_building(b, current)))) continue;
    const int direction = n::direction(current - origin.at(hash(current)));
    damage.sAnimation = "Lightning_Attack_" + num(direction);
    damage.loc = current;
    damage.iDamage = n::is_building(b, current) ? kDamageZero : f[0].as_int();
    if (!f[2].truthy() && n::is_pawn_team(c.L, b, current, kTeamPlayer)) damage.iDamage = kDamageZero;
    n::add_damage(ret, damage);
    if (!n::is_building(b, current)) n::add_animation(ret, current, "Lightning_Hit", 2);
    for (int i = 0; i <= 3; ++i) {
      const Point neighbor = current + vec(i);
      if (!explored.count(hash(neighbor))) {
        todo.push_back(neighbor);
        origin[hash(neighbor)] = current;
      }
    }
  }
  return true;
}

// Prime_Flamethrower:GetTargetArea(point): 1..PathSize tiles in each
// direction, the first off-board one included. Fields: PathSize.
bool flamethrower_area(PortContext&, const Fields& f, Point point, std::vector<Point>& out) {
  for (int i = 0; i <= 3; ++i) {
    for (double k = 1; k <= f[0].n; k += 1) {
      const Point curr = n::mul(vec(i), n::to_int(k)) + point;
      out.push_back(curr);
      if (!curr.valid()) break;
    }
  }
  return true;
}

// Prime_Flamethrower:GetSkillEffect(p1, p2): fire on every tile up to p2,
// pushing the last; burning pawns take FireDamage more. Fields: Push,
// FireDamage.
bool flamethrower_effect(PortContext& c, const Fields& f, Point p1, Point p2, SE& ret) {
  const Board& b = c.board;
  const int direction = n::direction(p2 - p1);
  const int distance = std::abs(p1.x - p2.x) + std::abs(p1.y - p2.y);
  for (int i = 1; i <= distance; ++i) {
    if (!is_dir(direction)) return false;
    const Point curr = p1 + n::mul(vec(direction), i);
    const bool exploding = n::is_pawn_space(b, curr) && n::is_fire(*n::pawn_at(b, curr));
    const int push = i == distance ? n::to_int(direction * f[0].n) : 4;
    SD damage = n::space_damage(p1 + n::mul(vec(direction), i), 0, push);
    if (exploding) {
      double fire_damage = 0;
      if (!number(f[1], fire_damage)) return false;
      damage.iDamage = n::to_int(damage.iDamage + fire_damage);
      damage.sAnimation = "ExploAir1";
    }
    damage.iFire = kEffectCreate;
    if (i == distance) damage.sAnimation = "flamethrower" + num(distance) + "_" + num(direction);
    n::add_damage(ret, damage);
  }
  return true;
}
// ---- batch 3: more enemy weapons and squad weapons ---------------------------------

// DiggerAtk1:GetTargetArea(point): { point }  (SelfTarget's shape, its own function)

// DiggerAtk1:GetSkillEffect(p1, p2): rock walls on the free neighbours now,
// then a queued hit on all four (pushing with self.Push).
// Fields: SoundId, Push, Damage.
bool digger_effect(PortContext& c, const Fields& f, Point p1, Point, SE& ret) {
  const Board& b = c.board;
  for (int dir = 0; dir <= 3; ++dir) {
    const Point curr = p1 + vec(dir);
    if (!n::is_blocked(b, curr, kPathProjectile) && n::terrain(b, curr) != static_cast<int>(Terrain::Water) &&
        !n::is_pod(b, curr)) {
      SD damage = n::space_damage(curr);
      damage.sPawn = "Wall";
      damage.sSound = "/enemy/" + f[0].s + "/attack_queued";
      n::add_damage(ret, damage);
    }
    const int push = f[1].truthy() ? dir : 4;
    SD damage = n::space_damage(p1 + vec(dir), f[2].as_int(), push);
    damage.sAnimation = "explorocker_" + num(dir);
    damage.sSound = "/enemy/" + f[0].s + "/attack";
    n::add_queued_damage(ret, damage);
  }
  return true;
}

// Vek_Hornet:GetSkillEffect(p1, p2): a stab through every tile up to p2,
// pushing the last. Fields: Push, Damage.
bool vek_hornet_effect(PortContext&, const Fields& f, Point p1, Point p2, SE& ret) {
  const int direction = n::direction(p2 - p1);
  const int distance = std::abs(p1.x - p2.x) + std::abs(p1.y - p2.y);
  for (int i = 1; i <= distance; ++i) {
    if (!is_dir(direction)) return false;
    const int push = i == distance ? n::to_int(direction * f[0].n) : 4;
    SD damage = n::space_damage(p1 + n::mul(vec(direction), i), f[1].as_int(), push);
    damage.sAnimation = "explohornet_" + num(direction);
    damage.fDelay = 0.15f;
    n::add_damage(ret, damage);
  }
  return true;
}

// Science_Swap:GetTargetArea(point): 1..Range tiles in each direction holding
// a pawn that is not guarding, or free for flyers. Fields: Range.
bool swap_area(PortContext& c, const Fields& f, Point point, std::vector<Point>& out) {
  const Board& b = c.board;
  for (int dir = 0; dir <= 3; ++dir) {
    for (double range = 1; range <= f[0].n; range += 1) {
      const Point curr = point + n::mul(vec(dir), n::to_int(range));
      if ((n::is_pawn_space(b, curr) && !n::is_guarding(*n::pawn_at(b, curr))) ||
          !n::is_blocked(b, curr, kPathFlyer)) {
        out.push_back(curr);
      }
    }
  }
  return true;
}

// Science_Swap:GetSkillEffect(p1, p2): teleport to p2, swapping with a pawn
// there (`Board:IsPawnSpace(p2) and 0 or FULL_DELAY`: 0 is true in Lua).
bool swap_effect(PortContext& c, const Fields&, Point p1, Point p2, SE& ret) {
  const float delay = n::is_pawn_space(c.board, p2) ? 0.0f : kFullDelay;
  n::add_teleport(ret, p1, p2, delay);
  if (delay != kFullDelay) n::add_teleport(ret, p2, p1, kFullDelay);
  return true;
}

// SpiderlingAtk1:GetSkillEffect(p1, p2): a queued melee hit. Fields: Damage.
bool spiderling_effect(PortContext&, const Fields& f, Point p1, Point p2, SE& ret) {
  n::add_melee(ret, true, p1, n::space_damage(p2, f[0].as_int()), kFullDelay);
  return true;
}

// WebeggHatch1:GetSkillEffect(p1, p2): a queued spawn of self.SpiderType on
// the egg. Fields: SpiderType.
bool webegg_effect(PortContext&, const Fields& f, Point p1, Point, SE& ret) {
  SD damage = n::space_damage(p1);
  damage.sPawn = f[0].s;
  n::add_queued_damage(ret, damage);
  return true;
}

// TotemAtk1:GetSkillEffect(p1, p2): a queued projectile to GetProjectileEnd,
// then the totem destroys itself. Fields: Damage, HitExplosion, Projectile.
bool totem_effect(PortContext& c, const Fields& f, Point p1, Point p2, SE& ret) {
  const int direction = n::direction(p2 - p1);
  Point target;
  if (!projectile_end(c.board, p1, p2, kPathProjectile, target)) return false;
  SD damage = n::space_damage(target, f[0].as_int(), direction);
  damage.sAnimation = f[1].s;
  n::add_queued_projectile(ret, damage, f[2].s, kProjDelay);
  n::add_queued_damage(ret, n::space_damage(p1, kDamageDeath));
  return true;
}

// SnowartAtk1:GetSkillEffect(p1, p2): artillery on p2 and its two side tiles
// (all queued with self.Queued). Fields: Queued, Damage, Projectile.
bool snowart_effect(PortContext&, const Fields& f, Point p1, Point p2, SE& ret) {
  const int dir = n::direction(p2 - p1);
  const int a = lmod(dir + 1, 4), b = lmod(dir - 1, 4);
  if (f[0].truthy()) {
    n::add_queued_artillery(ret, n::space_damage(p2, f[1].as_int()), f[2].s, kProjDelay);
    n::add_queued_damage(ret, n::space_damage(p2 + vec(a), f[1].as_int()));
    n::add_queued_damage(ret, n::space_damage(p2 + vec(b), f[1].as_int()));
  } else {
    n::add_artillery(ret, kInvalidPoint, n::space_damage(p2, f[1].as_int()), f[2].s, kProjDelay);
    n::add_damage(ret, n::space_damage(p2 + vec(a), f[1].as_int()));
    n::add_damage(ret, n::space_damage(p2 + vec(b), f[1].as_int()));
  }
  return true;
}

// BeetleAtk1:GetSkillEffect(p1, p2): a queued charge to the first blocking
// tile (or into water / a chasm), ramming it; the boss leaves a fire trail.
// The Lua assigns the global `damage` (no `local`); the port assigns its
// final value the same way. Fields: Fly, Damage, Fire.
bool beetle_atk_effect(PortContext& c, const Fields& f, Point p1, Point p2, SE& ret) {
  const Board& b = c.board;
  const int direction = n::direction(p2 - p1);
  const int pathing = f[0].truthy() ? kPathProjectile : kPathGround;
  Point target;
  if (!projectile_end(b, p1, p2, pathing, target)) return false;
  bool do_damage = true;
  if (!n::is_blocked(b, target, pathing)) {
    do_damage = false;
    target = target + vec(direction);
  }
  if (!n::is_pawn_space(b, target) && (n::terrain(b, target) == static_cast<int>(Terrain::Water) ||
                                       n::terrain(b, target) == static_cast<int>(Terrain::Hole))) {
    do_damage = false;
    target = target + vec(direction);
  }
  const float delay = f[2].truthy() ? kNoDelay : kFullDelay;
  n::add_move(ret, true, n::simple_path(b, p1, target - vec(direction)), delay, 2);
  bool assigned = false;
  SD global;
  if (f[2].truthy()) {
    Point i = p1;
    for (int guard = 0; i != target - vec(direction); ++guard) {
      if (guard > 64) return false;
      global = n::space_damage(i, 0);
      global.iFire = 1;
      global.fDelay = 0.1f;
      assigned = true;
      n::add_queued_damage(ret, global);
      i = i + vec(direction);
    }
  }
  if (do_damage) {
    global = n::space_damage(target, f[1].as_int(), direction);
    global.sAnimation = "ExploAir2";
    global.sSound = "/enemy/beetle_1/attack_impact";
    assigned = true;
    n::add_queued_damage(ret, global);
  }
  if (assigned) n::set_global_space_damage(c.L, "damage", global);
  return true;
}
// ---- batch 4: mission units -----------------------------------------------------------

// Trapped_Explode:GetTargetArea(p1) (mission_trapped.lua): the neighbours that
// are not buildings, then the tile itself.
bool trapped_area(PortContext& c, const Fields&, Point p1, std::vector<Point>& out) {
  for (int dir = 0; dir <= 3; ++dir) {
    if (!n::is_building(c.board, p1 + vec(dir))) out.push_back(p1 + vec(dir));
  }
  out.push_back(p1);
  return true;
}

// Trapped_Explode:GetSkillEffect(p1, p2): DAMAGE_DEATH on the tile and its
// non-building neighbours (one SpaceDamage moved around).
bool trapped_effect(PortContext& c, const Fields&, Point p1, Point, SE& ret) {
  SD damage = n::space_damage(p1, kDamageDeath);
  n::add_damage(ret, damage);
  for (int dir = 0; dir <= 3; ++dir) {
    if (!n::is_building(c.board, p1 + vec(dir))) {
      damage.loc = p1 + vec(dir);
      n::add_damage(ret, damage);
    }
  }
  return true;
}

// Train_Move:GetTargetArea(point) (mission_train.lua): { point + VEC_UP }
bool train_area(PortContext&, const Fields&, Point point, std::vector<Point>& out) {
  out.assign(1, point + Point{0, -1});
  return true;
}

// Train_Move:GetSkillEffect(p1, p2): a queued charge up to two tiles up,
// crushing the first blocking tile (and hitting the one behind it).
bool train_effect(PortContext& c, const Fields&, Point p1, Point p2, SE& ret) {
  const Board& b = c.board;
  std::vector<Point> q_move{p1};
  Point current = p2;
  for (int k = 1; k <= 2; ++k) {
    if (n::is_blocked(b, current, kPathGround) && current.valid()) {
      n::add_move(ret, true, q_move, kFullDelay, 2);
      SD damage = n::space_damage(current, kDamageDeath);
      n::add_queued_damage(ret, damage);
      damage.sImageMark = "combat/arrow_hit.png";
      damage.loc = current + Point{0, 1};
      damage.iDamage = 1;
      n::add_queued_damage(ret, damage);
      return true;
    }
    if (current.valid()) q_move.push_back(current);
    current = current + Point{0, -1};
  }
  n::add_move(ret, true, q_move, kFullDelay, 2);
  return true;
}

// VIP_Truck_Move:GetTargetArea(point) (mission_civilians.lua):
//   Board:GetReachable(point, 3, Pawn:GetPathProf())
bool truck_area(PortContext& c, const Fields&, Point point, std::vector<Point>& out) {
  if (!c.pawn) return false;
  out = n::reachable(c.board, point, 3, n::path_prof(c.board, *c.pawn));
  return true;
}

// VIP_Truck_Move:GetSkillEffect(p1, p2): a sound, then a walk along GetPath.
bool truck_effect(PortContext& c, const Fields&, Point p1, Point p2, SE& ret) {
  if (!c.pawn) return false;
  n::add_sound(ret, "/support/vip_truck/move");
  n::add_move(ret, false, n::path(c.board, p1, p2, n::path_prof(c.board, *c.pawn)), kFullDelay, 0);
  return true;
}

// Archive_ArtShot:GetSkillEffect(p1, p2) (mission_artillery.lua): artillery
// on p2 and the tile behind it. Fields: Damage, UpShot.
bool archive_art_effect(PortContext&, const Fields& f, Point p1, Point p2, SE& ret) {
  const int dir = n::direction(p2 - p1);
  n::add_artillery(ret, kInvalidPoint, n::space_damage(p2, f[0].as_int()), f[1].s, kNoDelay);
  if (!is_dir(dir)) return false;
  SD damage2 = n::space_damage(p2 + vec(dir), f[0].as_int());
  damage2.bHidePath = true;
  n::add_artillery(ret, kInvalidPoint, damage2, f[1].s, kProjDelay);
  return true;
}

// BlobAtk1:GetSkillEffect(p1, p2): a queued explosion on itself and
// BombSize tiles in each direction. Fields: InnerDamage, BombSize,
// OuterDamage, OuterExplosion.
bool blob_effect(PortContext&, const Fields& f, Point p1, Point, SE& ret) {
  SD damage = n::space_damage(p1, f[0].as_int());
  damage.sSound = "/impact/generic/explosion_large";
  damage.bHideIcon = true;
  n::add_queued_damage(ret, damage);
  for (int dir = 0; dir <= 3; ++dir) {
    for (double size = 1; size <= f[1].n; size += 1) {
      damage = n::space_damage(p1 + n::mul(vec(dir), n::to_int(size)), f[2].as_int());
      damage.sAnimation = f[3].s + num(dir);
      n::add_queued_damage(ret, damage);
    }
  }
  return true;
}

// ---- batch 5: deployable units (weapons_base.lua Deployable, weapons_deploy.lua) ----

// Deployable:GetSkillEffect(p1, p2): one artillery shot that creates
// self.Deployed on p2 (the support tanks). Fields: Deployed, Projectile.
bool deployable_effect(PortContext&, const Fields& f, Point, Point p2, SE& ret) {
  SD damage = n::space_damage(p2, 0);
  damage.sPawn = f[0].s;
  n::add_artillery(ret, kInvalidPoint, damage, f[1].s, kProjDelay);
  return true;
}

// Deploy_AcidTankShot:GetSkillEffect(p1, p2): ACID on GetProjectileEnd
// (pushed when self.Push == 1). Fields: Damage, Push, Acid, ProjectileArt.
bool acid_tank_effect(PortContext& c, const Fields& f, Point p1, Point p2, SE& ret) {
  const int direction = n::direction(p2 - p1);
  Point target;
  if (!projectile_end(c.board, p1, p2, kPathProjectile, target)) return false;
  SD damage = n::space_damage(target, f[0].as_int());
  if (f[1].equals(1)) damage = n::space_damage(target, f[0].as_int(), direction);
  damage.iAcid = f[2].as_int();
  damage.sAnimation = "ExploAcid1";
  n::add_projectile(ret, kInvalidPoint, damage, f[3].s, kNoDelay);
  return true;
}

// Deploy_ShieldTankShot:GetSkillEffect(p1, p2): a melee shield on p2. The
// Lua assigns the global `damage` (no `local`); the port does the same.
bool shield_tank_effect(PortContext& c, const Fields&, Point p1, Point p2, SE& ret) {
  SD damage = n::space_damage(p2, 0);
  damage.iShield = 1;
  n::add_melee(ret, false, p1, damage, kFullDelay);
  n::set_global_space_damage(c.L, "damage", damage);
  return true;
}

// Deploy_ShieldTankShot2:GetSkillEffect(p1, p2): a projectile that shields
// GetProjectileEnd. Fields: Damage, Shield, ProjectileArt.
bool shield_tank2_effect(PortContext& c, const Fields& f, Point p1, Point p2, SE& ret) {
  Point target;
  if (!projectile_end(c.board, p1, p2, kPathProjectile, target)) return false;
  SD damage = n::space_damage(target, f[0].as_int());
  damage.iShield = f[1].as_int();
  n::add_projectile(ret, kInvalidPoint, damage, f[2].s, kNoDelay);
  return true;
}

using K = FieldKind;

constexpr DepSpec kProjectileEnd{DepSpec::Global, "GetProjectileEnd", "weapons_base.lua", 54, 0x36caaebbd1f32521};
constexpr DepSpec kAddLaser{DepSpec::SelfMethod, "AddLaser", "weapons_base.lua", 587, 0xa453d270fb69a64b};
constexpr DepSpec kAddQueuedLaser{DepSpec::SelfMethod, "AddQueuedLaser", "weapons_base.lua", 583, 0xabfc153cab34f722};
constexpr DepSpec kPopBack{DepSpec::Global, "pop_back", "global.lua", 577, 0x44ddfc024deeb959};

// The fields Laser_Base:AddLaser reads (kLaserFields of them, in order).
std::vector<FieldSpec> laser_fields(std::vector<FieldSpec> more = {}) {
  std::vector<FieldSpec> f = {{"MinDamage", K::Any}, {"Damage", K::Num}, {"FriendlyDamage", K::Any},
                              {"Smoke", K::Num},     {"Acid", K::Num},   {"Fire", K::Num},
                              {"Freeze", K::Num},    {"LaserArt", K::Str}};
  f.insert(f.end(), more.begin(), more.end());
  return f;
}

}  // namespace

const std::vector<PortSpec>& port_specs() {
  static const std::vector<PortSpec> specs = {
      // weapons_base.lua / global.lua
      {"GetTargetArea", "global.lua", 331, 0x43340d5eb3b566a4, {{"PathSize", K::Num}, {"CornersAllowed", K::Bool}}, {}, false,
       skill_area, nullptr},
      {"GetTargetArea", "weapons_base.lua", 224, 0x2757e13d99c1dd19, {}, {}, false, self_target_area, nullptr},
      {"GetTargetArea", "weapons_base.lua", 156, 0xcd0f2ccf568d81a0, {}, {}, false, move_area, nullptr},
      {"GetSkillEffect", "weapons_base.lua", 160, 0xcc4e65c59f21b30e, {}, {}, false, nullptr, move_effect},
      {"GetTargetArea", "weapons_base.lua", 310, 0xd893da6e4c1e2dfc, {{"ArtillerySize", K::Num}, {"OnlyEmpty", K::Any}}, {}, false,
       line_artillery_area, nullptr},
      // weapons_ranged.lua
      {"GetSkillEffect", "weapons_ranged.lua", 264, 0x19090ef838a1971b,
       {{"Damage", K::Num}, {"Smoke", K::Num}, {"UpShot", K::Str}, {"BounceAmount", K::Num}}, {}, false, nullptr,
       rocket_effect},
      // weapons_science.lua
      {"GetTargetArea", "weapons_science.lua", 146, 0xe51ed2c7138692ac, {}, {}, false, repulse_area, nullptr},
      {"GetSkillEffect", "weapons_science.lua", 156, 0xede72a115cea39bb, {{"ShieldFriendly", K::Any}, {"ShieldSelf", K::Any}}, {},
       false, nullptr, repulse_effect},
      // advanced/ae_weapons_base.lua
      {"GetSkillEffect", "advanced/ae_weapons_base.lua", 4, 0x3a032072291dd499, {{"Amount", K::Num}, {"Pulse", K::Any}},
       {{DepSpec::Global, "extract_table", "events.lua", 270, 0xb60070f84f45567b}}, false, nullptr, repair_effect},
      // advanced/ae_weapons_enemy.lua
      {"GetSkillEffect", "advanced/ae_weapons_enemy.lua", 343, 0x490ebddb0c0a1e6c, {{"Damage", K::Num}, {"SoundBase", K::Str}}, {},
       false, nullptr, bouncer_effect},
      {"GetSkillEffect", "advanced/ae_weapons_enemy.lua", 261, 0x65f878a92890e119,
       {{"BossFire", K::Any}, {"Damage", K::Num}, {"PullSound", K::Str}}, {}, false, nullptr, burnbug_effect},
      {"GetSkillEffect", "advanced/ae_weapons_enemy.lua", 385, 0x62914997acb8f612, {{"Damage", K::Num}, {"Projectile", K::Str}},
       {}, false, nullptr, moth_effect},
      {"GetSkillEffect", "advanced/ae_weapons_enemy.lua", 72, 0xf9041c08f773d836,
       {{"SoundBase", K::Str}, {"Webbing", K::Any}, {"Damage", K::Num}}, {}, false, nullptr, mosquito_effect},
      // weapons_base.lua: tanks, artillery, lasers
      {"GetTargetArea", "weapons_base.lua", 450, 0xdac43829aa0ea134,
       {{"Phase", K::Any}, {"PathSize", K::Any}, {"CornersAllowed", K::Any}}, {}, false, tank_area, nullptr},
      {"GetSkillEffect", "weapons_base.lua", 477, 0x405a21923414e0f6,
       {{"PushBack", K::Any}, {"SelfDamage", K::Any}, {"Phase", K::Any}, {"Damage", K::Num}, {"Flip", K::Any},
        {"Push", K::Any}, {"Acid", K::Num}, {"Freeze", K::Num}, {"Fire", K::Num}, {"Shield", K::Num},
        {"Explo", K::Str}, {"ProjectileArt", K::Str}, {"BackShot", K::Any}, {"PhaseShield", K::Any}},
       {kProjectileEnd}, false, nullptr, tank_effect},
      {"GetSkillEffect", "weapons_base.lua", 388, 0x523bce7fbeffbb2b,
       {{"DamageCenter", K::Num}, {"ExplosionCenter", K::Str}, {"BuildingDamage", K::Any}, {"UpShot", K::Str},
        {"BounceAmount", K::Any}, {"DamageOuter", K::Num}, {"Push", K::Any}, {"OuterAnimation", K::Str},
        {"BounceOuterAmount", K::Any}},
       {}, false, nullptr, artillery_effect},
      {"GetTargetArea", "weapons_base.lua", 566, 0x8e13d0802fd0d952, {}, {}, false, laser_area, nullptr},
      {"GetSkillEffect", "weapons_base.lua", 645, 0xc30cc06db74e63f0, laser_fields(), {kAddLaser}, false, nullptr,
       laser_effect},
      // weapons_snow.lua
      {"GetTargetArea", "weapons_snow.lua", 55, 0x39b0d6ae5240fdaa, {}, {}, false, snowlaser_area, nullptr},
      {"GetSkillEffect", "weapons_snow.lua", 67, 0xe264ba5e598f1ebf, laser_fields({{"Queued", K::Any}}),
       {kAddLaser, kAddQueuedLaser}, false, nullptr, snowlaser_effect},
      // weapons_enemy.lua
      {"GetSkillEffect", "weapons_enemy.lua", 54, 0x0a66c341acb0081a,
       {{"Push", K::Any}, {"Web", K::Any}, {"SoundBase", K::Str}, {"Damage", K::Num}, {"Acid", K::Num}}, {}, false,
       nullptr, scorpion_effect},
      {"GetSkillEffect", "weapons_enemy.lua", 115, 0xcafee02b08c48a11, {{"Damage", K::Num}, {"SoundBase", K::Str}}, {}, false,
       nullptr, burrower_effect},
      {"GetSkillEffect", "weapons_enemy.lua", 158, 0xd2b1dfb1b6875c45, {{"Damage", K::Num}, {"TargetBehind", K::Any}}, {},
       false, nullptr, hornet_effect},
      {"GetSkillEffect", "weapons_enemy.lua", 218, 0xf4755b228257dacc,
       {{"Damage", K::Num}, {"Projectile", K::Str}, {"Type", K::Any}}, {}, false, nullptr, crab_effect},
      {"GetSkillEffect", "weapons_enemy.lua", 423, 0x3c038ab66d9a2287,
       {{"Push", K::Any}, {"Damage", K::Num}, {"Fire", K::Num}, {"Freeze", K::Num}, {"Acid", K::Num},
        {"Projectile", K::Str}},
       {kProjectileEnd}, false, nullptr, firefly_effect},
      // weapons_brute.lua
      {"GetTargetArea", "weapons_brute.lua", 105, 0x15947ef665f20945, {{"MinMove", K::Num}, {"Range", K::Num}}, {}, false,
       jetmech_area, nullptr},
      {"GetSkillEffect", "weapons_brute.lua", 118, 0xd6e26cfb04430610,
       {{"Range", K::Num}, {"Damage", K::Num}, {"Smoke", K::Num}, {"Acid", K::Num}, {"AttackAnimation", K::Str},
        {"BombSound", K::Str}, {"AnimDelay", K::Num}, {"DoubleAttack", K::Any}, {"Damage2", K::Any}},
       {}, false, nullptr, jetmech_effect},
      {"GetTargetArea", "weapons_brute.lua", 316, 0xe32a9a2762dca41a, {}, {}, false, grapple_area, nullptr},
      {"GetSkillEffect", "weapons_brute.lua", 339, 0x13326b9679024aca, {{"ShieldAlly", K::Num}, {"Shield", K::Num}}, {},
       false, nullptr, grapple_effect},
      {"GetSkillEffect", "weapons_brute.lua", 570, 0x83eb1c41093d1a96,
       {{"Fly", K::Any}, {"BackSmoke", K::Any}, {"Damage", K::Num}, {"ImpactSound", K::Str}, {"SelfDamage", K::Num}},
       {kProjectileEnd}, false, nullptr, beetle_effect},
      // weapons_ranged.lua
      {"GetSkillEffect", "weapons_ranged.lua", 137, 0x13eb7c05e499dcfc, {{"Damage", K::Num}, {"BounceAmount", K::Num}}, {},
       false, nullptr, rockthrow_effect},
      {"GetSkillEffect", "weapons_ranged.lua", 331, 0x31e9066fa3199a22,
       {{"Backhit", K::Any}, {"Damage", K::Num}, {"UpShot", K::Str}, {"BounceAmount", K::Num}}, {}, false, nullptr,
       ignite_effect},
      {"GetSkillEffect", "weapons_ranged.lua", 581, 0x12b164e3512ea2f5, {{"SelfFreeze", K::Any}, {"Damage", K::Num}}, {},
       false, nullptr, ice_effect},
      // weapons_science.lua
      {"GetSkillEffect", "weapons_science.lua", 515, 0x67af70453bbb05a1, {{"WideArea", K::Any}, {"SelfShield", K::Any}}, {},
       false, nullptr, shield_effect},
      {"GetSkillEffect", "weapons_science.lua", 67, 0x0888be9ba48d192c,
       {{"Damage", K::Num}, {"Shield", K::Any}, {"Acid", K::Any}}, {kProjectileEnd}, false, nullptr, pullmech_effect},
      // weapons_prime.lua
      {"GetSkillEffect", "weapons_prime.lua", 57, 0x535785b334717668,
       {{"Flip", K::Any}, {"Damage", K::Num}, {"Shield", K::Any}, {"Dash", K::Any}, {"Projectile", K::Any},
        {"PushBack", K::Any}},
       {kProjectileEnd}, false, nullptr, punch_effect},
      {"GetSkillEffect", "weapons_prime.lua", 183, 0x375c3a842183df29,
       {{"Damage", K::Num}, {"Buildings", K::Any}, {"FriendlyDamage", K::Any}}, {kPopBack}, false, nullptr,
       lightning_effect},
      {"GetTargetArea", "weapons_prime.lua", 672, 0x4c657a8e976cf8a6, {{"PathSize", K::Num}}, {}, false, flamethrower_area,
       nullptr},
      {"GetSkillEffect", "weapons_prime.lua", 687, 0x204ace952a22f974, {{"Push", K::Num}, {"FireDamage", K::Any}}, {}, false,
       nullptr, flamethrower_effect},
      {"GetTargetArea", "weapons_prime.lua", 813, 0xba8219e6c9b2e31c, {{"PathSize", K::Num}}, {}, false, flamethrower_area,
       nullptr},
      // weapons_technovek.lua
      {"GetSkillEffect", "weapons_technovek.lua", 79, 0x318870eb3e795cba, {{"Push", K::Num}, {"Damage", K::Num}}, {}, false,
       nullptr, vek_hornet_effect},
      // weapons_science.lua (Swap)
      {"GetTargetArea", "weapons_science.lua", 233, 0x5528f06bc1d557f3, {{"Range", K::Num}}, {}, false, swap_area, nullptr},
      {"GetSkillEffect", "weapons_science.lua", 248, 0xceef7832ee018d8d, {}, {}, false, nullptr, swap_effect},
      // weapons_enemy.lua (more)
      {"GetTargetArea", "weapons_enemy.lua", 580, 0x0841d12f3765f349, {}, {}, false, self_target_area, nullptr},
      {"GetSkillEffect", "weapons_enemy.lua", 586, 0x0d1f1ed9fe368532,
       {{"SoundId", K::Str}, {"Push", K::Any}, {"Damage", K::Num}}, {}, false, nullptr, digger_effect},
      {"GetSkillEffect", "weapons_enemy.lua", 13, 0x8e8875ee770d8083, {{"Damage", K::Num}}, {}, false, nullptr,
       spiderling_effect},
      {"GetSkillEffect", "weapons_enemy.lua", 844, 0x82ce95a733795cab, {{"SpiderType", K::Str}}, {}, false, nullptr,
       webegg_effect},
      {"GetSkillEffect", "weapons_enemy.lua", 490, 0x35124a426c3d398a,
       {{"Fly", K::Any}, {"Damage", K::Num}, {"Fire", K::Any}}, {kProjectileEnd}, false, nullptr, beetle_atk_effect},
      {"GetSkillEffect", "advanced/ae_weapons_enemy.lua", 201, 0x942156cd8706411c,
       {{"Damage", K::Num}, {"HitExplosion", K::Str}, {"Projectile", K::Str}}, {kProjectileEnd}, false, nullptr,
       totem_effect},
      // weapons_snow.lua
      {"GetSkillEffect", "weapons_snow.lua", 120, 0x28ad6af731fdcd65,
       {{"Queued", K::Any}, {"Damage", K::Num}, {"Projectile", K::Str}}, {}, false, nullptr, snowart_effect},
      {"GetSkillEffect", "weapons_enemy.lua", 713, 0x7a5fcf0578db075f,
       {{"InnerDamage", K::Num}, {"BombSize", K::Num}, {"OuterDamage", K::Num}, {"OuterExplosion", K::Str}}, {}, false,
       nullptr, blob_effect},
      // mission units
      {"GetTargetArea", "advanced/missions/sand/mission_trapped.lua", 54, 0xe6f5879b03604547, {}, {}, false, trapped_area,
       nullptr},
      {"GetSkillEffect", "advanced/missions/sand/mission_trapped.lua", 68, 0xcd0c7f166983b8bb, {}, {}, false, nullptr,
       trapped_effect},
      {"GetTargetArea", "missions/mission_train.lua", 145, 0x912cfe2a7f722c19, {}, {}, false, train_area, nullptr},
      {"GetSkillEffect", "missions/mission_train.lua", 151, 0x181a1a5c02bd7b43, {}, {}, false, nullptr, train_effect},
      {"GetTargetArea", "advanced/missions/acid/mission_civilians.lua", 73, 0x28be527b42cef626, {}, {}, false, truck_area,
       nullptr},
      {"GetSkillEffect", "advanced/missions/acid/mission_civilians.lua", 77, 0xa391c936635fab65, {}, {}, false, nullptr,
       truck_effect},
      {"GetSkillEffect", "missions/grass/mission_artillery.lua", 99, 0x21e7365d30096c24,
       {{"Damage", K::Num}, {"UpShot", K::Str}}, {}, false, nullptr, archive_art_effect},
      // deployable units
      {"GetSkillEffect", "weapons_base.lua", 355, 0x750d6de920c63d87, {{"Deployed", K::Str}, {"Projectile", K::Str}}, {},
       false, nullptr, deployable_effect},
      {"GetSkillEffect", "weapons_deploy.lua", 265, 0x5ed5c3ec059c13ee,
       {{"Damage", K::Num}, {"Push", K::Any}, {"Acid", K::Num}, {"ProjectileArt", K::Str}}, {kProjectileEnd}, false,
       nullptr, acid_tank_effect},
      {"GetSkillEffect", "weapons_deploy.lua", 129, 0x02aff87cdaaed9a4, {}, {}, false, nullptr, shield_tank_effect},
      {"GetSkillEffect", "weapons_deploy.lua", 159, 0x9ee4611ca3cc1335,
       {{"Damage", K::Num}, {"Shield", K::Num}, {"ProjectileArt", K::Str}}, {kProjectileEnd}, false, nullptr,
       shield_tank2_effect},
  };
  return specs;
}

}  // namespace itb::lua
