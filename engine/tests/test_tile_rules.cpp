// Stage 2 tile rules. "vNN" test names are the numbered vectors of the tile
// rules spec; the rest cover branches the vectors don't reach.

#include <doctest/doctest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "itb/game_data.hpp"
#include "itb/movement.hpp"
#include "itb/tile_rules.hpp"

using namespace itb;

namespace {

constexpr Point kP{3, 3};
constexpr Point kUp{3, 2};
constexpr Point kRight{4, 3};

struct World {
  Board board;
  RulesContext ctx;
  std::vector<RulesEvent> events;

  World() { ctx.events = &events; }

  Tile& tile(Point p = kP) { return board.tile(p); }
  Pawn& add(const Pawn& p) { return board.add_pawn(p); }
  Pawn& pawn(int32_t uid) { return *board.find_pawn(uid); }

  void hit(SpaceDamage sd) { apply_space_damage(board, sd, ctx); }
  void hit(int damage, Point p = kP) { hit(sd_at(damage, p)); }
  void bump(int damage, Point p = kP) { damage_tile(board, p, damage, DamageMode::Push, ctx); }
  void bump(SpaceDamage sd) { damage_tile(board, sd.loc, sd, DamageMode::Push, ctx); }
  int settle() { return itb::settle(board, ctx); }

  int count(RulesEventType type) const {
    int n = 0;
    for (const RulesEvent& e : events) n += e.type == type;
    return n;
  }
  const RulesEvent* find(RulesEventType type) const {
    for (const RulesEvent& e : events) {
      if (e.type == type) return &e;
    }
    return nullptr;
  }

  static SpaceDamage sd_at(int damage, Point p = kP) {
    SpaceDamage sd;
    sd.loc = p;
    sd.damage = damage;
    return sd;
  }
};

Pawn vek(int32_t uid = 1, int hp = 3, Point pos = kP) {
  Pawn p;
  p.uid = uid;
  p.type = intern("Test_Vek");
  p.pos = pos;
  p.hp = p.max_hp = static_cast<int8_t>(hp);
  p.team = Team::Enemy;
  p.faction = Faction::Default;
  p.active = true;
  return p;
}

Pawn mech(int32_t uid = 10, int hp = 3, Point pos = kP) {
  Pawn p;
  p.uid = uid;
  p.type = intern("Test_Mech");
  p.pos = pos;
  p.hp = p.max_hp = static_cast<int8_t>(hp);
  p.team = Team::Player;
  p.mech = true;
  p.corpse = true;
  p.active = true;
  return p;
}

void make_building(Tile& t, int hp, int max_hp, bool populated = true) {
  t.terrain = Terrain::Building;
  t.hp = static_cast<int8_t>(hp);
  t.max_hp = static_cast<int8_t>(max_hp);
  t.populated = populated;
}

void make_mountain(Tile& t, int hp = 2) {
  t.terrain = Terrain::Mountain;
  t.hp = static_cast<int8_t>(hp);
  t.max_hp = 2;
}

void make_ice(Tile& t, int hp = 2) {
  t.terrain = Terrain::Ice;
  t.hp = static_cast<int8_t>(hp);
  t.max_hp = 2;
}

void make_lava(Tile& t) {
  t.terrain = Terrain::Water;
  t.lava = true;
}

const GameData* game_data() {
  static const GameData* data = [] {
    const auto root = GameData::default_game_root();
    if (root.empty() || !std::filesystem::exists(root / "scripts" / "scripts.lua")) {
      return static_cast<GameData*>(nullptr);
    }
    return new GameData(GameData::load(root));
  }();
  return data;
}

}  // namespace

// ---- Pawn damage math ---------------------------------------------------------

TEST_CASE("v01 plain hit") {
  World w;
  w.add(vek(1, 3));
  w.hit(2);
  CHECK(w.pawn(1).hp == 1);
  CHECK(w.count(RulesEventType::PawnDamaged) == 1);
}

TEST_CASE("v02 armor reduces weapon damage") {
  World w;
  Pawn p = vek(1, 3);
  p.armor = true;
  w.add(p);
  w.hit(2);
  CHECK(w.pawn(1).hp == 2);
}

TEST_CASE("v03 armor absorbs a 1-damage hit: infected cleared, no dive, no retaliation") {
  World w;
  Pawn p = vek(1, 3);
  p.armor = true;
  p.infected = true;
  p.burrows = true;
  w.add(p);
  w.hit(1);
  CHECK(w.pawn(1).hp == 3);
  CHECK_FALSE(w.pawn(1).infected);
  CHECK(w.ctx.burrow_dives.empty());

  // An armored Retaliation mech that loses nothing does not retaliate.
  World r;
  Pawn m = mech(10, 3);
  m.pilot_abilities = kPilotArmored | kPilotRetaliation;
  r.add(m);
  r.add(vek(1, 2, kUp));
  r.hit(1);
  CHECK(r.pawn(10).hp == 3);
  CHECK(r.pawn(1).hp == 2);
}

TEST_CASE("v04 ACID disables armor and doubles") {
  World w;
  Pawn p = vek(1, 4);
  p.armor = true;
  p.acid = true;
  w.add(p);
  w.hit(1);
  CHECK(w.pawn(1).hp == 2);
}

TEST_CASE("v05 ACID from a hit lands after the damage, no tile pool") {
  World w;
  w.add(vek(1, 3));
  SpaceDamage sd = World::sd_at(1);
  sd.acid = StatusChange::Apply;
  w.hit(sd);
  CHECK(w.pawn(1).hp == 2);
  CHECK(w.pawn(1).acid);
  CHECK_FALSE(w.tile().acid);
}

TEST_CASE("v06 bumps ignore ACID") {
  World w;
  Pawn p = vek(1, 3);
  p.acid = true;
  w.add(p);
  w.bump(1);
  CHECK(w.pawn(1).hp == 2);
}

TEST_CASE("v07 damaging freeze on a shielded pawn pops the shield and freezes") {
  World w;
  Pawn p = vek(1, 3);
  p.shield = true;
  w.add(p);
  SpaceDamage sd = World::sd_at(2);
  sd.frozen = StatusChange::Apply;
  w.hit(sd);
  CHECK_FALSE(w.pawn(1).shield);
  CHECK(w.pawn(1).hp == 3);
  CHECK(w.pawn(1).frozen);
}

TEST_CASE("v08 zero-damage freeze on a shielded pawn does nothing") {
  World w;
  Pawn p = vek(1, 3);
  p.shield = true;
  w.add(p);
  SpaceDamage sd = World::sd_at(0);
  sd.frozen = StatusChange::Apply;
  w.hit(sd);
  CHECK(w.pawn(1).shield);
  CHECK_FALSE(w.pawn(1).frozen);
}

TEST_CASE("v09 shield blocks catching fire, also at settle") {
  World w;
  Pawn p = vek(1, 3);
  p.shield = true;
  w.add(p);
  SpaceDamage sd = World::sd_at(0);
  sd.fire = StatusChange::Apply;
  w.hit(sd);
  CHECK(w.pawn(1).shield);
  CHECK_FALSE(w.pawn(1).fire);
  CHECK(w.tile().on_fire());
  w.settle();
  CHECK_FALSE(w.pawn(1).fire);
}

TEST_CASE("v10 a popped shield exposes the pawn to tile fire at once") {
  World w;
  Pawn p = vek(1, 3);
  p.shield = true;
  w.add(p);
  w.tile().fire = FireState::Burning;
  w.hit(1);
  CHECK_FALSE(w.pawn(1).shield);
  CHECK(w.pawn(1).hp == 3);
  CHECK(w.pawn(1).fire);
}

TEST_CASE("v11 frozen absorbs a hit and thaws") {
  World w;
  Pawn p = vek(1, 2);
  p.frozen = true;
  w.add(p);
  w.hit(3);
  CHECK_FALSE(w.pawn(1).frozen);
  CHECK(w.pawn(1).hp == 2);
}

TEST_CASE("v12 DAMAGE_DEATH thaws and still kills") {
  World w;
  Pawn p = vek(1, 3);
  p.frozen = true;
  w.add(p);
  w.hit(kDamageDeath);
  CHECK_FALSE(w.pawn(1).frozen);
  CHECK(w.pawn(1).hp == 0);
  CHECK(w.pawn(1).dying);
  CHECK(w.count(RulesEventType::PawnKilled) == 1);
}

TEST_CASE("v13 DAMAGE_DEATH pops a shield and still kills") {
  World w;
  Pawn p = vek(1, 3);
  p.shield = true;
  w.add(p);
  w.hit(kDamageDeath);
  CHECK_FALSE(w.pawn(1).shield);
  CHECK_FALSE(w.pawn(1).alive());
}

TEST_CASE("v14 one hit consumes one absorber, shield first") {
  World w;
  Pawn p = vek(1, 3);
  p.shield = true;
  p.frozen = true;
  w.add(p);
  w.hit(1);
  CHECK_FALSE(w.pawn(1).shield);
  CHECK(w.pawn(1).frozen);
  CHECK(w.pawn(1).hp == 3);
}

