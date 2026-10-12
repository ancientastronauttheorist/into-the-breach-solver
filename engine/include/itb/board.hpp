// Board state: 8x8 tiles plus the ordered pawn list.
//
// Everything here is plain data so a Board can be copied cheaply during
// search. Rules live elsewhere; this layer only stores state and preserves the
// orderings the native game relies on.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "itb/core.hpp"
#include "itb/symbols.hpp"

namespace itb {

// Tile fire state. A forest that catches fire becomes road and keeps burning
// (BurningForest); natively the state values are 0/1/2.
enum class FireState : int8_t { None = 0, Burning = 1, BurningForest = 2 };

enum class PodState : int8_t { None = 0, Present = 1, Destroyed = 2, Collected = 3 };

struct Tile {
  Terrain terrain = Terrain::Road;
  // Structure HP shared by buildings, mountains (2 = intact) and ice
  // (2 = intact, 1 = cracked ice).
  int8_t hp = 0;
  int8_t max_hp = 0;
  bool populated = false;  // plain building costs grid when hit
  bool shield = false;     // structure shield (buildings/mountains)
  bool frozen = false;     // frozen structure (buildings/mountains)
  bool lava = false;       // natively a flag on a water (or ice) tile
  FireState fire = FireState::None;
  bool smoke = false;
  bool acid = false;       // acid pool on the ground (or acid water)
  bool cracked = false;    // cracked ground: next weapon hit opens a chasm
  bool pending_hole = false;  // iTerrain = HOLE applied; becomes a chasm shortly after
  bool vines = false;
  bool spikes = false;     // injures pawns that end on it
  bool building_on_water = false;  // destroyed building leaves water, not rubble
  bool teleporter = false;         // teleporter pad (blocks cracking)
  PodState pod = PodState::None;
  int8_t conveyor = -1;     // conveyor direction, -1 = none
  uint8_t walls = 0;        // bit d set = wall on the Dir(d) edge
  // Webs belong to tiles (BoardSpace +0x2758 / +0x2750). web_out: bit d set
  // = this tile webs its Dir(d) neighbour (natively a list of directions;
  // BoardSpace::SetGrappled(d)). web_in: how many webs hold this tile
  // (SetGrappled(5) adds one, SetGrappled(4) takes one off). A pawn's
  // Pawn::webbed flag follows them (tile_rules: set_grappled, release_webs,
  // check_webs).
  uint8_t web_out = 0;
  uint8_t web_in = 0;
  // Always 0: fills what would be padding, so a Tile's bytes are its value
  // (Board compares tiles with memcmp).
  uint8_t reserved = 0;
  Symbol item = kNoSymbol;  // mine / item name
  Symbol unique_building = kNoSymbol;  // objective building id
  Symbol custom_tile = kNoSymbol;
  Symbol special_tag = kNoSymbol;      // e.g. "supervolcano"

  bool is_building() const { return terrain == Terrain::Building; }
  bool is_mountain() const { return terrain == Terrain::Mountain; }
  bool is_chasm() const { return terrain == Terrain::Hole; }
  bool is_liquid() const { return terrain == Terrain::Water; }  // includes lava
  bool on_fire() const { return fire != FireState::None; }
  bool has_wall(Dir d) const { return (walls >> static_cast<int>(d)) & 1; }

  bool operator==(const Tile&) const = default;
};

struct QueuedShot {
  int8_t weapon = -1;  // index into Pawn::weapons, -1 = no queued attack
  Point origin = kInvalidPoint;
  Point target = kInvalidPoint;

  bool active() const { return weapon >= 0 && target.valid(); }

