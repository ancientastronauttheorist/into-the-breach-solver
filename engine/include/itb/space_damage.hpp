// SpaceDamage and SkillEffect: what a weapon, environment or death effect
// asks the board to do. Mirrors the fields the game exposes to Lua
// (SpaceDamage is 0xC8 bytes natively; SkillEffect holds an instant list and a
// queued list).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "itb/core.hpp"
#include "itb/symbols.hpp"

namespace itb {

// How a hit is applied. Weapon hits respect armor/ACID and affect terrain;
// push-mode hits (bumps, blocked spawns) ignore armor/ACID and only damage
// pawns, buildings and mountains. Explosion mode is what a corpse explosion
// uses (SpaceDamage mode override 2): the tile treats it like a weapon hit,
// pawns take it without armor or ACID arithmetic.
enum class DamageMode : int8_t { Weapon = 0, Push = 1, Explosion = 2 };

// SpaceDamage status fields use 0 = untouched, 1 = apply, 2 = remove.
enum class StatusChange : int8_t { None = 0, Apply = 1, Remove = 2 };

// Weapon animation kinds (SpaceDamage +0x58). Air strikes carry no damage;
// lasers are AddProjectile calls whose art contains "laser".
enum class ProjectileKind : int8_t {
  None = 0,
  Artillery = 1,
  Projectile = 2,
  AirStrike = 3,
  Pylon = 4,  // AddDropper
  Laser = 5,
  ReverseAirStrike = 6,
};

enum class MoveKind : int8_t { Walk = 0, Leap = 1, Charge = 2, Melee = 3, Teleport = 4, Burrow = 5 };

// iTerrain sentinel meaning "leave the terrain alone".
inline constexpr int kNoTerrainChange = 10;

// sAnimation flags (+0x34). ANIM_DELAY is the default: the tile counts as
// busy while the animation plays, which is what FULL_DELAY waits for.
inline constexpr uint8_t kAnimNoDelay = 1;
inline constexpr uint8_t kAnimDelay = 2;
inline constexpr uint8_t kAnimReverse = 4;

// The internal mode override (+0xC0): every constructor sets 3; only corpse
// explosions use 2. It is not bound to Lua.
inline constexpr int8_t kModeDefault = 3;
inline constexpr int8_t kModeExplosion = 2;

struct SpaceDamage {
  Point loc = kInvalidPoint;
  int damage = 0;  // may be kDamageZero / kDamageDeath; negative heals
  Dir push = Dir::None;
  int shield = 0;  // >0 adds a shield, <0 removes one
  StatusChange fire = StatusChange::None;
  StatusChange frozen = StatusChange::None;
  StatusChange injure = StatusChange::None;
  StatusChange smoke = StatusChange::None;
  StatusChange acid = StatusChange::None;
  bool crack = false;
  bool ko_effect = false;
  bool boosted = false;  // set when Boost already added +1
  int terrain = kNoTerrainChange;
  Symbol spawn_pawn = kNoSymbol;  // sPawn: create this pawn on the tile
  Team spawn_team = Team::None;
  Team owner_team = Team::None;   // stamped by the firing skill
  bool evacuate = false;
  float delay = kNoDelay;
  Symbol animation = kNoSymbol;   // sAnimation, played on loc
  uint8_t anim_flags = kAnimDelay;
  int8_t mode_override = kModeDefault;
  ProjectileKind projectile = ProjectileKind::None;
  Point projectile_source = kInvalidPoint;
  Symbol art = kNoSymbol;         // projectile / artillery image
  MoveKind move_kind = MoveKind::Walk;
  std::vector<Point> path;  // movement and melee entries only
  Point grapple_source = kInvalidPoint;
  Symbol item = kNoSymbol;  // sItem: place an item
  std::string script;       // sScript: Lua run before the hit

  // SpaceDamage::IsMovement: a path, not a melee, not a projectile.
  bool is_movement() const {
    return !path.empty() && move_kind != MoveKind::Melee && projectile == ProjectileKind::None;
  }
  bool is_melee() const { return move_kind == MoveKind::Melee && !path.empty(); }

  bool operator==(const SpaceDamage&) const = default;  // every field
};

// SpaceDamage(loc, damage, push), the Lua constructor's common form.
SpaceDamage space_damage(Point loc, int damage = 0, Dir push = Dir::None);

// SpaceDamage::Boost: positive damage other than DAMAGE_DEATH gets +1, heals
// of 1..9 get one stronger; DAMAGE_ZERO is untouched.
void boost_damage(SpaceDamage& sd);

struct SkillEffect {
  std::vector<SpaceDamage> effect;    // applied when fired
  std::vector<SpaceDamage> q_effect;  // telegraphed part, applied in the enemy phase
  Point origin = kInvalidPoint;
  Point target = kInvalidPoint;
  int32_t owner = -1;  // firing pawn uid, -1 = none
  Team team = Team::None;  // the firing skill's team (PrepareEffect)
  bool follow_up = false;  // death/explosion effect: keeps the last-shot record

  // The Lua SkillEffect builders, with the game's default delays. Projectile
  // sources left invalid are filled with `origin` by prepare_effect.
  void add_damage(const SpaceDamage& sd) { effect.push_back(sd); }
  void add_queued_damage(const SpaceDamage& sd) { q_effect.push_back(sd); }
  void add_delay(float seconds);  // an empty entry carrying the delay
  // AddProjectile: an art containing "laser" makes a laser (applied at once,
  // its delay forced to 0).
  void add_projectile(Point source, SpaceDamage sd, std::string_view art, float delay = kProjDelay);
  void add_projectile(SpaceDamage sd, std::string_view art, float delay = kProjDelay) {
    add_projectile(kInvalidPoint, std::move(sd), art, delay);
  }
  void add_queued_projectile(SpaceDamage sd, std::string_view art, float delay = kProjDelay);
  void add_artillery(SpaceDamage sd, std::string_view art, float delay = kProjDelay);
  void add_queued_artillery(SpaceDamage sd, std::string_view art, float delay = kProjDelay);
  // AddMelee(origin, sd, delay): the attacker lunges from `origin`.
  void add_melee(Point origin, SpaceDamage sd, float delay = kFullDelay);
  void add_queued_melee(Point origin, SpaceDamage sd, float delay = kFullDelay);
  void add_move(std::vector<Point> path, float delay = kNoDelay);
  void add_leap(std::vector<Point> path, float delay = kNoDelay);
  void add_charge(std::vector<Point> path, float delay = kNoDelay);
  void add_burrow(std::vector<Point> path, float delay = kNoDelay);
  void add_teleport(Point from, Point to, float delay = kNoDelay);
  void add_script(std::string script);
  void add_air_strike(Point loc, std::string_view art = {});
  void add_dropper(SpaceDamage sd, std::string_view art = {});
};

}  // namespace itb