TEST_CASE("v15 iShield -1 removes the shield before the hit") {
  World w;
  Pawn p = vek(1, 3);
  p.shield = true;
  w.add(p);
  SpaceDamage sd = World::sd_at(1);
  sd.shield = -1;
  w.hit(sd);
  CHECK_FALSE(w.pawn(1).shield);
  CHECK(w.pawn(1).hp == 2);
}

TEST_CASE("v16 an SD that shields and damages absorbs itself") {
  World w;
  w.add(vek(1, 3));
  SpaceDamage sd = World::sd_at(1);
  sd.shield = 1;
  w.hit(sd);
  CHECK_FALSE(w.pawn(1).shield);
  CHECK(w.pawn(1).hp == 3);
}

// ---- Terrain ------------------------------------------------------------------

TEST_CASE("v17 mountains lose 1 HP per hit") {
  World w;
  make_mountain(w.tile(), 2);
  w.hit(3);
  CHECK(w.tile().is_mountain());
  CHECK(w.tile().hp == 1);
}

TEST_CASE("v18 damaged mountain becomes rubble") {
  World w;
  make_mountain(w.tile(), 1);
  w.hit(1);
  CHECK(w.tile().terrain == Terrain::Rubble);
  CHECK(w.find(RulesEventType::TerrainChanged)->amount == static_cast<int>(Terrain::Rubble));
}

TEST_CASE("v19 DAMAGE_DEATH flattens a mountain") {
  World w;
  make_mountain(w.tile(), 2);
  w.hit(kDamageDeath);
  CHECK(w.tile().terrain == Terrain::Rubble);
}

TEST_CASE("v20 bumps damage mountains") {
  World w;
  make_mountain(w.tile(), 2);
  w.bump(1);
  CHECK(w.tile().hp == 1);
}

TEST_CASE("v21 structure shield absorbs a hit on a mountain") {
  World w;
  make_mountain(w.tile(), 2);
  w.tile().shield = true;
  w.hit(2);
  CHECK_FALSE(w.tile().shield);
  CHECK(w.tile().hp == 2);
}

TEST_CASE("v22 ice loses 1 HP per hit") {
  World w;
  make_ice(w.tile(), 2);
  w.hit(3);
  CHECK(w.tile().terrain == Terrain::Ice);
  CHECK(w.tile().hp == 1);
}

TEST_CASE("v23 ice loses only 1 HP to DAMAGE_DEATH") {
  World w;
  make_ice(w.tile(), 2);
  w.hit(kDamageDeath);
  CHECK(w.tile().terrain == Terrain::Ice);
  CHECK(w.tile().hp == 1);
}

TEST_CASE("v24 broken ice becomes water and drowns at settle") {
  World w;
  make_ice(w.tile(), 1);
  w.add(vek(1, 3));
  w.hit(1);
  CHECK(w.tile().terrain == Terrain::Water);
  CHECK(w.pawn(1).hp == 2);
  w.settle();
  CHECK_FALSE(w.pawn(1).alive());
  CHECK(w.pawn(1).dying);
}

TEST_CASE("v25 bumps don't crack ice") {
  World w;
  make_ice(w.tile(), 2);
  w.bump(1);
  CHECK(w.tile().terrain == Terrain::Ice);
  CHECK(w.tile().hp == 2);
}

TEST_CASE("v26 thawing a pawn on ice refills the ice") {
  World w;
  make_ice(w.tile(), 1);
  Pawn p = vek(1, 3);
  p.frozen = true;
  w.add(p);
  w.hit(1);
  CHECK_FALSE(w.pawn(1).frozen);
  CHECK(w.pawn(1).hp == 3);
  CHECK(w.tile().terrain == Terrain::Ice);
  CHECK(w.tile().hp == 2);
}

TEST_CASE("v27 iFrozen = remove refills ice too") {
  World w;
  make_ice(w.tile(), 1);
  SpaceDamage sd = World::sd_at(0);
  sd.frozen = StatusChange::Remove;
  w.hit(sd);
  CHECK(w.tile().hp == 2);
}

TEST_CASE("v28 shooting a forest sets it on fire") {
  World w;
  w.tile().terrain = Terrain::Forest;
  w.hit(1);
  CHECK(w.tile().terrain == Terrain::Road);
  CHECK(w.tile().fire == FireState::BurningForest);
}

TEST_CASE("v29 iTerrain suppresses forest ignition") {
  World w;
  w.tile().terrain = Terrain::Forest;
  SpaceDamage sd = World::sd_at(1);
  sd.terrain = static_cast<int>(Terrain::Sand);
  w.hit(sd);
  CHECK(w.tile().terrain == Terrain::Sand);
  CHECK_FALSE(w.tile().on_fire());
}

TEST_CASE("v30 bumps don't ignite forests") {
  World w;
  w.tile().terrain = Terrain::Forest;
  w.bump(1);
  CHECK(w.tile().terrain == Terrain::Forest);
  CHECK_FALSE(w.tile().on_fire());
}

TEST_CASE("v31 forest fire reaches its pawn at settle") {
  World w;
  w.tile().terrain = Terrain::Forest;
  w.add(vek(1, 3));
  w.hit(1);
  CHECK(w.pawn(1).hp == 2);
  CHECK_FALSE(w.pawn(1).fire);
  CHECK(w.tile().terrain == Terrain::Road);
  CHECK(w.tile().on_fire());
  w.settle();
  CHECK(w.pawn(1).fire);
}

TEST_CASE("v32 a pawn pushed off a forest it set alight does not burn") {
  World w;
  w.tile().terrain = Terrain::Forest;
  w.add(vek(1, 3));
  SpaceDamage sd = World::sd_at(1);
  sd.push = Dir::Right;
  w.hit(sd);
  REQUIRE(w.ctx.pushes.size() == 1);
  CHECK(w.ctx.pushes[0].uid == 1);
  CHECK(w.ctx.pushes[0].dir == Dir::Right);
  CHECK(w.ctx.pushes[0].from == kP);
  CHECK_FALSE(w.pawn(1).fire);
  CHECK(w.tile().terrain == Terrain::Road);
  CHECK(w.tile().on_fire());
  // Stage 3 lands the push before the next settle.
  w.pawn(1).pos = kRight;
  w.settle();
  CHECK_FALSE(w.pawn(1).fire);
}

TEST_CASE("v33 shooting sand makes smoke") {
  World w;
  w.tile().terrain = Terrain::Sand;
  w.hit(1);
  CHECK(w.tile().terrain == Terrain::Road);
  CHECK(w.tile().smoke);
}

TEST_CASE("v34 bumps leave sand alone") {
  World w;
  w.tile().terrain = Terrain::Sand;
  w.bump(1);
  CHECK(w.tile().terrain == Terrain::Sand);
  CHECK_FALSE(w.tile().smoke);
}

// ---- Buildings and grid ---------------------------------------------------------

TEST_CASE("v35 populated building: grid loss is capped by building HP") {
  World w;
  w.board.grid_power = 5;
  make_building(w.tile(), 1, 1);
  w.hit(2);
  CHECK(w.tile().terrain == Terrain::Rubble);
  CHECK(w.board.grid_power == 4);
  REQUIRE(w.find(RulesEventType::GridDamaged));
  CHECK(w.find(RulesEventType::GridDamaged)->amount == 1);
  CHECK(w.count(RulesEventType::BuildingDestroyed) == 1);
}

TEST_CASE("v36 a resisted hit leaves the building untouched") {
  World w;
  make_building(w.tile(), 2, 2);
  std::vector<std::pair<Point, int>> calls;
  w.ctx.grid_resist = [&](Point p, int n) {
    calls.emplace_back(p, n);
    return true;
  };
  w.hit(1);
  CHECK(w.tile().hp == 2);
  CHECK(w.board.grid_power == 7);
  REQUIRE(calls.size() == 1);
  CHECK(calls[0].first == kP);
  CHECK(calls[0].second == 1);
  REQUIRE(w.find(RulesEventType::GridResisted));
  CHECK(w.find(RulesEventType::GridResisted)->amount == 1);
  CHECK(w.count(RulesEventType::GridDamaged) == 0);
  CHECK(w.count(RulesEventType::BuildingDamaged) == 0);
}

TEST_CASE("v37 unpopulated building loses HP but no grid") {
  World w;
  make_building(w.tile(), 1, 1, /*populated=*/false);
  bool rolled = false;
  w.ctx.grid_resist = [&](Point, int) { return rolled = true; };
  w.hit(1);
  CHECK(w.tile().terrain == Terrain::Rubble);
  CHECK(w.board.grid_power == 7);
  CHECK_FALSE(rolled);
}

TEST_CASE("v38 building shield absorbs without a roll") {
  World w;
  make_building(w.tile(), 2, 2);
  w.tile().shield = true;
  bool rolled = false;
  w.ctx.grid_resist = [&](Point, int) { return rolled = true; };
  w.hit(3);
  CHECK_FALSE(w.tile().shield);
  CHECK(w.tile().hp == 2);
  CHECK_FALSE(rolled);
  CHECK(w.board.grid_power == 7);
}