  bool operator==(const QueuedShot&) const = default;
};

// Pilot abilities that change rules (pilots.lua Skill names in comments).
enum PilotAbility : uint32_t {
  kPilotNone = 0,
  kPilotArmored = 1u << 0,          // Armored
  kPilotThick = 1u << 1,            // Thick: immune to fire and ACID
  kPilotRockSkill = 1u << 2,        // Rock_Skill
  kPilotRetaliation = 1u << 3,      // Retaliation
  kPilotFlying = 1u << 4,           // Flying
  kPilotDisableImmunity = 1u << 5,  // Disable_Immunity
  kPilotFreezeWalk = 1u << 6,       // Freeze_Walk
  kPilotPainImmunity = 1u << 7,     // Pain_Immunity
  kPilotRoadRunner = 1u << 8,       // Road_Runner
  kPilotShifty = 1u << 9,           // Shifty
  kPilotPostMove = 1u << 10,        // Post_Move
  kPilotDoubleShot = 1u << 11,      // Double_Shot
  kPilotYouthMove = 1u << 12,       // Youth_Move
  kPilotArrogantBoost = 1u << 13,   // Arrogant_Boost
  kPilotRegen = 1u << 14,           // Regen: +1 HP at the end of each enemy phase's ticks
  kPilotZoltan = 1u << 15,          // Zoltan_Skill: shield at turn start, kept by Mission_Shields
  kPilotExtraXp = 1u << 16,         // Extra_XP (Experienced): +2 XP per XP-giving kill
  kPilotKoBoost = 1u << 17,         // KO_Boost: Boost after a kill
};

// AE pilot level-up skills, in Pilot::GetAllPilotSkills order (@0085d0c0):
// the ids the save stores as skill1/skill2.
enum class LevelSkill : int8_t {
  Health = 0,        // +2 Mech HP
  Move = 1,          // +1 Move
  Grid = 2,          // +3 Grid Defense
  Reactor = 3,       // +1 Reactor Core
  Opener = 4,        // Boost and +2 Move on the first turn
  Closer = 5,        // Boost and +2 Move on the last turn
  Popular = 6,       // sells for 4 reputation
  Thick = 7,         // immune to ACID and fire
  Skilled = 8,       // +1 Move, +2 Mech HP
  Invulnerable = 9,  // the pilot survives the mech's defeat
  Adrenaline = 10,   // +1 Move per Vek killed in the battle
  Pain = 11,         // +2 Move when not at full HP
  Regen = 12,        // repair 1 HP at the start of each turn
  Conservative = 13, // limited-use weapons +1 use
};

// Squad passives that change rules. Comments give the Lua weapon name and,
// where it differs, the name the binary checks.
enum Passive : uint32_t {
  kPassiveNone = 0,
  kPassiveForceAmp = 1u << 0,          // Passive_ForceAmp
  kPassiveAutoShield = 1u << 1,        // Passive_AutoShields / "Auto_Shield"
  kPassiveFlameImmune = 1u << 2,       // Passive_FlameImmune / "Flame_Immune"
  kPassiveFireBoost = 1u << 3,         // Passive_FireBoost
  kPassiveHealingSmoke = 1u << 4,      // Passive_HealingSmoke
  kPassivePlayerTurnShield = 1u << 5,  // Passive_PlayerTurnShield
  kPassivePsionLeech = 1u << 6,        // Passive_Psions / "Psion_Leech" (Psionic Receiver)
  kPassiveElectricSmoke = 1u << 7,     // Passive_Electric
  kPassiveBurrows = 1u << 8,           // Passive_Burrows
  kPassiveKickoff = 1u << 9,           // Passive_Boosters (Kickoff Boosters)
  kPassiveKickoffUpgraded = 1u << 10,  // Passive_Boosters_A / "Kickoff_Booster_A" (+2 move)
  // Vek Hormones: enemy shots hitting Vek get +1, +2 with either upgrade, +3
  // with both.
  kPassiveFriendlyFire = 1u << 11,     // Passive_FriendlyFire
  kPassiveFriendlyFireA = 1u << 12,    // Passive_FriendlyFire_A
  kPassiveFriendlyFireB = 1u << 13,    // Passive_FriendlyFire_B
  kPassiveFriendlyFireAB = 1u << 14,   // Passive_FriendlyFire_AB
  kPassiveFastDecay = 1u << 15,        // Passive_FastDecay: dead Vek leave forest
  kPassiveVoidShock = 1u << 16,        // Passive_VoidShock (AE)
  kPassiveElectricSmokeA = 1u << 17,   // Passive_Electric_A: Storm Generator deals 2
  // Viscera Nanobots: a mech heals 1 per Vek it kills (Pawn::UpdateKills), 2
  // with the upgrade.
  kPassiveLeechKill = 1u << 18,        // Passive_Leech / "Leech_Kill"
  kPassiveLeechKillA = 1u << 19,       // Passive_Leech_A / "Leech_Kill_A"
};

inline constexpr int kMaxWeapons = 4;

// Movement bookkeeping beyond Pawn::move / Pawn::moved (stage 5). Natively
// these are separate pawn fields; the defaults describe a plain pawn.
struct MoveState {
  int8_t bonus_shift = 0;     // >0: replaces the move speed (Shifty 1, Post_Move full move)
  int8_t kickoff_bonus = 0;   // Kickoff Boosters bonus, assigned at turn start
  int8_t pilot_bonus = 0;     // recorded speed beyond base_move's live terms (pilot, Adrenaline kills)
  int8_t turn_count = 0;      // turn starts so far; Youth_Move applies while <= 1
  bool move_upgrade = false;  // powered +1 move upgrade
  bool reset_bonus = false;   // Reset_Bonus pilot after Reset Turn: +2 until next turn start
  bool powered = true;        // unpowered pawns can neither act nor move
  bool undo_ready = false;    // the last player move can still be undone
  Point prev_pos = kInvalidPoint;  // last tile left; an underground burrower resurfaces near it