TEST_CASE("v39 a frozen building absorbs a bump") {
  World w;
  make_building(w.tile(), 1, 1);
  w.tile().frozen = true;
  w.bump(1);
  CHECK_FALSE(w.tile().frozen);
  CHECK(w.tile().hp == 1);
  CHECK(w.board.grid_power == 7);
}

TEST_CASE("v40 Auto_Shield re-shields a surviving building") {
  World w;
  w.board.passives = kPassiveAutoShield;
  make_building(w.tile(), 2, 2);
  w.hit(1);
  CHECK(w.tile().hp == 1);
  CHECK(w.board.grid_power == 6);
  CHECK(w.tile().shield);
}

TEST_CASE("v41 unique buildings are populated only at full HP and never rubble") {
  World w;
  make_building(w.tile(), 2, 2, /*populated=*/false);
  w.tile().unique_building = intern("str_power1");
  w.hit(1);
  CHECK(w.tile().hp == 1);
  CHECK(w.board.grid_power == 6);
  w.hit(1);
  CHECK(w.tile().hp == 0);
  CHECK(w.board.grid_power == 6);
  CHECK(w.tile().terrain == Terrain::Building);
}

TEST_CASE("v42 a building raised on water leaves water") {
  World w;
  make_building(w.tile(), 1, 1);
  w.tile().building_on_water = true;
  w.hit(1);
  CHECK(w.tile().terrain == Terrain::Water);
}

TEST_CASE("v43 DAMAGE_DEATH on a 2-HP building costs 2 grid") {
  World w;
  make_building(w.tile(), 2, 2);
  w.hit(kDamageDeath);
  CHECK(w.tile().terrain == Terrain::Rubble);
  CHECK(w.board.grid_power == 5);
}

// ---- Cracks -------------------------------------------------------------------

TEST_CASE("v44 weapon damage collapses a cracked tile, the pawn falls at settle") {
  World w;
  w.tile().cracked = true;
  w.add(vek(1, 3));
  w.hit(1);
  CHECK(w.pawn(1).hp == 2);
  CHECK(w.tile().is_chasm());
  CHECK_FALSE(w.tile().cracked);
  w.settle();
  CHECK(w.pawn(1).fallen);
  CHECK_FALSE(w.pawn(1).alive());
  CHECK(w.count(RulesEventType::PawnFell) == 1);
}

TEST_CASE("v45 bumps don't collapse cracks") {
  World w;
  w.tile().cracked = true;
  w.bump(1);
  CHECK(w.tile().terrain == Terrain::Road);
  CHECK(w.tile().cracked);
}

TEST_CASE("v46 train shots don't collapse cracks") {
  for (const char* shot : {"Train_Move", "Armored_Train_Move"}) {
    World w;
    w.ctx.current_shot = intern(shot);
    w.tile().cracked = true;
    w.hit(1);
    CHECK(w.tile().terrain == Terrain::Road);
    CHECK(w.tile().cracked);
  }
}

TEST_CASE("v47 an absorbed hit doesn't collapse the crack") {
  World w;
  w.tile().cracked = true;
  Pawn p = vek(1, 3);
  p.shield = true;
  w.add(p);
  w.hit(1);
  CHECK_FALSE(w.pawn(1).shield);
  CHECK(w.tile().terrain == Terrain::Road);
  CHECK(w.tile().cracked);
}

TEST_CASE("v48 cracking a mountain damages it instead") {
  World w;
  make_mountain(w.tile(), 2);
  SpaceDamage sd = World::sd_at(0);
  sd.crack = true;
  w.hit(sd);
  CHECK(w.tile().hp == 1);
  CHECK_FALSE(w.tile().cracked);
}

// ---- Freezing, water, lava -----------------------------------------------------

TEST_CASE("v49 lava can't be frozen, nor its pawn") {
  World w;
  make_lava(w.tile());
  Pawn p = vek(1, 3);
  p.flying = true;
  w.add(p);
  SpaceDamage sd = World::sd_at(0);
  sd.frozen = StatusChange::Apply;
  w.hit(sd);
  CHECK(w.tile().terrain == Terrain::Water);
  CHECK_FALSE(w.pawn(1).frozen);
}

TEST_CASE("v50 freezing water makes ice and freezes the flyer") {
  World w;
  w.tile().terrain = Terrain::Water;
  Pawn p = vek(1, 3);
  p.flying = true;
  w.add(p);
  SpaceDamage sd = World::sd_at(0);
  sd.frozen = StatusChange::Apply;
  w.hit(sd);
  CHECK(w.tile().terrain == Terrain::Ice);
  CHECK(w.tile().hp == 2);
  CHECK(w.pawn(1).frozen);
}

TEST_CASE("v51 frozen flyers drown") {
  World w;
  w.tile().terrain = Terrain::Water;
  Pawn p = vek(1, 3);
  p.flying = true;
  p.frozen = true;
  w.add(p);
  w.settle();
  CHECK_FALSE(w.pawn(1).alive());
}

TEST_CASE("v52 massive pawns thaw in water") {
  World w;
  w.tile().terrain = Terrain::Water;
  Pawn p = vek(1, 3);
  p.massive = true;
  p.frozen = true;
  w.add(p);
  w.settle();
  CHECK_FALSE(w.pawn(1).frozen);
  CHECK(w.pawn(1).alive());
}

TEST_CASE("v53 an ACID massive pawn turns water acidic") {
  World w;
  w.tile().terrain = Terrain::Water;
  Pawn p = vek(1, 3);
  p.massive = true;
  p.acid = true;
  w.add(p);
  w.settle();
  CHECK(w.tile().acid);
  CHECK(w.pawn(1).acid);
}

TEST_CASE("v54 acid water coats ground pawns and stays") {
  World w;
  w.tile().terrain = Terrain::Water;
  w.tile().acid = true;
  Pawn p = vek(1, 3);
  p.massive = true;
  w.add(p);
  w.settle();
  CHECK(w.pawn(1).acid);
  CHECK(w.tile().acid);
}

TEST_CASE("v55 acid water spares flyers") {
  World w;
  w.tile().terrain = Terrain::Water;
  w.tile().acid = true;
  Pawn p = vek(1, 3);
  p.flying = true;
  w.add(p);
  w.settle();
  CHECK_FALSE(w.pawn(1).acid);
  CHECK(w.tile().acid);
}

TEST_CASE("v56 flyers pick up ground acid pools") {
  World w;
  w.tile().acid = true;
  Pawn p = vek(1, 3);
  p.flying = true;
  w.add(p);
  w.settle();
  CHECK(w.pawn(1).acid);
  CHECK_FALSE(w.tile().acid);
}

TEST_CASE("v57 acid on ice is inert") {
  World w;
  make_ice(w.tile(), 2);
  w.tile().acid = true;
  w.add(vek(1, 3));
  w.settle();
  CHECK_FALSE(w.pawn(1).acid);
  CHECK(w.tile().acid);
}

TEST_CASE("v58 a shielded pawn's acid goes to the tile, and later to the pawn") {
  World w;
  Pawn p = vek(1, 3);
  p.shield = true;
  w.add(p);
  SpaceDamage sd = World::sd_at(0);
  sd.acid = StatusChange::Apply;
  w.hit(sd);
  CHECK_FALSE(w.pawn(1).acid);
  CHECK(w.tile().acid);
  w.settle();
  CHECK_FALSE(w.pawn(1).acid);
  w.pawn(1).shield = false;  // broken later
  w.settle();
  CHECK(w.pawn(1).acid);
  CHECK_FALSE(w.tile().acid);
}

TEST_CASE("v59 acid puts out a fire") {
  World w;
  w.tile().fire = FireState::Burning;
  SpaceDamage sd = World::sd_at(0);
  sd.acid = StatusChange::Apply;
  w.hit(sd);
  CHECK(w.tile().acid);
  CHECK_FALSE(w.tile().on_fire());
}

TEST_CASE("v60 acid can't be placed on lava") {
  World w;
  make_lava(w.tile());
  SpaceDamage sd = World::sd_at(0);
  sd.acid = StatusChange::Apply;
  w.hit(sd);
  CHECK_FALSE(w.tile().acid);
}

TEST_CASE("v61 fire melts ice") {
  World w;
  make_ice(w.tile(), 2);
  SpaceDamage sd = World::sd_at(0);
  sd.fire = StatusChange::Apply;
  w.hit(sd);
  CHECK(w.tile().terrain == Terrain::Water);
  CHECK_FALSE(w.tile().on_fire());
}

TEST_CASE("v62 water doesn't burn, a flyer over it does") {
  World w;
  w.tile().terrain = Terrain::Water;
  Pawn p = vek(1, 3);
  p.flying = true;
  w.add(p);
  SpaceDamage sd = World::sd_at(0);
  sd.fire = StatusChange::Apply;
  w.hit(sd);
  CHECK_FALSE(w.tile().on_fire());
  CHECK(w.pawn(1).fire);
}

TEST_CASE("v63 fire clears smoke") {
  World w;
  w.tile().smoke = true;
  SpaceDamage sd = World::sd_at(0);
  sd.fire = StatusChange::Apply;
  w.hit(sd);
  CHECK_FALSE(w.tile().smoke);
  CHECK(w.tile().fire == FireState::Burning);
}

TEST_CASE("v64 smoke clears tile fire now and pawn fire at settle") {
  World w;
  w.tile().fire = FireState::Burning;
  Pawn p = vek(1, 3);
  p.fire = true;
  w.add(p);
  SpaceDamage sd = World::sd_at(0);
  sd.smoke = StatusChange::Apply;
  w.hit(sd);
  CHECK(w.tile().smoke);
  CHECK_FALSE(w.tile().on_fire());
  CHECK(w.pawn(1).fire);
  w.settle();
  CHECK_FALSE(w.pawn(1).fire);
}

TEST_CASE("v65 fire thaws a frozen pawn") {
  World w;
  Pawn p = vek(1, 3);
  p.frozen = true;
  w.add(p);
  SpaceDamage sd = World::sd_at(0);
  sd.fire = StatusChange::Apply;
  w.hit(sd);
  CHECK(w.pawn(1).fire);
  CHECK_FALSE(w.pawn(1).frozen);
  CHECK(w.tile().on_fire());
}

TEST_CASE("v66 freezing puts out pawn and tile fire") {
  World w;
  w.tile().fire = FireState::Burning;
  Pawn p = vek(1, 3);
  p.fire = true;
  w.add(p);
  SpaceDamage sd = World::sd_at(0);
  sd.frozen = StatusChange::Apply;
  w.hit(sd);
  CHECK(w.pawn(1).frozen);
  CHECK_FALSE(w.pawn(1).fire);
  CHECK_FALSE(w.tile().on_fire());
}

// ---- Heals ----------------------------------------------------------------------

TEST_CASE("v67 a heal clears fire, ACID and frozen") {
  World w;
  Pawn p = vek(1, 3);
  p.hp = 1;
  p.fire = true;
  p.acid = true;
  p.frozen = true;
  w.add(p);
  w.hit(-1);
  CHECK(w.pawn(1).hp == 2);
  CHECK_FALSE(w.pawn(1).fire);
  CHECK_FALSE(w.pawn(1).acid);
  CHECK_FALSE(w.pawn(1).frozen);
}

TEST_CASE("v68 a heal at full HP still clears ACID") {
  World w;
  Pawn p = vek(1, 3);
  p.acid = true;
  w.add(p);
  w.hit(-1);
  CHECK(w.pawn(1).hp == 3);
  CHECK_FALSE(w.pawn(1).acid);
}

TEST_CASE("v69 a heal with iAcid ends without ACID") {
  World w;
  w.add(vek(1, 3));
  SpaceDamage sd = World::sd_at(-1);
  sd.acid = StatusChange::Apply;
  w.hit(sd);
  CHECK_FALSE(w.pawn(1).acid);
}

TEST_CASE("v70 healing a mech corpse revives it") {
  World w;
  Pawn p = mech(10, 3);
  p.hp = 0;
  w.add(p);
  w.hit(-10);
  CHECK(w.pawn(10).hp == 3);
  CHECK(w.pawn(10).alive());
  CHECK(w.count(RulesEventType::PawnRevived) == 1);
}

TEST_CASE("v71 corpses ignore ACID, and it doesn't pool") {
  World w;
  Pawn p = mech(10, 3);
  p.hp = 0;
  w.add(p);
  SpaceDamage sd = World::sd_at(0);
  sd.acid = StatusChange::Apply;
  w.hit(sd);
  CHECK_FALSE(w.pawn(10).acid);
  CHECK_FALSE(w.tile().acid);
}

// ---- Networked Shielding ----------------------------------------------------------

TEST_CASE("v72-v75 Networked Shielding on a mech during the player turn") {
  auto setup = [](World& w, bool shield) -> Pawn& {
    w.board.passives = kPassivePlayerTurnShield;
    w.board.player_phase = true;
    Pawn p = mech(10, 3);
    p.shield = shield;
    return w.add(p);
  };
  SUBCASE("v72 damage blocked") {
    World w;
    setup(w, false);
    w.hit(2);
    CHECK(w.pawn(10).hp == 3);
  }
  SUBCASE("v73 a real shield survives") {
    World w;
    setup(w, true);
    w.hit(2);
    CHECK(w.pawn(10).hp == 3);
    CHECK(w.pawn(10).shield);
  }
  SUBCASE("v74 even DAMAGE_DEATH") {
    World w;
    setup(w, false);
    w.hit(kDamageDeath);
    CHECK(w.pawn(10).hp == 3);
    CHECK(w.pawn(10).alive());
  }
  SUBCASE("v75 heals pass") {
    World w;
    setup(w, false).hp = 2;
    w.hit(-1);
    CHECK(w.pawn(10).hp == 3);
  }
  SUBCASE("not in the enemy phase") {
    World w;
    setup(w, false);
    w.board.player_phase = false;
    w.hit(2);
    CHECK(w.pawn(10).hp == 1);
  }
}

// ---- Force Amp, Retaliation, burrowers ---------------------------------------------

TEST_CASE("v76 Force Amp adds 1 to bumps on Vek") {
  World w;
  w.board.passives = kPassiveForceAmp;
  w.add(vek(1, 3));
  w.bump(1);
  CHECK(w.pawn(1).hp == 1);
}

TEST_CASE("v77 Force Amp ignores bots") {
  World w;
  w.board.passives = kPassiveForceAmp;
  Pawn p = vek(1, 3);
  p.faction = Faction::Bots;
  w.add(p);
  w.bump(1);
  CHECK(w.pawn(1).hp == 2);
}

TEST_CASE("v78 a shield absorbs the amplified bump") {
  World w;
  w.board.passives = kPassiveForceAmp;
  Pawn p = vek(1, 3);
  p.shield = true;
  w.add(p);
  w.bump(1);
  CHECK_FALSE(w.pawn(1).shield);
  CHECK(w.pawn(1).hp == 3);
}

TEST_CASE("v79 Retaliation hits adjacent enemies in weapon mode") {
  World w;
  Pawn m = mech(10, 3);
  m.pilot_abilities = kPilotRetaliation;
  w.add(m);
  w.add(vek(1, 2, kUp));
  Pawn armored = vek(2, 2, kRight);
  armored.armor = true;
  w.add(armored);
  w.hit(1);
  CHECK(w.pawn(10).hp == 2);
  CHECK(w.pawn(1).hp == 1);
  CHECK(w.pawn(2).hp == 2);
}

TEST_CASE("v80 a mech killed by the hit doesn't retaliate") {
  World w;
  Pawn m = mech(10, 3);
  m.hp = 1;
  m.pilot_abilities = kPilotRetaliation;
  w.add(m);
  w.add(vek(1, 2, kUp));
  w.hit(1);
  CHECK_FALSE(w.pawn(10).alive());
  CHECK(w.pawn(10).dying);
  CHECK(w.pawn(1).hp == 2);
}

TEST_CASE("v81 a hurt burrower dives") {
  World w;
  Pawn p = vek(1, 3);
  p.burrows = true;
  p.fire = true;
  p.queued = QueuedShot{0, kP, kRight};
  w.add(p);
  w.hit(1);
  CHECK(w.pawn(1).hp == 2);
  CHECK(w.ctx.burrow_dives == std::vector<int32_t>{1});
  CHECK_FALSE(w.pawn(1).queued.active());
  CHECK_FALSE(w.pawn(1).fire);
}

TEST_CASE("v82 burrowers on cracked tiles dive from bumps only") {
  SUBCASE("weapon") {
    World w;
    w.tile().cracked = true;
    Pawn p = vek(1, 3);
    p.burrows = true;
    w.add(p);
    w.hit(1);
    CHECK(w.ctx.burrow_dives.empty());
    CHECK(w.tile().is_chasm());
  }
  SUBCASE("bump") {
    World w;
    w.tile().cracked = true;
    Pawn p = vek(1, 3);
    p.burrows = true;
    w.add(p);
    w.bump(1);
    CHECK(w.ctx.burrow_dives == std::vector<int32_t>{1});
    CHECK(w.tile().cracked);
  }
}

// ---- Fire immunity -------------------------------------------------------------

TEST_CASE("v83 IgnoreFire pawns don't catch fire") {
  World w;
  Pawn p = vek(1, 3);
  p.ignore_fire = true;
  w.add(p);
  SpaceDamage sd = World::sd_at(0);
  sd.fire = StatusChange::Apply;
  w.hit(sd);
  CHECK_FALSE(w.pawn(1).fire);
  CHECK(w.tile().on_fire());
}

TEST_CASE("v84 Heat Engines: immune mechs burn, then absorb the fire") {
  World w;
  w.board.passives = kPassiveFlameImmune | kPassiveFireBoost;
  w.add(mech(10, 3));
  SpaceDamage sd = World::sd_at(0);
  sd.fire = StatusChange::Apply;
  w.hit(sd);
  CHECK(w.pawn(10).fire);
  w.settle();
  CHECK_FALSE(w.pawn(10).fire);
  CHECK(w.pawn(10).boosted);
  CHECK_FALSE(w.tile().on_fire());
}