  bool operator==(const MoveState&) const = default;
};

struct Pawn {
  int32_t uid = -1;
  Symbol type = kNoSymbol;
  Point pos = kInvalidPoint;
  int8_t hp = 0;
  int8_t max_hp = 0;
  int8_t move = 0;  // base move speed (Lua MoveSpeed); bonuses live in `movement`
  Team team = Team::None;
  Faction faction = Faction::Default;
  Leader leader = Leader::None;  // own psion/boss leader type
  std::array<Symbol, kMaxWeapons> weapons{};
  // Uses left of a limited weapon (Lua Limited > 0), per weapon slot; -1 =
  // unlimited or not recorded. A weapon at 0 cannot fire.
  std::array<int8_t, kMaxWeapons> uses{-1, -1, -1, -1};
  uint32_t pilot_abilities = kPilotNone;
  // AE pilot experience (itb/pilot_xp.hpp): Pilot::GetLevel (0-2), the
  // save's "exp" (progress toward the next level) and the level-up skill ids
  // (LevelSkill; skill1 is learned at level 1, skill2 at level 2).
  // pilot_level -1: no pilot (Pawn::IsPilot false). pilot_xp -1: not
  // recorded, so no experience is tracked for this pilot.
  int8_t pilot_level = -1;
  int8_t pilot_skill1 = -1;
  int8_t pilot_skill2 = -1;
  int16_t pilot_xp = -1;
  // Multi-tile pawns (Lua ExtraSpaces: Dam_Pawn, the trains): the offset of
  // the second tile from `pos`, (0, 0) for a one-tile pawn. Every shipped
  // definition has at most one extra space. Natively the pawn sits in the
  // BoardSpace of its tile and of each extra tile: Pawn::SetSpace (0087dcb0)
  // moves all of them through Board::MovePawn (0093abd0) and
  // Board::RemovePawn(Pawn*) (0093a980) clears all of them, so every
  // per-tile query (GetPawn, IsPawnSpace, IsBlocked, damage, pushes, tile
  // rules) sees the pawn on the extra tile too. `pos` stays the main tile
  // (Pawn::GetSpace).
  int8_t extra_dx = 0;
  int8_t extra_dy = 0;

  // Static traits (copied from the pawn definition, overridable per board).
  bool mech = false;
  bool massive = false;
  bool flying = false;
  bool pushable = true;
  bool armor = false;
  bool ignore_smoke = false;
  bool ignore_fire = false;
  // Natively one flag (+0x10F0): Lua Minor pawns start with it, and a pawn
  // that retreats (bEvacuate) gets it. Minor pawns are never psion-affected.
  bool minor = false;
  bool corpse = false;  // leaves a corpse (mechs)
  bool burrows = false;
  bool jumper = false;
  bool teleporter = false;
  bool explodes = false;
  bool burns = false;        // Lua Burns (AE): leaves fire when it dies
  bool ignore_flip = false;  // Lua IgnoreFlip: DIR_FLIP leaves its attack alone
  bool neutral = false;
  bool non_grid = false;
  bool mission_critical = false;

  // Status.
  bool active = false;  // can still act this turn
  bool moved = false;   // has used its move this turn
  bool fire = false;
  bool frozen = false;
  bool acid = false;
  bool shield = false;
  bool boosted = false;
  // Pawn +0xFB1 (Pawn::SetGrappled). Natively the web itself is tile state
  // (Tile::web_out / web_in); this flag is what IsGrappled and the move
  // rules read.
  bool webbed = false;
  bool infected = false;
  bool injured = false;  // loses 1 HP whenever it changes tile
  bool dying = false;    // HP reached 0; death not processed yet
  // Carries the Soldier psion's or the Psion Abomination's +1
  // (Pawn::SetMutation(LEADER_HEALTH / LEADER_BOSS) raised max and current HP). Natively a per-pawn mutation that a pawn keeps while
  // a psion of another type, which does not affect it, is the board psion.
  bool health_bonus = false;
  bool fallen = false;   // fell into a chasm: off the board, never a corpse
  // When the pawn entered its tile (Board::stamp_arrival). Natively each tile
  // keeps its occupants in arrival order; pawns that share a stamp (0 for
  // boards loaded without that history) fall back to board-list order.
  uint32_t arrival = 0;
  QueuedShot queued;
  MoveState movement;