TEST_CASE("v85 Thick pilots ignore fire and ACID") {
  World w;
  Pawn m = mech(10, 3);
  m.pilot_abilities = kPilotThick;
  w.add(m);
  SpaceDamage sd = World::sd_at(0);
  sd.fire = StatusChange::Apply;
  sd.acid = StatusChange::Apply;
  w.hit(sd);
  CHECK_FALSE(w.pawn(10).fire);
  CHECK_FALSE(w.pawn(10).acid);
  CHECK(w.tile().on_fire());
  CHECK_FALSE(w.tile().acid);
}

// ---- Items and pods ----------------------------------------------------------------

TEST_CASE("v86 stepping on a mine kills, through a shield") {
  for (bool shielded : {false, true}) {
    World w;
    w.tile().item = intern("Item_Mine");
    Pawn p = vek(1, 3, kRight);
    p.shield = shielded;
    w.add(p);
    w.pawn(1).pos = kP;  // moved onto the mine
    w.settle();
    CHECK(w.tile().item == kNoSymbol);
    CHECK_FALSE(w.pawn(1).alive());
    CHECK_FALSE(w.pawn(1).shield);
    CHECK(w.count(RulesEventType::ItemTriggered) == 1);
  }
}

TEST_CASE("v87 a shield blocks a freeze mine, which is still used up") {
  World w;
  w.tile().item = intern("Freeze_Mine");
  Pawn p = vek(1, 3);
  p.shield = true;
  w.add(p);
  w.settle();
  CHECK(w.tile().item == kNoSymbol);
  CHECK_FALSE(w.pawn(1).frozen);
  CHECK(w.pawn(1).shield);
}

TEST_CASE("v88 Repair Mines survive weapon damage") {
  World w;
  w.tile().item = intern("Item_Repair_Mine");
  w.hit(1);
  CHECK(w.tile().item == intern("Item_Repair_Mine"));
}

TEST_CASE("v89 fire sets off a Repair Mine") {
  World w;
  w.tile().item = intern("Item_Repair_Mine");
  SpaceDamage sd = World::sd_at(0);
  sd.fire = StatusChange::Apply;
  w.hit(sd);
  CHECK(w.tile().item == kNoSymbol);
  CHECK(w.count(RulesEventType::ItemTriggered) == 1);
}

TEST_CASE("v90 a shot mine detonates, and the crack still collapses") {
  World w;
  w.tile().item = intern("Item_Mine");
  w.tile().cracked = true;
  w.hit(1);
  CHECK(w.tile().item == kNoSymbol);
  CHECK(w.count(RulesEventType::ItemTriggered) == 1);
  CHECK(w.tile().is_chasm());
}

TEST_CASE("v91 weapons break pods, bumps don't") {
  World w;
  w.tile().pod = PodState::Present;
  w.bump(1);
  CHECK(w.tile().pod == PodState::Present);
  w.hit(1);
  CHECK(w.tile().pod == PodState::Destroyed);
  CHECK(w.count(RulesEventType::PodDestroyed) == 1);
}

// ---- sPawn and iTerrain -------------------------------------------------------------

TEST_CASE("v92 sPawn into water: created, drowns at settle") {
  SpaceDamage sd = World::sd_at(0);
  sd.spawn_pawn = intern("Firefly1");
  SUBCASE("without game data the spawn is recorded") {
    World w;
    w.tile().terrain = Terrain::Water;
    w.hit(sd);
    REQUIRE(w.ctx.spawns.size() == 1);
    CHECK(w.ctx.spawns[0].type == intern("Firefly1"));
    CHECK(w.ctx.spawns[0].loc == kP);
    CHECK(w.ctx.spawns[0].team == Team::None);
  }
  SUBCASE("with game data") {
    const GameData* data = game_data();
    if (!data) {
      WARN_MESSAGE(false, "no game install found; set ITB_GAME_DIR to run game-data tests");
      return;
    }
    World w;
    w.ctx.data = data;
    w.tile().terrain = Terrain::Water;
    w.add(vek(7, 3, kRight));
    w.hit(sd);
    REQUIRE(w.board.pawns().size() == 2);
    const Pawn* spawned = w.board.pawn_at(kP);
    REQUIRE(spawned);
    CHECK(spawned->uid == 8);
    CHECK(spawned->type == intern("Firefly1"));
    CHECK(spawned->team == Team::Enemy);
    CHECK(spawned->alive());
    w.settle();
    CHECK_FALSE(w.board.pawn_at(kP)->alive());
  }
}

TEST_CASE("v93 iTerrain HOLE opens at settle and kills the pawn") {
  World w;
  w.add(vek(1, 3));
  SpaceDamage sd = World::sd_at(0);
  sd.terrain = static_cast<int>(Terrain::Hole);
  w.hit(sd);
  CHECK(w.tile().terrain == Terrain::Road);
  CHECK(w.tile().pending_hole);
  CHECK(w.pawn(1).alive());
  w.settle();
  CHECK(w.tile().is_chasm());
  CHECK_FALSE(w.tile().pending_hole);
  CHECK_FALSE(w.pawn(1).alive());
}

TEST_CASE("v94 flyers survive a deferred chasm") {
  World w;
  Pawn p = vek(1, 3);
  p.flying = true;
  w.add(p);
  SpaceDamage sd = World::sd_at(0);
  sd.terrain = static_cast<int>(Terrain::Hole);
  w.hit(sd);
  w.settle();
  CHECK(w.tile().is_chasm());
  CHECK(w.pawn(1).alive());
  CHECK_FALSE(w.pawn(1).fallen);
}

TEST_CASE("v95 iTerrain BUILDING kills the occupant and builds") {
  World w;
  w.add(vek(1, 3));
  SpaceDamage sd = World::sd_at(0);
  sd.terrain = static_cast<int>(Terrain::Building);
  w.hit(sd);
  CHECK_FALSE(w.pawn(1).alive());
  CHECK(w.tile().is_building());
  CHECK(w.tile().hp == 1);
  CHECK(w.tile().max_hp == 1);
  CHECK(w.tile().populated);
}

TEST_CASE("v96 iTerrain BUILDING on a building adds a floor") {
  World w;
  make_building(w.tile(), 2, 2);
  SpaceDamage sd = World::sd_at(0);
  sd.terrain = static_cast<int>(Terrain::Building);
  w.hit(sd);
  CHECK(w.tile().hp == 3);
  CHECK(w.tile().max_hp == 3);
}

TEST_CASE("v97 iTerrain LAVA makes non-burning lava") {
  World w;
  w.tile().fire = FireState::Burning;
  SpaceDamage sd = World::sd_at(0);
  sd.terrain = static_cast<int>(Terrain::Lava);
  w.hit(sd);
  CHECK(w.tile().terrain == Terrain::Water);
  CHECK(w.tile().lava);
  CHECK_FALSE(w.tile().on_fire());
}

TEST_CASE("v98 spikes injure at settle") {
  World w;
  w.add(vek(1, 3));
  SpaceDamage sd = World::sd_at(0);
  sd.terrain = 15;
  w.hit(sd);
  CHECK(w.tile().spikes);
  CHECK_FALSE(w.pawn(1).injured);
  w.settle();
  CHECK(w.pawn(1).injured);
}

TEST_CASE("v99 DAMAGE_DEATH thaws a frozen mountain and flattens it") {
  World w;
  make_mountain(w.tile(), 2);
  w.tile().frozen = true;
  w.hit(kDamageDeath);
  CHECK_FALSE(w.tile().frozen);
  CHECK(w.tile().terrain == Terrain::Rubble);
}

TEST_CASE("v100 healing smoke heals before the hit lands") {
  World w;
  w.board.passives = kPassiveHealingSmoke;
  w.tile().smoke = true;
  Pawn m = mech(10, 3);
  m.hp = 2;
  m.acid = true;
  w.add(m);
  w.hit(1);
  CHECK_FALSE(w.tile().smoke);
  CHECK_FALSE(w.pawn(10).acid);
  CHECK(w.pawn(10).hp == 2);
}

// ---- Coverage beyond the vectors ---------------------------------------------------

TEST_CASE("every populated-building hit goes to the resolver, in order") {
  World w;
  make_building(w.tile(kP), 2, 2);
  make_building(w.tile(kRight), 1, 1);
  std::vector<int> amounts;
  w.ctx.grid_resist = [&](Point p, int n) {
    amounts.push_back(n);
    return p == kRight;  // the second building resists
  };
  w.hit(kDamageDeath, kP);
  w.bump(1, kRight);  // bumps roll too
  CHECK(amounts == std::vector<int>{2, 1});
  CHECK(w.board.grid_power == 5);
  CHECK(w.count(RulesEventType::GridDamaged) == 1);
  CHECK(w.count(RulesEventType::GridResisted) == 1);
  CHECK(w.tile(kRight).hp == 1);
}

TEST_CASE("freeze-events mode skips the grid") {
  World w;
  w.ctx.freeze_events = true;
  make_building(w.tile(), 2, 2);
  w.hit(1);
  CHECK(w.tile().hp == 1);
  CHECK(w.board.grid_power == 7);
}

TEST_CASE("scripts run first, even off the board") {
  World w;
  SpaceDamage sd = World::sd_at(1, kInvalidPoint);
  sd.script = "Board:AddAlert()";
  w.hit(sd);
  CHECK(w.ctx.scripts == std::vector<std::string>{"Board:AddAlert()"});

  std::vector<Point> seen;
  w.ctx.run_script = [&](Board&, const std::string& s, Point p) {
    CHECK(s == "x()");
    seen.push_back(p);
  };
  sd.script = "x()";
  sd.loc = kP;
  w.hit(sd);
  CHECK(seen == std::vector<Point>{kP});
  CHECK(w.ctx.scripts.size() == 1);
}

TEST_CASE("pushes: none on buildings, flips release webs") {
  World w;
  make_building(w.tile(), 1, 1);
  SpaceDamage sd = World::sd_at(0);
  sd.push = Dir::Up;
  w.hit(sd);
  CHECK(w.ctx.pushes.empty());

  World f;
  f.add(vek(1, 3));
  f.add(mech(10, 3, kRight));
  add_web(f.board, kP, Dir::Right);
  REQUIRE(f.pawn(10).webbed);
  sd.push = Dir::Flip;
  f.hit(sd);
  REQUIRE(f.ctx.pushes.size() == 1);
  CHECK(f.ctx.pushes[0].dir == Dir::Flip);
  CHECK_FALSE(f.pawn(10).webbed);
  CHECK(f.tile(kP).web_out == 0);
  CHECK(f.tile(kRight).web_in == 0);
}

TEST_CASE("a pawn killed by the hit is still pushed") {
  World w;
  w.add(vek(1, 1));
  SpaceDamage sd = World::sd_at(2);
  sd.push = Dir::Left;
  w.hit(sd);
  CHECK_FALSE(w.pawn(1).alive());
  REQUIRE(w.ctx.pushes.size() == 1);
  CHECK(w.ctx.pushes[0].uid == 1);
}

TEST_CASE("sPawn kills the occupant; a mech corpse blocks it") {
  SpaceDamage sd = World::sd_at(0);
  sd.spawn_pawn = intern("Spider1");
  World w;
  w.add(vek(1, 3));
  w.hit(sd);
  CHECK_FALSE(w.pawn(1).alive());
  CHECK(w.ctx.spawns.size() == 1);  // a dead Vek no longer blocks

  World c;
  Pawn corpse = mech(10, 3);
  corpse.hp = 0;
  c.add(corpse);
  c.hit(sd);
  CHECK(c.ctx.spawns.empty());
}

TEST_CASE("Armor psion protects Vek but not psions; Psion Leech extends it to mechs") {
  World w;
  w.board.psion = Leader::Armor;
  w.add(vek(1, 3));
  Pawn psion = vek(2, 2, kRight);
  psion.leader = Leader::Armor;
  w.add(psion);
  w.add(mech(10, 3, kUp));
  w.hit(2, kP);
  w.hit(1, kRight);
  w.hit(2, kUp);
  CHECK(w.pawn(1).hp == 2);
  CHECK(w.pawn(2).hp == 1);
  CHECK(w.pawn(10).hp == 1);
  w.board.passives = kPassivePsionLeech;
  w.hit(2, kUp);
  CHECK(w.pawn(10).hp == 0);  // 1 HP, 2 - 1 armor = 1
}

TEST_CASE("turn shield without a shield thaws instead of popping") {
  World w;
  w.board.passives = kPassivePlayerTurnShield;
  make_ice(w.tile(), 1);
  Pawn m = mech(10, 3);
  m.frozen = true;
  w.add(m);
  w.hit(2);
  CHECK_FALSE(w.pawn(10).frozen);
  CHECK(w.pawn(10).hp == 3);
  CHECK(w.tile().hp == 2);
}

TEST_CASE("Retaliation reaches bots and triggers on bumps") {
  World w;
  Pawn m = mech(10, 3);
  m.pilot_abilities = kPilotRetaliation;
  w.add(m);
  Pawn bot = vek(1, 2, kUp);
  bot.faction = Faction::Bots;
  w.add(bot);
  w.bump(1);
  CHECK(w.pawn(10).hp == 2);
  CHECK(w.pawn(1).hp == 1);
}

TEST_CASE("cracking triggers items and pods, and clears fire and acid") {
  World w;
  w.tile().terrain = Terrain::Forest;
  w.tile().item = intern("Item_Mine");
  w.tile().pod = PodState::Present;
  SpaceDamage sd = World::sd_at(0);
  sd.crack = true;
  w.hit(sd);
  CHECK(w.tile().item == kNoSymbol);
  CHECK(w.tile().pod == PodState::Destroyed);
  CHECK(w.tile().terrain == Terrain::Road);
  CHECK(w.tile().cracked);
}

TEST_CASE("uncrackable tiles") {
  RulesContext ctx;
  {
    Board b;
    b.tile(kP).terrain = Terrain::Water;
    CHECK_FALSE(is_crackable(b, kP));
  }
  {
    Board b;
    b.tile(kP).teleporter = true;
    CHECK_FALSE(is_crackable(b, kP));
  }
  {
    Board b;
    b.tile(kP).custom_tile = intern("conveyor1.png");
    CHECK_FALSE(is_crackable(b, kP));
  }
  {
    Board b;
    b.tile(kP).special_tag = intern("supervolcano");
    CHECK_FALSE(is_crackable(b, kP));
  }
  {
    Board b;
    Pawn rocket = vek(1, 2);
    rocket.type = intern("SatelliteRocket");
    b.add_pawn(rocket);
    CHECK_FALSE(is_crackable(b, kP));
    set_cracked(b, kP, true, ctx);
    CHECK_FALSE(b.tile(kP).cracked);
  }
  {
    Board b;
    CHECK(is_crackable(b, kP));
  }
}

TEST_CASE("settle: burning tiles set off items; burning sand and acid forest turn to road") {
  World w;
  w.tile(kP).fire = FireState::Burning;
  w.tile(kP).item = intern("Item_Mine");
  w.tile(kUp).terrain = Terrain::Sand;
  w.tile(kUp).fire = FireState::Burning;
  w.tile(kRight).terrain = Terrain::Forest;
  w.tile(kRight).acid = true;
  w.tile(kRight).pod = PodState::Present;
  w.settle();
  CHECK(w.tile(kP).item == kNoSymbol);
  CHECK(w.tile(kUp).terrain == Terrain::Road);
  CHECK(w.tile(kUp).on_fire());
  CHECK(w.tile(kRight).terrain == Terrain::Road);
  CHECK(w.tile(kRight).pod == PodState::Destroyed);
}

TEST_CASE("settle: pods are collected by the player and destroyed by others") {
  World w;
  w.tile(kP).pod = PodState::Present;
  w.tile(kRight).pod = PodState::Present;
  w.add(mech(10, 3, kP));
  w.add(vek(1, 3, kRight));
  w.settle();
  CHECK(w.tile(kP).pod == PodState::Collected);
  CHECK(w.tile(kRight).pod == PodState::Destroyed);
  CHECK(w.count(RulesEventType::PodCollected) == 1);
}

TEST_CASE("settle: burning pawns light forests, submerged ones go out, lava keeps burning") {
  World w;
  w.tile(kP).terrain = Terrain::Forest;
  Pawn burning = vek(1, 3, kP);
  burning.fire = true;
  w.add(burning);
  w.tile(kRight).terrain = Terrain::Water;
  Pawn wading = vek(2, 3, kRight);
  wading.massive = true;
  wading.fire = true;
  w.add(wading);
  make_lava(w.tile(kUp));
  Pawn hot = vek(3, 3, kUp);
  hot.massive = true;
  w.add(hot);
  w.settle();
  CHECK(w.tile(kP).fire == FireState::BurningForest);
  CHECK_FALSE(w.pawn(2).fire);
  CHECK(w.pawn(3).fire);
  CHECK(w.pawn(3).alive());
}

TEST_CASE("settle: smoke puts out any pawn standing in it, IgnoreSmoke or not") {
  World w;
  w.tile().smoke = true;
  Pawn p = vek(1, 3);
  p.ignore_smoke = true;
  p.fire = true;
  w.add(p);
  w.settle();
  CHECK_FALSE(w.pawn(1).fire);
}

TEST_CASE("settle: trains die without corpses; dams stay frozen in water") {
  World w;
  w.tile(kP).terrain = Terrain::Hole;
  Pawn train = vek(1, 3, kP);
  train.type = intern("Train_Pawn");
  train.corpse = true;
  w.add(train);
  w.tile(kRight).terrain = Terrain::Water;
  Pawn dam = vek(2, 3, kRight);
  dam.type = intern("Dam_Pawn");
  dam.massive = true;
  dam.frozen = true;
  w.add(dam);
  w.settle();
  CHECK_FALSE(w.pawn(1).alive());
  CHECK_FALSE(w.pawn(1).corpse);
  CHECK_FALSE(w.pawn(1).fallen);
  CHECK(w.pawn(2).frozen);
}

TEST_CASE("freezing water under a dam leaves water") {
  World w;
  w.tile().terrain = Terrain::Water;
  Pawn dam = vek(1, 3);
  dam.type = intern("Dam_Pawn");
  dam.massive = true;
  w.add(dam);
  SpaceDamage sd = World::sd_at(0);
  sd.frozen = StatusChange::Apply;
  w.hit(sd);
  CHECK(w.tile().terrain == Terrain::Water);
  CHECK(w.pawn(1).frozen);
}