  bool alive() const { return hp > 0; }
  bool multi_tile() const { return (extra_dx | extra_dy) != 0; }
  // The extra tile (may be off the board); kInvalidPoint for a one-tile pawn
  // or one that is off the board.
  Point extra_tile() const {
    return multi_tile() && pos.valid() ? Point{pos.x + extra_dx, pos.y + extra_dy} : kInvalidPoint;
  }
  // Whether the pawn is listed in p's BoardSpace: its own tile or its extra
  // tile.
  bool occupies(Point p) const {
    return pos == p || (multi_tile() && pos.valid() && pos.x + extra_dx == p.x && pos.y + extra_dy == p.y);
  }
  bool controlled() const { return team == Team::Player && !neutral; }
  bool has_pilot(PilotAbility a) const { return (pilot_abilities & a) != 0; }
  // Pilot::IsAbility for a level-up skill: learned once the level reaches its slot.
  bool has_level_skill(LevelSkill s) const {
    const int8_t id = static_cast<int8_t>(s);
    return (pilot_level >= 1 && pilot_skill1 == id) || (pilot_level >= 2 && pilot_skill2 == id);
  }

  bool operator==(const Pawn&) const = default;
};

class Board {
 public:
  Tile& tile(Point p) { return tiles_[p.index()]; }
  const Tile& tile(Point p) const { return tiles_[p.index()]; }

  // Pawns in the game's board-list order. That order drives Vek attack order,
  // status ticks and same-frame push resolution.
  const std::vector<Pawn>& pawns() const { return pawns_; }
  std::vector<Pawn>& pawns() { return pawns_; }

  // Appends a pawn and regroups the list the way Board::AddPawn does.
  Pawn& add_pawn(const Pawn& pawn);
  void remove_pawn(int32_t uid);

  // The first pawn standing on p, in tile order (arrival, then list order;
  // a tile rarely holds more than one pawn).
  Pawn* pawn_at(Point p);
  const Pawn* pawn_at(Point p) const;
  // Every pawn standing on p in tile order (corpses and dying pawns can share
  // a tile).
  std::vector<Pawn*> pawns_at(Point p);
  // Records that `pawn` just entered its tile: it goes last in the tile's
  // occupant order.
  void stamp_arrival(Pawn& pawn) { pawn.arrival = ++arrival_clock_; }
  Pawn* find_pawn(int32_t uid);
  const Pawn* find_pawn(int32_t uid) const;

  bool has_passive(Passive p) const { return (passives & p) != 0; }

  // Every field, tiles compared bytewise (Tile has no padding). A new
  // Board field must be added there.
  bool operator==(const Board& other) const;

  int grid_power = 7;
  int grid_power_max = 7;
  int grid_defense = 15;  // percent chance a populated building resists a hit
  int turn = 1;
  int total_turns = 5;
  bool player_phase = true;  // false during the enemy phase
  uint32_t passives = kPassiveNone;  // active squad passives
  // The uid the next new pawn gets. Like the game's pawn id counter it only
  // increases, so a uid is never reused within a turn; add_pawn keeps it
  // above every uid on the board.
  int32_t next_uid = 0;
  Leader psion = Leader::None;       // active Vek psion mutation
  std::vector<Point> spawn_points;   // emerging Vek locations
  // Teleporter pads (Board:AddTeleport), paired 0<->1, 2<->3, ... and, per
  // pad, the uid of the pawn last seen arriving on it (-1: none). A pawn on a
  // pad that is not its recorded occupant is warped (Board::Teleport).
  std::vector<Point> teleporters;
  std::vector<int32_t> teleporter_occupants;
  // EVENT_ACID_DESTROYED raised on this board so far: Pawn::ProcessDeath of a
  // non-mech enemy-team pawn that has ACID at that moment (minor ones too).
  // A running count, never reset: a turn's acid kills are after - before.
  // Kept here because the dead pawn (and its ACID) leaves the board.
  int acid_destroyed = 0;

 private:
  std::array<Tile, kTileCount> tiles_{};
  std::vector<Pawn> pawns_;
  uint32_t arrival_clock_ = 0;
};

}  // namespace itb