TEST_CASE("settle: Freeze_Walk pilots freeze the water they stand in") {
  World w;
  w.tile().terrain = Terrain::Water;
  Pawn m = mech(10, 3);
  m.massive = true;
  m.pilot_abilities = kPilotFreezeWalk;
  w.add(m);
  w.settle();
  CHECK(w.tile().terrain == Terrain::Ice);
}

TEST_CASE("settle: mech corpses fall into chasms and stop counting") {
  World w;
  w.tile().terrain = Terrain::Hole;
  Pawn corpse = mech(10, 3);
  corpse.hp = 0;
  w.add(corpse);
  CHECK(has_pawn(w.board, kP));
  w.settle();
  CHECK(w.pawn(10).fallen);
  CHECK_FALSE(has_pawn(w.board, kP));
  CHECK(w.settle() == 1);  // already stable
}

TEST_CASE("iTerrain BUILDING on a mech corpse terminates") {
  World w;
  Pawn corpse = mech(10, 3);
  corpse.hp = 0;
  w.add(corpse);
  SpaceDamage sd = World::sd_at(0);
  sd.terrain = static_cast<int>(Terrain::Building);
  w.hit(sd);
  CHECK(w.tile().is_building());
}

TEST_CASE("mountains with an acid pool fall to road") {
  World w;
  make_mountain(w.tile(), 1);
  w.tile().acid = true;
  w.hit(1);
  CHECK(w.tile().terrain == Terrain::Road);
}

TEST_CASE("pseudo-terrains and terrain side effects") {
  World w;
  w.tile().pod = PodState::Present;
  w.tile().cracked = true;
  set_terrain(w.board, kP, Terrain::Water, w.ctx);
  CHECK(w.tile().pod == PodState::Destroyed);
  CHECK_FALSE(w.tile().cracked);

  set_terrain(w.board, kRight, Terrain::Fire, w.ctx);
  CHECK(w.tile(kRight).fire == FireState::Burning);
  set_terrain(w.board, kRight, Terrain::Acid, w.ctx);
  CHECK(w.tile(kRight).acid);
  CHECK_FALSE(w.tile(kRight).on_fire());
  set_terrain(w.board, kUp, 16, w.ctx);
  CHECK(w.tile(kUp).cracked);
  set_terrain(w.board, kUp, 17, w.ctx);  // cosmetic only
  CHECK(w.tile(kUp).terrain == Terrain::Road);

  // A frozen mountain sinking into water thaws.
  make_mountain(w.tile(kP), 2);
  w.tile(kP).frozen = true;
  set_terrain(w.board, kP, Terrain::Water, w.ctx);
  CHECK_FALSE(w.tile(kP).frozen);
}

TEST_CASE("settle: a lava flag only survives on water or ice") {
  World w;
  w.tile().lava = true;  // stale flag on road
  w.settle();
  CHECK_FALSE(w.tile().lava);
}

TEST_CASE("bEvacuate: Vek retreat, players stay") {
  World w;
  w.add(vek(1, 3, kP));
  w.add(mech(10, 3, kRight));
  SpaceDamage sd = World::sd_at(0);
  sd.evacuate = true;
  w.hit(sd);
  sd.loc = kRight;
  w.hit(sd);
  CHECK(w.pawn(1).minor);
  CHECK(w.pawn(1).hp == 0);
  CHECK_FALSE(w.pawn(1).dying);
  CHECK_FALSE(w.pawn(10).minor);
}

TEST_CASE("bEvacuate: an already-minor Vek dies, a bot powers down") {
  // Pawn::Retreat: the retreat mark is the minor flag (+0x10F0).
  World w;
  w.add(vek(1, 3, kP));
  w.pawn(1).minor = true;  // e.g. Lua Minor = true
  w.add(vek(2, 3, kRight));
  w.pawn(2).faction = Faction::Bots;
  SpaceDamage sd = World::sd_at(0);
  sd.evacuate = true;
  w.hit(sd);
  sd.loc = kRight;
  w.hit(sd);
  CHECK(w.pawn(1).dying);
  CHECK(w.pawn(2).minor);
  CHECK(w.pawn(2).hp == 3);
  CHECK_FALSE(w.pawn(2).movement.powered);
}

TEST_CASE("the hit tile webs the pawn on the grapple point") {
  // Lua AddGrapple(webber, target): loc = the webber's tile, grapple point =
  // the target. BoardSpace::DamageSpace D16 -> SetGrappled(dir): the loc tile
  // emits, the neighbour's pawn is held.
  World w;
  w.add(vek(1, 3, kP));
  w.add(mech(10, 3, kRight));
  SpaceDamage sd = World::sd_at(0, kP);
  sd.grapple_source = kRight;
  w.hit(sd);
  CHECK(w.pawn(10).webbed);
  // The web is tile state: kP webs its right neighbour, which holds one web.
  CHECK(w.tile(kP).web_out == 1u << static_cast<int>(Dir::Right));
  CHECK(w.tile(kRight).web_in == 1);
  CHECK(web_sources(w.board, kRight) == 1u << static_cast<int>(Dir::Left));
  CHECK_FALSE(w.pawn(1).webbed);
  // Smoking the webber's tile releases it.
  SpaceDamage smoke = World::sd_at(0);
  smoke.smoke = StatusChange::Apply;
  w.hit(smoke);
  CHECK_FALSE(w.pawn(10).webbed);
  CHECK(w.tile(kP).web_out == 0);
  CHECK(w.tile(kRight).web_in == 0);
}

TEST_CASE("webs: a pawn webbed from two tiles stays webbed until both let go") {
  // BoardSpace::SetGrappled(5/4): the held tile counts its webs; releasing
  // one emitter (ClearGrapple(1), or the web check) takes one off, and the
  // pawn is freed only when none is left.
  const Point east{5, 3};
  World w;
  w.add(vek(1, 3, kP));
  w.add(vek(2, 3, east));
  w.add(mech(10, 3, kRight));
  SpaceDamage a = World::sd_at(0, kP);
  a.grapple_source = kRight;
  w.hit(a);
  SpaceDamage b = World::sd_at(0, east);
  b.grapple_source = kRight;
  w.hit(b);
  REQUIRE(w.pawn(10).webbed);
  CHECK(w.tile(kRight).web_in == 2);
  CHECK(web_sources(w.board, kRight) == ((1u << static_cast<int>(Dir::Left)) | (1u << static_cast<int>(Dir::Right))));

  // The first webber dies: its web breaks, the other still holds.
  w.pawn(1).hp = 0;
  w.settle();
  CHECK(w.pawn(10).webbed);
  CHECK(w.tile(kP).web_out == 0);
  CHECK(w.tile(kRight).web_in == 1);

  // Smoke on the second webber's tile releases the last web.
  SpaceDamage smoke = World::sd_at(0, east);
  smoke.smoke = StatusChange::Apply;
  w.hit(smoke);
  CHECK_FALSE(w.pawn(10).webbed);
  CHECK(w.tile(east).web_out == 0);
  CHECK(w.tile(kRight).web_in == 0);
}

TEST_CASE("webs: the emitter tile changing hands breaks only its own web") {
  // The live case (2026-10-10 Mission_Train t2): one of two Scorpions webbing
  // a mech is swapped away; the tile it left now holds a mech (not
  // Pawn::IsEnemy), so that web breaks and the other keeps the mech webbed.
  const Point east{5, 3};
  World w;
  w.add(vek(1, 3, kP));
  w.add(vek(2, 3, east));
  w.add(mech(10, 3, kRight));
  w.add(mech(11, 3, Point{3, 6}));
  add_web(w.board, kP, Dir::Right);
  add_web(w.board, east, Dir::Left);
  REQUIRE(w.tile(kRight).web_in == 2);
  set_space(w.board, w.pawn(1), Point{3, 7});
  set_space(w.board, w.pawn(11), kP);
  w.settle();
  CHECK(w.pawn(10).webbed);
  CHECK(w.tile(kP).web_out == 0);
  CHECK(w.tile(kRight).web_in == 1);
  CHECK(w.tile(east).web_out == 1u << static_cast<int>(Dir::Left));
}

TEST_CASE("webs: the web stays with the held tile") {
  // BoardSpace::OnLoop: while a tile holds a web its first occupant is
  // webbed once it is not busy, so a pawn that takes the held pawn's place
  // (here: within the same frame, before the web check) is caught.
  World w;
  w.add(vek(1, 3, kP));
  w.add(mech(10, 3, kRight));
  w.add(mech(11, 3, Point{6, 6}));
  add_web(w.board, kP, Dir::Right);
  REQUIRE(w.pawn(10).webbed);
  set_space(w.board, w.pawn(10), Point{5, 5});
  set_space(w.board, w.pawn(11), kRight);
  CHECK_FALSE(w.pawn(10).webbed);
  w.settle();
  CHECK_FALSE(w.pawn(10).webbed);
  CHECK(w.pawn(11).webbed);
  CHECK(w.tile(kRight).web_in == 1);

  // Left empty, the held tile breaks the web at the emitter's next check.
  set_space(w.board, w.pawn(11), Point{6, 6});
  w.settle();
  CHECK_FALSE(w.pawn(11).webbed);
  CHECK(w.tile(kP).web_out == 0);
  CHECK(w.tile(kRight).web_in == 0);
}

TEST_CASE("webs: a multi-tile pawn is webbed through its extra tile") {
  // The live case (2026-10-10 Mission_Train t3): a Scorpion webs the train's
  // second tile; the train (listed on that tile) is webbed and stays so while
  // its main tile is hit.
  World w;
  Pawn train = mech(10, 1, kP);
  train.mech = false;
  train.team = Team::Player;
  train.pushable = false;
  train.extra_dx = 0;
  train.extra_dy = 1;
  w.add(train);
  w.add(vek(1, 3, Point{3, 5}));
  add_web(w.board, Point{3, 5}, Dir::Up);
  CHECK(w.pawn(10).webbed);
  CHECK(w.tile(Point{3, 4}).web_in == 1);
  w.settle();
  CHECK(w.pawn(10).webbed);
}

TEST_CASE("a web breaks when its emitter tile loses its Vek") {
  World w;
  w.add(vek(1, 3, kP));
  w.add(mech(10, 3, kRight));
  SpaceDamage sd = World::sd_at(0, kP);
  sd.grapple_source = kRight;
  w.hit(sd);
  REQUIRE(w.pawn(10).webbed);
  // BoardSpace::OnLoop: no living pawn on the emitting tile -> released.
  w.pawn(1).hp = 0;
  w.settle();
  CHECK_FALSE(w.pawn(10).webbed);
}

TEST_CASE("webs: a Disable_Immunity pilot (Camila Vera) shakes off a web at once; others stay held") {
  // Pawn::SetGrappled sets the flag even for her (the game only swaps the
  // voice line for Mech_WebBlocked); BoardSpace::OnLoop's web check then
  // breaks the web (IsGrappleImmunity: Disable_Immunity, not busy), so after
  // the settle she is free and can move. A second web on another mech holds.
  const Point down{3, 4};
  World w;
  w.add(vek(1, 3, kP));
  w.add(mech(10, 3, kRight));
  w.add(mech(11, 3, down));
  w.pawn(10).pilot_abilities = kPilotDisableImmunity;
  w.pawn(10).move = 3;
  w.pawn(10).movement.powered = true;
  SpaceDamage a = World::sd_at(0, kP);
  a.grapple_source = kRight;
  w.hit(a);
  SpaceDamage b = World::sd_at(0, kP);
  b.grapple_source = down;
  w.hit(b);
  CHECK(w.pawn(10).webbed);  // the flag, for the rest of this frame
  CHECK(w.pawn(11).webbed);
  w.settle();
  CHECK_FALSE(w.pawn(10).webbed);
  CHECK(w.tile(kRight).web_in == 0);
  CHECK(w.tile(kP).web_out == 1u << static_cast<int>(Dir::Down));
  CHECK(can_move(w.pawn(10)));
  CHECK(w.pawn(11).webbed);
  CHECK(w.tile(down).web_in == 1);
  CHECK_FALSE(can_move(w.pawn(11)));

  // A web left on a tile does not catch her when she steps onto it either:
  // the plain mech leaves its held tile and she takes it, all in one frame.
  set_space(w.board, w.pawn(11), Point{6, 6});
  set_space(w.board, w.pawn(10), down);
  w.settle();
  CHECK_FALSE(w.pawn(10).webbed);
  CHECK(w.tile(kP).web_out == 0);
  CHECK(w.tile(down).web_in == 0);
}

TEST_CASE("smoke still puts out a Disable_Immunity pilot's fire") {
  // BoardSpace::OnLoop puts out every pawn on a smoked tile (its own check,
  // not Pawn::IsSmoked), so the pilot's immunity does not keep her burning.
  World w;
  Pawn m = mech(10, 3);
  m.fire = true;
  m.pilot_abilities = kPilotDisableImmunity;
  w.add(m);
  SpaceDamage sd = World::sd_at(0);
  sd.smoke = StatusChange::Apply;
  w.hit(sd);
  w.settle();
  CHECK(w.tile().smoke);
  CHECK_FALSE(w.pawn(10).fire);
}

TEST_CASE("sItem places an item; DAMAGE_ZERO reads as 0") {
  World w;
  SpaceDamage sd = World::sd_at(kDamageZero);
  sd.item = intern("Freeze_Mine");
  w.tile().terrain = Terrain::Forest;
  w.hit(sd);
  CHECK(w.tile().item == intern("Freeze_Mine"));
  CHECK(w.tile().terrain == Terrain::Forest);
  CHECK(effective_damage(sd) == 0);
}

TEST_CASE("item damage table") {
  CHECK(item_damage(intern("Item_Mine")).damage == kDamageDeath);
  CHECK(item_damage(intern("Item_Repair_Mine")).damage == -10);
  CHECK(item_damage(intern("Freeze_Mine")).frozen == StatusChange::Apply);
  CHECK(item_damage(intern("Supply_Drop")).damage == 0);
}

TEST_CASE("SpiderlingEgg1 spawns inactive") {
  const GameData* data = game_data();
  if (!data) {
    WARN_MESSAGE(false, "no game install found; set ITB_GAME_DIR to run game-data tests");
    return;
  }
  World w;
  w.ctx.data = data;
  w.board.next_uid = 40;
  SpaceDamage sd = World::sd_at(0);
  sd.spawn_pawn = intern("SpiderlingEgg1");
  sd.spawn_team = Team::Enemy;
  w.hit(sd);
  const Pawn* egg = w.board.pawn_at(kP);
  REQUIRE(egg);
  CHECK(egg->uid == 40);
  CHECK_FALSE(egg->active);
  CHECK(w.count(RulesEventType::PawnSpawned) == 1);
}

// ---- Explosion mode (SpaceDamage mode override 2) --------------------------------
// Pawn::DetonateCorpse builds its hits with the mode override set to 2: the
// tile treats them as weapon hits, pawns get mode 2.

namespace {

SpaceDamage explosion(int damage, Point p = kP) {
  SpaceDamage sd = World::sd_at(damage, p);
  sd.mode_override = kModeExplosion;
  return sd;
}

}  // namespace

TEST_CASE("explosion mode skips armor and ACID arithmetic") {
  World w;
  Pawn p = vek(1, 4);
  p.armor = true;
  p.acid = true;
  p.infected = true;
  w.add(p);
  w.hit(explosion(1));
  CHECK(w.pawn(1).hp == 3);  // a weapon hit would deal 2 (ACID doubles, armor is off)
  CHECK_FALSE(w.pawn(1).infected);
  Pawn a = vek(2, 3, kRight);
  a.armor = true;
  w.add(a);
  w.hit(explosion(1, kRight));
  CHECK(w.pawn(2).hp == 2);  // armor would have absorbed it
}

TEST_CASE("explosion mode gets no Force Amp bonus") {
  World w;
  w.board.passives = kPassiveForceAmp;
  w.add(vek(1, 3));
  w.hit(explosion(1));
  CHECK(w.pawn(1).hp == 2);
}

TEST_CASE("explosion mode is not a bump, even through the push-mode entry point") {
  for (bool via_push : {false, true}) {
    CAPTURE(via_push);
    World w;
    auto apply = [&](SpaceDamage sd) {
      if (via_push) {
        w.bump(sd);
      } else {
        w.hit(sd);
      }
    };
    make_ice(w.tile(kP));
    apply(explosion(1));
    CHECK(w.tile(kP).hp == 1);  // ice cracks

    w.tile(kUp).terrain = Terrain::Forest;
    apply(explosion(1, kUp));
    CHECK(w.tile(kUp).terrain == Terrain::Road);  // the forest ignites
    CHECK(w.tile(kUp).on_fire());

    w.tile(kRight).item = intern("Item_Mine");
    w.tile(kRight).pod = PodState::Present;
    apply(explosion(1, kRight));
    CHECK(w.tile(kRight).item == kNoSymbol);  // the mine goes off
    CHECK(w.tile(kRight).pod == PodState::Destroyed);
  }
}

TEST_CASE("explosion mode on a cracked tile: collapses, the burrower doesn't dive") {
  World w;
  w.tile().cracked = true;
  Pawn p = vek(1, 3);
  p.burrows = true;
  w.add(p);
  w.hit(explosion(1));
  CHECK(w.tile().terrain == Terrain::Hole);
  CHECK(w.ctx.burrow_dives.empty());
}

TEST_CASE("explosion mode still pops shields and hits buildings") {
  World w;
  Pawn p = vek(1, 3);
  p.shield = true;
  w.add(p);
  w.hit(explosion(1));
  CHECK_FALSE(w.pawn(1).shield);
  CHECK(w.pawn(1).hp == 3);
  make_building(w.tile(kRight), 1, 1);
  w.board.grid_power = 5;
  w.hit(explosion(1, kRight));
  CHECK(w.tile(kRight).terrain == Terrain::Rubble);
  CHECK(w.board.grid_power == 4);
}
