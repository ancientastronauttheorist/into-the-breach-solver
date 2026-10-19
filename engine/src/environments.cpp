// Stage 7: native environments and mission combat hooks (environment.hpp).
//
// Each class mirrors one shipped Lua environment (scripts/environments.lua,
// scripts/missions/**, scripts/advanced/missions/**) and rebuilds the
// instance state the Lua keeps (Locations, Index, WindDir, Belts, ...) from
// what the bridge recorded. The effects are built with the same entries and
// delays the Lua builds; purely cosmetic entries (sounds, voices, weather and
// shake scripts, emitters) are left out because they change nothing on the
// board and take no time. Bounces are left out too, although a bouncing tile
// is busy for 0.5 s (Resolver::bounce): here that only stretches the busy
// time after the last row of a flood or strike.

#include <algorithm>
#include <map>
#include <set>
#include <string>

#include "itb/environment.hpp"
#include "itb/game_data.hpp"
#include "itb/movement.hpp"
#include "rules_detail.hpp"

namespace itb {
namespace {

constexpr int kEnvOwner = -10;  // ENV_EFFECT

std::vector<Point> danger_points(const MissionData& m) {
  std::vector<Point> out;
  for (const DangerTile& d : m.danger) {
    if (d.p.valid() && std::find(out.begin(), out.end(), d.p) == out.end()) out.push_back(d.p);
  }
  return out;
}

bool contains(const std::vector<Point>& v, Point p) { return std::find(v.begin(), v.end(), p) != v.end(); }

bool adjacent(Point a, Point b) { return std::abs(a.x - b.x) + std::abs(a.y - b.y) == 1; }

// Environment:GetQuarters order: 3x3 blocks at (1,1), (1,4), (4,1), (4,4).
int quarter(Point p) { return (p.x >= 4 ? 2 : 0) + (p.y >= 4 ? 1 : 0); }

// An environment's attack: the scripts set iOwner = ENV_EFFECT.
SkillEffect env_effect() {
  SkillEffect fx;
  fx.owner = kEnvOwner;
  return fx;
}

// A mission hook's effect (UpdateMission's SkillEffect()): Board:AddEffect
// keeps the script's iOwner, here the default -1. Board::ApplyEffect leaves
// a recorded shooter in place for an owner of -1, so a hook that fires
// inside a shot (the dam flood) credits its kills to that shooter.
SkillEffect hook_effect() { return SkillEffect{}; }

// Board:IsPawnSpace(p): a living pawn or a corpse.
bool pawn_space(const Board& b, Point p) { return p.valid() && has_pawn(b, p); }

// The i-th permutation (Lehmer code) of `items`.
std::vector<Point> permutation(std::vector<Point> items, int index) {
  std::vector<Point> out;
  int n = static_cast<int>(items.size());
  std::vector<int> fact(static_cast<size_t>(n + 1), 1);
  for (int i = 1; i <= n; ++i) fact[static_cast<size_t>(i)] = fact[static_cast<size_t>(i - 1)] * i;
  for (int i = n; i > 0; --i) {
    const int f = fact[static_cast<size_t>(i - 1)];
    const int k = index / f;
    index %= f;
    out.push_back(items[static_cast<size_t>(k)]);
    items.erase(items.begin() + k);
  }
  return out;
}

int factorial(int n) { return n <= 1 ? 1 : n * factorial(n - 1); }

// A strike order the recording does not pin down. Only strikes on occupied
// tiles can interact (a death effect, explosion or psion loss reaching
// another strike), so `candidates` that strike the occupied tiles in the same
// relative order are one branch; the rest is a chance node (EnvOrder).
std::vector<Point> choose_order(EnvHost& host, std::vector<std::vector<Point>> candidates) {
  if (candidates.empty()) return {};
  std::vector<std::vector<Point>> branches;
  std::vector<std::vector<Point>> keys;
  for (const auto& c : candidates) {
    std::vector<Point> key;
    for (Point p : c) {
      if (pawn_space(host.board(), p)) key.push_back(p);
    }
    if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
      keys.push_back(key);
      branches.push_back(c);
    }
  }
  const int pick = host.choose(ChanceKind::EnvOrder, static_cast<int>(branches.size()),
                               branches.front().empty() ? kInvalidPoint : branches.front().front());
  return branches[static_cast<size_t>(pick)];
}

// Orders a set of tiles into a chain of neighbours; empty if they are not one.
std::vector<Point> chain_order(const std::vector<Point>& tiles) {
  if (tiles.empty()) return {};
  for (Point start : tiles) {
    std::vector<Point> path{start};
    std::vector<Point> rest;
    for (Point p : tiles) {
      if (p != start) rest.push_back(p);
    }
    while (!rest.empty()) {
      auto it = std::find_if(rest.begin(), rest.end(), [&](Point p) { return adjacent(p, path.back()); });
      if (it == rest.end()) break;
      path.push_back(*it);
      rest.erase(it);
    }
    if (rest.empty()) return path;
  }
  return {};
}

// Env_Seismic: how well a marked chain read first-to-last continues the
// path: its first tile next to the chasm the previous turn left (2 per
// chasm neighbour off the chain), or at the board edge on the first turn (1).
int seismic_start_score(const Board& b, const std::vector<Point>& c) {
  const Point first = c.front();
  int s = 0;
  for (int d = 0; d < 4; ++d) {
    const Point q = step(first, static_cast<Dir>(d));
    if (q.valid() && !contains(c, q) && b.tile(q).is_chasm()) s += 2;
  }
  if (first.x == 0 || first.x == 7 || first.y == 0 || first.y == 7) s += 1;
  return s;
}

// ---- Env_Null and friends ---------------------------------------------------------

class NullEnv : public Environment {
 public:
  explicit NullEnv(std::string name) : name_(std::move(name)) {}
  std::string name() const override { return name_; }

 private:
  std::string name_;
};

// A mission with environment tiles but no native implementation.
class UnsupportedEnv : public Environment {
 public:
  explicit UnsupportedEnv(const MissionData& m) : m_(m) {}
  std::string name() const override { return "unsupported:" + m_.mission_id; }
  void begin(EnvHost& host) override {
    host.note(PhaseEventType::EnvUnsupported,
              m_.mission_id + ": " + std::to_string(m_.danger.size() + m_.freeze.size()) +
                  " environment tiles (" + m_.env_type + ") with no native environment");
  }

 private:
  MissionData m_;
};

// ---- Env_Attack (Airstrike, Lightning, Seismic, SnowStorm, Volcano, Final) --------

// Env_Attack:ApplyEffect: one location per step (Ordered: Locations[1]; not
// ordered: random_removal), or all of them at once when Instant.
class AttackEnv : public Environment {
 public:
  bool is_effect(EnvHost&) override { return !locations_.empty(); }
  bool apply(EnvHost& host) override {
    if (locations_.empty()) return false;
    if (instant_) {
      SkillEffect fx = env_effect();
      for (Point p : locations_) attack(host, p, fx);
      host.add_effect(std::move(fx));
      locations_.clear();
      return false;
    }
    if (!ordered_) {
      if (!decided_) {
        decide(host);
        decided_ = true;
      }
    }
    const Point p = locations_.front();
    locations_.erase(locations_.begin());
    SkillEffect fx = env_effect();
    attack(host, p, fx);
    host.add_effect(std::move(fx));
    return !locations_.empty();
  }

 protected:
  // GetAttackEffect(location, effect): appends this location's entries.
  virtual void attack(EnvHost& host, Point p, SkillEffect& fx) = 0;
  // Fixes the order of a non-ordered list (its random_removal draws).
  virtual void decide(EnvHost& host) {
    std::vector<Point> occupied, empty;
    for (Point p : locations_) (pawn_space(host.board(), p) ? occupied : empty).push_back(p);
    const int n = factorial(static_cast<int>(occupied.size()));
    const int pick = host.choose(ChanceKind::EnvOrder, n, occupied.empty() ? kInvalidPoint : occupied.front());
    std::vector<Point> order = permutation(occupied, pick);
    order.insert(order.end(), empty.begin(), empty.end());
    locations_ = order;
  }

  std::vector<Point> locations_;
  bool ordered_ = false;
  bool instant_ = false;
  bool decided_ = false;
};

// Env_Airstrike (missions/grass/mission_airstrike.lua): the bomber's five
// tiles, DAMAGE_DEATH, once the plane has passed.
class AirstrikeEnv final : public AttackEnv {
 public:
  explicit AirstrikeEnv(const MissionData& m) {
    const std::vector<Point> marks = danger_points(m);
    for (Point c : marks) {
      bool all = true;
      for (int d = 0; d < 4; ++d) all = all && contains(marks, step(c, static_cast<Dir>(d)));
      if (all) locations_.push_back(c);
    }
    unmatched_ = !marks.empty() && locations_.empty();
  }
  std::string name() const override { return "Env_Airstrike"; }
  void begin(EnvHost& host) override {
    if (unmatched_) host.note(PhaseEventType::EnvInexact, "air strike marks without a 5-tile cross");
  }

 protected:
  void attack(EnvHost&, Point c, SkillEffect& fx) override {
    fx.add_delay(0.75f);
    // SkillEffect:AddAirstrike: the plane (waits for projectiles), then the
    // delay until it is over the target column.
    SpaceDamage plane;
    plane.loc = Point{0, c.y};
    plane.projectile = ProjectileKind::AirStrike;
    plane.art = intern("units/mission/bomber_1.png");
    plane.delay = kProjDelay;
    fx.effect.push_back(plane);
    fx.add_delay(static_cast<float>(c.x) * 0.125f + 0.375f);
    SpaceDamage hit = space_damage(c, kDamageDeath);
    hit.animation = intern("ExploArt2");
    for (Point p : {c, step(c, Dir::Up), step(c, Dir::Right), step(c, Dir::Down), step(c, Dir::Left)}) {
      hit.loc = p;
      fx.add_damage(hit);
    }
  }

 private:
  bool unmatched_ = false;
};

// Env_Lightning (missions/sand/mission_lightning.lua): four strikes in an
// order the game draws at random.
class LightningEnv final : public AttackEnv {
 public:
  explicit LightningEnv(const MissionData& m) { locations_ = danger_points(m); }
  std::string name() const override { return "Env_Lightning"; }

 protected:
  void attack(EnvHost&, Point p, SkillEffect& fx) override {
    fx.add_delay(1.0f);
    SpaceDamage hit = space_damage(p, kDamageDeath);
    hit.animation = intern("LightningBolt0");
    fx.add_damage(hit);
  }
};

// Env_Seismic (missions/sand/mission_crack.lua): the next three tiles of a
// path across the board become chasms, in path order. The bridge reports the
// tiles, not the order: the chain's direction comes from the chasm the
// previous turn left at one end (or the board edge on the first turn).
class SeismicEnv final : public AttackEnv {
 public:
  explicit SeismicEnv(const MissionData& m) {
    ordered_ = true;
    known_ = !m.ordered_locations.empty();
    locations_ = known_ ? m.ordered_locations : danger_points(m);
  }
  std::string name() const override { return "Env_Seismic"; }
  void begin(EnvHost& host) override {
    if (known_ || locations_.size() < 2) return;
    const std::vector<Point> chain = chain_order(locations_);
    if (chain.empty()) {
      host.note(PhaseEventType::EnvInexact, "seismic tiles are not one path: scan order used");
      return;
    }
    std::vector<Point> rev(chain.rbegin(), chain.rend());
    // The first tile continues the previous turn's chasm, or starts at an edge.
    const int a = seismic_start_score(host.board(), chain), b = seismic_start_score(host.board(), rev);
    if (a != b) {
      locations_ = a > b ? chain : rev;
      return;
    }
    locations_ = choose_order(host, {chain, rev});
  }

 protected:
  void attack(EnvHost&, Point p, SkillEffect& fx) override {
    fx.add_delay(0.5f);
    SpaceDamage sd = space_damage(p);
    sd.terrain = static_cast<int>(Terrain::Hole);
    fx.add_damage(sd);
  }

 private:
  bool known_ = false;
};

// Env_SnowStorm (missions/snow/mission_snowstorm.lua): Instant, the 3x3 block
// freezes in one frame.
class SnowStormEnv final : public AttackEnv {
 public:
  explicit SnowStormEnv(const MissionData& m) {
    instant_ = true;
    locations_ = m.freeze;
    // Old bridges without environment_freeze reported the block as danger.
    if (locations_.empty() && m.env_type == "snow") locations_ = danger_points(m);
  }
  std::string name() const override { return "Env_SnowStorm"; }
  bool apply(EnvHost& host) override {
    // Env_SnowStorm:ApplyEffect: weather script, sound, AddDelay(1), then one
    // SpaceDamage(0) with iFrozen = 1 per tile.
    if (locations_.empty()) return false;
    SkillEffect fx = env_effect();
    fx.add_delay(1.0f);
    for (Point p : locations_) attack(host, p, fx);
    host.add_effect(std::move(fx));
    locations_.clear();
    return false;
  }

 protected:
  void attack(EnvHost&, Point p, SkillEffect& fx) override {
    SpaceDamage sd = space_damage(p, 0);
    sd.frozen = StatusChange::Apply;
    fx.add_damage(sd);
  }
};

// Env_Volcano (missions/final/env_volcano.lua, Mission_Final): Ordered;
// rocks are DAMAGE_DEATH + fire artillery from the super-volcano, lava turns
// the tile into lava. Mission_Final:UpdateMission keeps the super-volcano
// whole.
//
// Mission_Final:NextTurn, once, at the start of the enemy turn that follows
// the deployment (turn count 0, enemy team; state 2, before the spawns and
// the AI's planning): after a 4.5 s pause, every tile of the map's "pylons"
// zone gets a building, in random order, 0.5 s apart. Each drop is a
// SpaceDamage(tile, 0) with iTerrain = TERRAIN_BUILDING applied twice (two
// AddDropper), which makes a populated 2-HP building. When the hook finds a
// pawn (or corpse) on a pylon tile, that drop and every later one in its
// draw order carry DAMAGE_DEATH (the script never resets it). The deploy UI
// refuses pylon tiles and the mission blocks spawns there for good, so in
// practice nothing stands on them; when something does, the zone order
// stands in for the random one (EnvInexact).
class VolcanoEnv final : public AttackEnv {
 public:
  explicit VolcanoEnv(const MissionData& m) {
    ordered_ = true;
    if (auto it = m.zones.find("pylons"); it != m.zones.end()) pylons_ = it->second;
    zones_known_ = !m.zones.empty();
    if (m.volcano && m.volcano->complete) {
      lava_ = m.volcano->mode == 2;
      locations_ = m.volcano->locations;
      return;
    }
    // Old bridges: the mode from the pattern (a lava path runs right/down
    // from (2,1) or (1,2); rocks take one tile per quarter).
    std::vector<Point> tiles = danger_points(m);
    std::vector<Point> chain = chain_order(tiles);
    if (!chain.empty() && (chain.back() == Point{2, 1} || chain.back() == Point{1, 2})) {
      std::reverse(chain.begin(), chain.end());
    }
    const bool path = !chain.empty() && (chain.front() == Point{2, 1} || chain.front() == Point{1, 2}) &&
                      std::adjacent_find(chain.begin(), chain.end(), [](Point a, Point b) {
                        const Point d = b - a;
                        return !((d.x == 1 && d.y == 0) || (d.x == 0 && d.y == 1));
                      }) == chain.end();
    if (path && tiles.size() > 1) {
      lava_ = true;
      locations_ = chain;
    } else {
      ambiguous_ = tiles.size() == 1;
      std::stable_sort(tiles.begin(), tiles.end(), [](Point a, Point b) { return quarter(a) < quarter(b); });
      locations_ = tiles;
    }
  }
  std::string name() const override { return "Env_Volcano"; }
  void begin(EnvHost& host) override {
    if (ambiguous_) host.note(PhaseEventType::EnvInexact, "volcano mode unknown for a single marked tile (rocks used)");
  }
  void next_turn(EnvHost& host) override {
    if (host.turn() != 0) return;
    if (pylons_.empty()) {
      if (!zones_known_) {
        host.note(PhaseEventType::EnvInexact, "Mission_Final: no pylons zone recorded (the turn-0 building drop is left out)");
      }
      return;
    }
    const Board& b = host.board();
    SkillEffect fx = hook_effect();
    fx.add_delay(4.5f);
    SpaceDamage drop = space_damage(Point{0, 0}, 0);
    drop.terrain = static_cast<int>(Terrain::Building);
    bool occupied = false;
    for (Point p : pylons_) {
      drop.loc = p;
      if (pawn_space(b, p)) {
        drop.damage = kDamageDeath;
        occupied = true;
      }
      fx.add_dropper(drop, "combat/tiles_grass/building_fall.png");
      fx.add_dropper(drop, "combat/tiles_grass/building_fall.png");
      fx.add_delay(0.5f);
    }
    if (occupied) {
      host.note(PhaseEventType::EnvInexact, "Mission_Final: a pawn on a pylon tile (the drop order is random; zone order used)");
    }
    host.add_effect(std::move(fx));
    host.note(PhaseEventType::MissionHook, "the pylons drop", kInvalidPoint, -1, static_cast<int>(pylons_.size()));
  }
  bool has_update() const override { return true; }
  void update(EnvHost& host) override {
    // Mission_Final:UpdateMission: damaged super-volcano tiles become
    // mountains again; empty ones lose ice and shields.
    Board& b = host.board();
    for (Point v : {Point{0, 0}, Point{1, 0}, Point{0, 1}, Point{1, 1}}) {
      Tile& t = b.tile(v);
      if (t.is_mountain() && t.hp < t.max_hp) set_terrain(b, v, Terrain::Mountain, host.rules());
      if (!pawn_space(b, v)) {
        t.frozen = false;
        t.shield = false;
      }
    }
  }

 protected:
  void attack(EnvHost&, Point p, SkillEffect& fx) override {
    if (!lava_) {
      SpaceDamage rock = space_damage(p, kDamageDeath);
      rock.fire = StatusChange::Apply;
      rock.animation = intern("explo_fire1");
      rock.projectile = ProjectileKind::Artillery;
      rock.art = intern("effects/shotup_fireball.png");
      rock.projectile_source = Point{0, 0};  // effect.piOrigin
      rock.delay = kProjDelay;
      fx.origin = Point{0, 0};
      fx.effect.push_back(rock);
    } else {
      SpaceDamage lava = space_damage(p, 0);
      lava.terrain = static_cast<int>(Terrain::Lava);
      fx.add_damage(lava);
    }
  }

 private:
  bool lava_ = false;
  bool ambiguous_ = false;
  std::vector<Point> pylons_;  // zone "pylons" (fixed by the map)
  bool zones_known_ = false;
};

// Env_Final (missions/final/env_final.lua, Mission_Final_Cave): Ordered,
// phases 1-4 (rocks / mech tiles / instant rocks / instant crossing path).
// Rocks drop (AddDropper) DAMAGE_DEATH and leave ground; tentacles are
// DAMAGE_DEATH and leave lava. ApplyStart only animates the cave background.
// Mission_Final_Cave:UpdateMission drops a new bomb at a random tile when the
// bomb is gone.
class FinalCaveEnv final : public AttackEnv {
 public:
  explicit FinalCaveEnv(const MissionData& m) {
    ordered_ = true;
    if (m.final_cave && m.final_cave->complete) {
      lava_ = m.final_cave->mode == 2;
      instant_ = m.final_cave->instant;
      locations_ = m.final_cave->locations;
      phase_ = m.final_cave->phase;
      return;
    }
    locations_ = danger_points(m);
    inferred_ = !locations_.empty();
  }
  std::string name() const override { return "Env_Final"; }
  void bind(const Board& b) override {
    for (const Pawn& p : b.pawns()) {
      if (detail::type_is(p, "BigBomb")) had_bomb_ = true;
    }
  }
  void begin(EnvHost& host) override {
    if (!inferred_) return;
    // The phase advances once per turn from turn 1: rocks, mech tiles
    // (tentacles), instant rocks, instant tentacle path.
    phase_ = ((std::max(1, host.turn()) - 1) % 4) + 1;
    lava_ = phase_ == 2 || phase_ == 4;
    instant_ = phase_ >= 3;
    if (phase_ == 1) {
      std::stable_sort(locations_.begin(), locations_.end(),
                       [](Point a, Point b) { return quarter(a) < quarter(b); });
    } else if (phase_ == 2 && locations_.size() > 1) {
      // Board:GetPawns(TEAM_MECH) order at plan time: the mechs have moved
      // since, so the order is a chance node over the occupied tiles.
      std::vector<std::vector<Point>> orders;
      const int n = factorial(static_cast<int>(locations_.size()));
      for (int i = 0; i < n; ++i) orders.push_back(permutation(locations_, i));
      locations_ = choose_order(host, orders);
    }
    host.note(PhaseEventType::EnvInexact,
              "final cave phase " + std::to_string(phase_) + " inferred from the turn (no mission_final_cave)");
  }
  bool has_update() const override { return true; }
  void update(EnvHost& host) override {
    // Mission_Final_Cave:UpdateMission, on an idle board only. (The Lua
    // tests IsBusy first; asking it only when a bomb is due is the same and
    // tells the enemy phase which frames the hook waited for.)
    Board& b = host.board();
    for (const Pawn& p : b.pawns()) {
      if (p.team == Team::Player && detail::type_is(p, "BigBomb") && (p.alive() || is_corpse(b, p))) return;
    }
    if (!had_bomb_) return;  // no bomb at all: not a recorded final-cave board
    if (host.busy()) return;
    // AddBomb: a random tile without a player pawn, building or environment
    // danger, preferring the inner 4x4.
    std::vector<Point> choices, inner;
    for (int x = 0; x < kBoardSize; ++x) {
      for (int y = 0; y < kBoardSize; ++y) {
        const Point p{x, y};
        const Pawn* q = pawn_space(b, p) ? board_pawn(b, p) : nullptr;
        if ((q && q->team == Team::Player) || b.tile(p).is_building() || contains(locations_, p)) continue;
        choices.push_back(p);
        if (x > 1 && x < 6 && y > 1 && y < 6) inner.push_back(p);
      }
    }
    const std::vector<Point>& pool = inner.empty() ? choices : inner;
    const Point at = pool.empty() ? Point{4, 4}
                                  : pool[static_cast<size_t>(
                                        host.choose(ChanceKind::MissionRandom, static_cast<int>(pool.size())))];
    SkillEffect fx = hook_effect();
    SpaceDamage drop = space_damage(at, 0);
    drop.spawn_pawn = intern("BigBomb");
    drop.terrain = static_cast<int>(Terrain::Road);
    fx.add_dropper(drop, "units/mission/bomb.png");
    host.add_effect(std::move(fx));
    b.total_turns += 2;  // TurnLimit += 2
    host.note(PhaseEventType::MissionHook, "bomb destroyed: a new bomb drops", at);
  }
  bool end_blocked(EnvHost&) override { return true; }

 protected:
  void attack(EnvHost&, Point p, SkillEffect& fx) override {
    SpaceDamage sd = space_damage(p, kDamageDeath);
    if (!lava_) {
      sd.terrain = static_cast<int>(Terrain::Road);
      fx.add_dropper(sd, "effects/shotdown_rock.png");
    } else {
      sd.animation = intern("tentacles");
      sd.terrain = static_cast<int>(Terrain::Lava);
      fx.add_damage(sd);
    }
  }

 private:
  bool had_bomb_ = false;
  bool lava_ = false;
  bool inferred_ = false;
  int phase_ = 0;
};

// ---- Row and column environments -----------------------------------------------

// Env_Cataclysm (missions/sand/mission_cataclysm.lua): column 7 - Index
// collapses top to bottom, 0.2 s per tile, buildings excepted (tested when
// the step runs). The column comes from the marks; with no mark nothing in
// it can change.
class CataclysmEnv final : public Environment {
 public:
  explicit CataclysmEnv(const MissionData& m) {
    for (Point p : danger_points(m)) {
      if (column_ < 0) column_ = p.x;
      if (p.x != column_) mixed_ = true;
    }
  }
  std::string name() const override { return "Env_Cataclysm"; }
  void begin(EnvHost& host) override {
    if (mixed_) host.note(PhaseEventType::EnvInexact, "cataclysm marks span several columns");
  }
  bool is_effect(EnvHost&) override { return true; }
  bool apply(EnvHost& host) override {
    if (column_ < 0) return false;
    SkillEffect fx = env_effect();
    for (int y = 0; y < kBoardSize; ++y) {
      const Point p{column_, y};
      if (host.board().tile(p).is_building()) continue;
      SpaceDamage sd = space_damage(p);
      sd.terrain = static_cast<int>(Terrain::Hole);
      sd.delay = 0.2f;
      fx.add_damage(sd);
    }
    host.add_effect(std::move(fx));
    return false;
  }

 private:
  int column_ = -1;
  bool mixed_ = false;
};

// Env_Tides (missions/grass/mission_tides.lua) and Env_Terratide
// (advanced/missions/sand/mission_terratide.lua). Rows 0..Index are swept;
// row Index floods (Tides: water, DAMAGE_DEATH on mountains first, buildings
// shadow their column) or smokes (Terratide, from the bottom, no shadow).
class TidesEnv final : public Environment {
 public:
  TidesEnv(const MissionData& m, bool sand) : sand_(sand) {
    if (m.tides_index) {
      index_ = *m.tides_index;
      return;
    }
    for (Point p : danger_points(m)) {
      const int i = sand_ ? 7 - p.y : p.y;
      if (index_ < 0) index_ = i;
      if (i != index_) mixed_ = true;
    }
  }
  std::string name() const override { return sand_ ? "Env_Terratide" : "Env_Tides"; }
  void begin(EnvHost& host) override {
    if (mixed_) host.note(PhaseEventType::EnvInexact, "tide marks span several rows");
  }
  bool is_effect(EnvHost&) override { return true; }
  bool apply(EnvHost& host) override {
    // No marks and no recorded Index: no tile of the flooded row can change.
    if (index_ < 0) return false;
    Board& b = host.board();
    SkillEffect fx = env_effect();
    std::map<int, int> building;  // column -> row of the shadowing building
    for (int i = 0; i <= index_ && i < kBoardSize; ++i) {
      const int y = sand_ ? 7 - i : i;
      for (int x = 0; x < kBoardSize; ++x) {
        const Point p{x, y};
        if (!sand_ && b.tile(p).is_building()) {
          building[x] = y;
          continue;
        }
        if (auto it = building.find(x); it != building.end() && it->second < y) continue;
        SpaceDamage sd = space_damage(p);
        if (i == index_) {
          if (!sand_) {
            sd.terrain = static_cast<int>(Terrain::Water);
            if (b.tile(p).is_mountain()) sd.damage = kDamageDeath;
          } else {
            sd.smoke = StatusChange::Apply;
          }
        }
        fx.add_damage(sd);
      }
      fx.add_delay(0.2f);
    }
    host.add_effect(std::move(fx));
    return false;
  }

 private:
  bool sand_ = false;
  int index_ = -1;
  bool mixed_ = false;
};

// Env_RandomWind (advanced/missions/sand/mission_wind.lua, the AE
// Mission_Wind): two marked columns push one way, rows from the downwind
// edge; after each row 0.2 s if either of its two tiles held a pawn when the
// effect was built.
class WindEnv final : public Environment {
 public:
  explicit WindEnv(const MissionData& m) : dir_(m.wind_dir) {
    std::map<int, int> rows;
    for (Point p : danger_points(m)) ++rows[p.x];
    for (const auto& [x, n] : rows) {
      if (n == kBoardSize) columns_.push_back(x);
    }
  }
  std::string name() const override { return "Env_RandomWind"; }
  void begin(EnvHost& host) override {
    if (!columns_.empty() && columns_.size() != 2) {
      host.note(PhaseEventType::EnvInexact, std::to_string(columns_.size()) + " wind columns marked");
    }
    if (!dir_ && !columns_.empty()) {
      // WindDir is visible in game (the arrows) but old bridges did not
      // record it: a chance node, Down first.
      const int pick = host.choose(ChanceKind::EnvChoice, 2);
      dir_ = pick == 0 ? Dir::Down : Dir::Up;
      host.note(PhaseEventType::EnvInexact, "wind direction not recorded");
    }
  }
  bool is_effect(EnvHost&) override { return true; }
  bool apply(EnvHost& host) override {
    if (columns_.empty() || !dir_) return false;
    Board& b = host.board();
    SkillEffect fx = env_effect();
    SpaceDamage sd;
    sd.push = *dir_;
    sd.animation = intern("windpush_" + std::to_string(static_cast<int>(*dir_)));
    int y = *dir_ == Dir::Up ? 0 : 7;
    for (int row = 0; row < kBoardSize; ++row) {
      bool occupied = false;
      for (size_t i = 0; i < columns_.size(); ++i) {
        const Point p{columns_[i], y};
        occupied = occupied || pawn_space(b, p);
        sd.loc = p;
        sd.delay = (i + 1 == columns_.size() && occupied) ? 0.2f : 0.0f;
        fx.add_damage(sd);
      }
      y -= dir_vector(*dir_).y;
    }
    host.add_effect(std::move(fx));
    return false;
  }

 private:
  std::optional<Dir> dir_;
  std::vector<int> columns_;
};

// Env_Belt (missions/acid/mission_belt.lua): every conveyor pushes its tile,
// in the Belts list order (downstream end of each chain first), with 0.2 s
// after each belt that held a pawn when the effect was built. CheckBelts
// drops belts on chasm, water or cracked tiles first. The list comes from the
// bridge's env_instance when recorded (exact order); otherwise it is rebuilt
// from the tiles' conveyor directions.
class BeltEnv final : public Environment {
 public:
  explicit BeltEnv(const MissionData& m) : mission_(m.mission_id), known_(m.belts_known), list_(m.belts) {}
  std::string name() const override { return mission_ == "Mission_Belt" ? "Env_BeltLine" : "Env_BeltRandom"; }
  void begin(EnvHost& host) override {
    if (known_) {
      for (const auto& [p, d] : list_) {
        belts_.push_back(p);
        dirs_.push_back(d);
      }
      return;
    }
    const Board& b = host.board();
    std::vector<Point> belts;
    for (int i = 0; i < kTileCount; ++i) {
      const Point p = Point::from_index(i);
      if (b.tile(p).conveyor >= 0 && b.tile(p).conveyor < 4) belts.push_back(p);
    }
    // Chains: a belt feeds the belt it points at. The Belts list holds each
    // chain from its downstream end backwards; chains in quarter order.
    auto dir_of = [&](Point p) { return static_cast<Dir>(b.tile(p).conveyor); };
    std::vector<std::vector<Point>> chains;
    std::set<int> used;
    for (Point p : belts) {
      const Point next = step(p, dir_of(p));
      if (contains(belts, next)) continue;  // not a downstream end
      std::vector<Point> chain{p};
      used.insert(p.index());
      for (bool grew = true; grew;) {
        grew = false;
        for (Point q : belts) {
          if (!used.count(q.index()) && step(q, dir_of(q)) == chain.back()) {
            chain.push_back(q);
            used.insert(q.index());
            grew = true;
            break;
          }
        }
      }
      chains.push_back(chain);
    }
    bool cyclic = false;
    for (Point p : belts) {
      if (!used.count(p.index())) {
        cyclic = true;
        chains.push_back({p});
      }
    }
    std::stable_sort(chains.begin(), chains.end(), [](const auto& a, const auto& c) {
      auto key = [](const std::vector<Point>& ch) {
        int k = 4;
        for (Point p : ch) k = std::min(k, quarter(p));
        return k;
      };
      return key(a) < key(c);
    });
    // Chains that feed into one another depend on the list order.
    bool feeding = false;
    for (const auto& ch : chains) {
      const Point out = step(ch.front(), dir_of(ch.front()));
      for (const auto& other : chains) {
        if (&other != &ch && (contains(other, out))) feeding = true;
      }
    }
    for (const auto& ch : chains) belts_.insert(belts_.end(), ch.begin(), ch.end());
    for (Point p : belts_) dirs_.push_back(dir_of(p));
    if (cyclic || feeding) host.note(PhaseEventType::EnvInexact, "conveyor list order not recorded");
  }
  bool is_effect(EnvHost&) override { return true; }
  bool apply(EnvHost& host) override {
    Board& b = host.board();
    SkillEffect fx = env_effect();
    for (size_t i = 0; i < belts_.size(); ++i) {
      const Point p = belts_[i];
      const Tile& t = b.tile(p);
      if (t.is_chasm() || t.is_liquid() || t.cracked) {
        host.note(PhaseEventType::EnvInexact, "a conveyor broke this turn (CheckBelts list quirk)", p);
        continue;
      }
      SpaceDamage sd = space_damage(p, 0, dirs_[i]);
      sd.animation = intern("Conveyor_" + std::to_string(static_cast<int>(dirs_[i])));
      fx.add_damage(sd);
      if (pawn_space(b, p)) fx.add_delay(0.2f);
    }
    host.add_effect(std::move(fx));
    return false;
  }

 private:
  std::string mission_;
  bool known_ = false;
  std::vector<std::pair<Point, Dir>> list_;
  std::vector<Point> belts_;
  std::vector<Dir> dirs_;
};

// ---- Missions without an environment step but with combat hooks -------------------

int32_t find_type(const Board& b, std::string_view type, bool alive_only = false) {
  for (const Pawn& p : b.pawns()) {
    if (detail::type_is(p, type) && (!alive_only || p.alive())) return p.uid;
  }
  return -1;
}

bool pawn_alive(const Board& b, int32_t uid) {
  const Pawn* p = b.find_pawn(uid);
  return p && p->alive() && !p->fallen;
}

// Board:GetPawns(team): living pawns, corpses and mechs (spec S6 3B).
bool listed(const Board& b, const Pawn& p) { return p.alive() || is_corpse(b, p) || p.mech; }

// Mission_Dam (missions/grass/mission_dam.lua): the frame after the dam dies,
// the two columns below it flood, one row every 0.3 s.
class DamMission final : public Environment {
 public:
  std::string name() const override { return "Mission_Dam"; }
  void bind(const Board& b) override {
    dam_ = find_type(b, "Dam_Pawn");
    if (const Pawn* d = b.find_pawn(dam_)) {
      pos_ = d->pos;
      // A dam that died before (Flooded) left water below it (ice once
      // frozen); one that died without its hook running (an action run
      // without ActionOptions::mission) floods now.
      const Point below = pos_ + Point{0, 1};
      const Tile* t = below.valid() ? &b.tile(below) : nullptr;
      flooded_ = !d->alive() && t && (t->is_liquid() || t->terrain == Terrain::Ice);
    }
  }
  bool has_update() const override { return true; }
  void update(EnvHost& host) override {
    if (flooded_ || dam_ < 0 || pawn_alive(host.board(), dam_)) return;
    // Queued while the killing shot is still active, so the Vek it drowns
    // are that shooter's kills (pilot XP).
    SkillEffect fx = hook_effect();
    for (int y = 1; y <= 7; ++y) {
      for (int x = 0; x <= 1; ++x) {
        SpaceDamage sd = space_damage(pos_ + Point{x, y});
        sd.terrain = static_cast<int>(Terrain::Water);
        if (sd.loc.valid()) fx.add_damage(sd);
      }
      fx.add_delay(0.3f);
    }
    flooded_ = true;
    host.add_effect(std::move(fx));
    host.note(PhaseEventType::MissionHook, "dam destroyed: the river floods", pos_);
  }
  bool end_blocked(EnvHost& host) override { return pawn_alive(host.board(), dam_); }

 private:
  int32_t dam_ = -1;
  Point pos_ = kInvalidPoint;
  bool flooded_ = true;
};

// Mission_Train / Mission_Armored_Train (missions/mission_train.lua,
// advanced/missions/grass/mission_armored_train.lua). The train queues its
// move up every turn (bridges before attack_order_all did not export team-1
// queued shots, so it is inferred for them), fires after every Vek, and once
// dead is replaced by its wreck at the tile it last stood on.
class TrainMission final : public Environment {
 public:
  TrainMission(bool armored, bool queued_known) : armored_(armored), queued_known_(queued_known) {}
  std::string name() const override { return armored_ ? "Mission_Armored_Train" : "Mission_Train"; }
  void bind(const Board& b) override {
    train_ = find_type(b, armored_ ? "Train_Armored" : "Train_Pawn");
    stopped_ = train_ < 0;
    if (const Pawn* t = b.find_pawn(train_)) loc_ = t->pos;
  }
  void begin(EnvHost& host) override {
    Pawn* t = host.board().find_pawn(train_);
    if (t && !queued_known_ && t->alive() && !t->queued.active() && t->weapons[0] != kNoSymbol && t->pos.valid()) {
      t->queued = QueuedShot{0, t->pos, step(t->pos, Dir::Up)};
    }
  }
  bool has_update() const override { return true; }
  void update(EnvHost& host) override {
    if (stopped_) return;
    Board& b = host.board();
    if (!pawn_alive(b, train_)) {
      // Mission_Train:StopTrain.
      b.remove_pawn(train_);
      const PawnDef* def = host.data().pawn(armored_ ? "Train_Armored_Damaged" : "Train_Damaged");
      if (def && loc_.valid()) {
        Pawn wreck = host.data().make_pawn(*def, host.new_uid(), loc_);
        wreck.active = false;
        Pawn& added = b.add_pawn(wreck);
        b.stamp_arrival(added);
      }
      stopped_ = true;
      host.note(PhaseEventType::MissionHook, "train destroyed: replaced by its wreck", loc_);
      return;
    }
    if (const Pawn* t = b.find_pawn(train_)) loc_ = t->pos;
  }

 private:
  bool armored_ = false;
  bool queued_known_ = false;
  int32_t train_ = -1;
  Point loc_ = kInvalidPoint;
  bool stopped_ = true;
};

// Mission_Satellite (missions/grass/mission_satellites.lua): a rocket with a
// queued launch fires it (Rocket_Launch: DAMAGE_DEATH around it, then it flies
// away) after every Vek. NextTurn powers rocket 1 on turn 1, rocket 2 on 3.
class SatelliteMission final : public Environment {
 public:
  explicit SatelliteMission(const MissionData& m) : launching_(m.launching) {}
  std::string name() const override { return "Mission_Satellite"; }
  void begin(EnvHost& host) override {
    for (int32_t uid : launching_) {
      Pawn* p = host.board().find_pawn(uid);
      if (p && p->alive() && !p->queued.active() && p->pos.valid()) p->queued = QueuedShot{0, p->pos, p->pos};
    }
  }
  void next_turn(EnvHost& host) override {
    std::vector<Pawn*> rockets;
    for (Pawn& p : host.board().pawns()) {
      if (detail::type_is(p, "SatelliteRocket")) rockets.push_back(&p);
    }
    std::sort(rockets.begin(), rockets.end(), [](const Pawn* a, const Pawn* b) { return a->uid < b->uid; });
    const int turn = host.turn();
    const size_t i = turn == 1 ? 0 : turn == 3 ? 1 : 2;
    if (i < rockets.size() && rockets[i]->alive()) rockets[i]->movement.powered = true;
  }

 private:
  std::vector<int32_t> launching_;
};

// Mission_Volatile (missions/mission_volatile.lua): the Volatile Vek retreats
// when it is the last enemy (unless the mission spawns forever).
class VolatileMission final : public Environment {
 public:
  explicit VolatileMission(const MissionData& m)
      : infinite_(m.infinite_spawn.value_or(false)), infinite_known_(m.infinite_spawn.has_value()) {}
  std::string name() const override { return "Mission_Volatile"; }
  void bind(const Board& b) override { target_ = find_type(b, "GlowingScorpion"); }
  void begin(EnvHost& host) override {
    if (!infinite_known_) host.note(PhaseEventType::EnvInexact, "Mission_Volatile: InfiniteSpawn not recorded");
  }
  bool has_update() const override { return true; }
  void update(EnvHost& host) override {
    Board& b = host.board();
    if (target_ < 0 || left_) return;
    if (!pawn_alive(b, target_)) return;
    // Board:GetEnemyCount() = GetPawnCount(TEAM_ENEMY).
    int alive = 0, frozen = 0;
    for (const Pawn& p : b.pawns()) {
      if (p.team != Team::Enemy || !p.alive() || detail::type_is(p, "BonusDebris")) continue;
      ++alive;
      if (p.frozen) ++frozen;
    }
    const int count = alive == 0 ? 0 : std::max(1, static_cast<int>(alive - frozen / 3.0));
    if (count != 1 || infinite_) return;
    if (Pawn* t = b.find_pawn(target_)) {
      retreat_pawn(b, *t, host.rules());
      left_ = true;
      host.note(PhaseEventType::MissionHook, "the Volatile Vek retreats", t->pos, t->uid);
    }
  }

 private:
  bool infinite_ = false;
  bool infinite_known_ = false;
  int32_t target_ = -1;
  bool left_ = false;
};

// AE Mission_AcidStorm: while the Storm Generator lives, every pawn is set
// ACID every frame (a shield or the Thick pilot blocks it).
class AcidStormMission final : public Environment {
 public:
  std::string name() const override { return "Mission_AcidStorm"; }
  void bind(const Board& b) override { gen_ = find_type(b, "Storm_Generator"); }
  bool has_update() const override { return true; }
  void update(EnvHost& host) override {
    Board& b = host.board();
    if (!pawn_alive(b, gen_)) return;
    for (Pawn& p : b.pawns()) {
      if (listed(b, p) && !p.acid) set_pawn_acid(b, p, true);
    }
  }

 private:
  int32_t gen_ = -1;
};

// AE Mission_Shields: while the generator lives, every pawn that appears gets
// a shield once; when it dies every pawn (but Zoltan pilots) and building
// loses its shield at once. Who already got a shield (ShieldedUnits) is not
// recorded: every pawn on the recorded board is taken to have had it.
class ShieldsMission final : public Environment {
 public:
  std::string name() const override { return "Mission_Shields"; }
  void bind(const Board& b) override {
    gen_ = find_type(b, "Shield_Building");
    for (const Pawn& p : b.pawns()) shielded_.insert(p.uid);
  }
  bool has_update() const override { return true; }
  void update(EnvHost& host) override {
    Board& b = host.board();
    if (gen_ < 0) return;
    if (pawn_alive(b, gen_)) {
      for (Pawn& p : b.pawns()) {
        if (!listed(b, p) || shielded_.count(p.uid)) continue;
        shielded_.insert(p.uid);
        set_pawn_shield(b, p, true);
      }
      return;
    }
    for (Pawn& p : b.pawns()) {
      if (listed(b, p) && !p.has_pilot(kPilotZoltan)) set_pawn_shield(b, p, false);
    }
    for (int i = 0; i < kTileCount; ++i) {
      Tile& t = b.tile(Point::from_index(i));
      if (t.is_building()) t.shield = false;
    }
    gen_ = -1;
    host.note(PhaseEventType::MissionHook, "shield generator destroyed: shields drop");
  }

 private:
  int32_t gen_ = -1;
  std::set<int32_t> shielded_;
};

// AE Mission_Hacking: when the hacked building dies, the stored Cannon Bot is
// replaced by a player-team Snowtank1_Player with its shield state.
class HackingMission final : public Environment {
 public:
  explicit HackingMission(const MissionData& m) : bot_(m.hacking_bot), hack_(m.hacking_building) {}
  std::string name() const override { return "Mission_Hacking"; }
  void bind(const Board& b) override {
    if (hack_ < 0) hack_ = find_type(b, "Hacked_Building");
    if (bot_ < 0) {
      // Old bridges: the bot is the mission's shielded Snowtank1.
      std::vector<int32_t> tanks;
      for (const Pawn& p : b.pawns()) {
        if (detail::type_is(p, "Snowtank1") && p.team != Team::Player && p.alive()) tanks.push_back(p.uid);
      }
      if (tanks.size() == 1) {
        bot_ = tanks[0];
      } else if (!tanks.empty()) {
        for (int32_t uid : tanks) {
          if (b.find_pawn(uid)->shield) bot_ = uid;
        }
        if (bot_ < 0) bot_ = tanks[0];
        bot_inferred_ = true;
      }
    }
  }
  void begin(EnvHost& host) override {
    if (bot_inferred_) host.note(PhaseEventType::EnvInexact, "Mission_Hacking: bot id not recorded, several Cannon Bots");
  }
  bool has_update() const override { return true; }
  void update(EnvHost& host) override {
    Board& b = host.board();
    if (hack_ < 0 || pawn_alive(b, hack_)) return;
    Pawn* bot = b.find_pawn(bot_);
    if (!bot || !bot->alive() || bot->team == Team::Player) return;
    const Point loc = bot->pos;
    const bool shielded = bot->shield;
    b.remove_pawn(bot_);
    const PawnDef* def = host.data().pawn("Snowtank1_Player");
    if (!def) return;
    Pawn tank = host.data().make_pawn(*def, host.new_uid(), loc);
    tank.shield = shielded;
    Pawn& added = b.add_pawn(tank);
    b.stamp_arrival(added);
    bot_ = added.uid;
    host.note(PhaseEventType::MissionHook, "facility destroyed: the Cannon Bot joins the player", loc, added.uid);
  }

 private:
  int32_t bot_ = -1;
  int32_t hack_ = -1;
  bool bot_inferred_ = false;
};

// Mission_Reactivation (missions/snow/mission_reactivation.lua). At mission
// start the mission keeps the uids of the units it placed frozen in its
// Enemies list. Its NextTurn with the enemy team draws entries uniformly at
// random, removing each from the list, until two have been thawed or the list
// is empty; a drawn unit is thawed only if it is alive and frozen, otherwise
// it is dropped. So the mission never thaws a unit it has already drawn (one
// the player re-froze stays frozen), nor one it did not place.
//
// The engine works from what a player can know, never from the mission's own
// list: the units frozen when the mission starts, minus the ones it has been
// seen to thaw (live_play.py keeps that list and hands it over as
// "reactivation_visible_list"). Which unfrozen or dead entries the draws
// silently discarded is hidden, so they stay listed (worst case: the mission
// may still thaw them if they are frozen again).
//
// The list lives on the board (Pawn::reactivation_listed) so that it carries
// across simulated turns. With E the listed units that are alive and frozen:
// |E| <= 2 thaws all of E; |E| > 2 thaws a uniformly random pair of E (chance
// node MissionRandom over the pairs). Every listed unit is drawn when |E| < 2,
// so the list empties. Each Thawed event carries |E| (amount) and E's uids
// (detail, "candidates u1,u2,..."), from which live_play tells which units
// the game's draw thawed.
//
// Without a visible list (old recordings) every frozen enemy is a candidate.
class ReactivationMission final : public Environment {
 public:
  explicit ReactivationMission(const MissionData& m) : known_(m.reactivation_known) {}
  std::string name() const override { return "Mission_Reactivation"; }
  void next_turn(EnvHost& host) override {
    Board& b = host.board();
    std::vector<int32_t> frozen;
    for (const Pawn& p : b.pawns()) {
      const bool candidate = known_ ? p.reactivation_listed : p.team == Team::Enemy;
      if (candidate && p.alive() && p.frozen) frozen.push_back(p.uid);
    }
    std::vector<int32_t> thaw = frozen;
    if (frozen.size() > 2) {
      // random_removal over the list: any pair of candidates, uniformly.
      std::vector<std::pair<int32_t, int32_t>> pairs;
      for (size_t i = 0; i < frozen.size(); ++i) {
        for (size_t j = i + 1; j < frozen.size(); ++j) pairs.emplace_back(frozen[i], frozen[j]);
      }
      const auto [a, c] = pairs[static_cast<size_t>(
          host.choose(ChanceKind::MissionRandom, static_cast<int>(pairs.size())))];
      thaw = {a, c};
    }
    if (known_ && frozen.size() < 2) {
      for (Pawn& p : b.pawns()) p.reactivation_listed = false;  // the whole list was drawn
    }
    std::string candidates = "candidates ";
    for (size_t i = 0; i < frozen.size(); ++i) candidates += (i ? "," : "") + std::to_string(frozen[i]);
    for (int32_t uid : thaw) {
      if (Pawn* p = b.find_pawn(uid)) {
        set_pawn_frozen(b, *p, false);
        p->reactivation_listed = false;
        host.note(PhaseEventType::Thawed, candidates, p->pos, uid, static_cast<int>(frozen.size()));
      }
    }
  }

 private:
  bool known_ = false;
};

}  // namespace

std::unique_ptr<Environment> make_native_environment(const MissionData& m) {
  const std::string& id = m.mission_id;
  if (id == "Mission_Airstrike") return std::make_unique<AirstrikeEnv>(m);
  if (id == "Mission_Lightning" || id == "Mission_LightningStorm") return std::make_unique<LightningEnv>(m);
  if (id == "Mission_Crack") return std::make_unique<SeismicEnv>(m);
  if (id == "Mission_Cataclysm") return std::make_unique<CataclysmEnv>(m);
  if (id == "Mission_Tides") return std::make_unique<TidesEnv>(m, false);
  if (id == "Mission_Terratide") return std::make_unique<TidesEnv>(m, true);
  if (id == "Mission_SnowStorm") return std::make_unique<SnowStormEnv>(m);
  if (id == "Mission_Wind") return std::make_unique<WindEnv>(m);
  if (id == "Mission_Belt" || id == "Mission_BeltRandom") return std::make_unique<BeltEnv>(m);
  if (id == "Mission_Final") return std::make_unique<VolcanoEnv>(m);
  if (id == "Mission_Final_Cave") return std::make_unique<FinalCaveEnv>(m);
  if (id == "Mission_Dam") return std::make_unique<DamMission>();
  if (id == "Mission_Train") return std::make_unique<TrainMission>(false, m.all_queued_known);
  if (id == "Mission_Armored_Train") return std::make_unique<TrainMission>(true, m.all_queued_known);
  if (id == "Mission_Satellite") return std::make_unique<SatelliteMission>(m);
  if (id == "Mission_Volatile") return std::make_unique<VolatileMission>(m);
  if (id == "Mission_AcidStorm") return std::make_unique<AcidStormMission>();
  if (id == "Mission_Shields") return std::make_unique<ShieldsMission>();
  if (id == "Mission_Hacking") return std::make_unique<HackingMission>(m);
  if (id == "Mission_Reactivation") return std::make_unique<ReactivationMission>(m);
  if (!m.danger.empty() || !m.freeze.empty()) return std::make_unique<UnsupportedEnv>(m);
  return std::make_unique<NullEnv>(id.empty() ? "Env_Null" : id);
}

bool native_environment_has_update(const MissionData& m) {
  const std::string& id = m.mission_id;
  return id == "Mission_Final" || id == "Mission_Final_Cave" || id == "Mission_Dam" || id == "Mission_Train" ||
         id == "Mission_Armored_Train" || id == "Mission_Volatile" || id == "Mission_AcidStorm" ||
         id == "Mission_Shields" || id == "Mission_Hacking";
}

// ---- Next-turn planning (environment.hpp plan_next_environment) -------------------
//
// Each planner mirrors the environment's Plan() / SelectSpaces() (Env_Attack
// moves the selected tiles into Locations; the others advance an index).
// Board queries are made on the board at planning time (the end of this
// turn's enemy phase, after the spawns), as the game makes them.

namespace {

// The sample's own stream (splitmix64): a function of the seed alone.
class PlanRng {
 public:
  explicit PlanRng(uint64_t seed) : s_(seed ^ 0x6a09e667f3bcc909ULL) {}
  uint64_t next() {
    uint64_t z = (s_ += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
  }
  // Uniform in [0, n); 0 for n <= 1.
  int below(int n) { return n <= 1 ? 0 : static_cast<int>(next() % static_cast<uint64_t>(n)); }
  template <class T>
  const T& pick(const std::vector<T>& v) {
    return v[static_cast<size_t>(below(static_cast<int>(v.size())))];
  }

 private:
  uint64_t s_;
};

// Environment:IsValidTarget: on the board, no pod, no building, no water
// (lava is water) unless the environment targets water.
bool valid_env_target(const Board& b, Point p, bool water) {
  if (!p.valid()) return false;
  const Tile& t = b.tile(p);
  return t.pod != PodState::Present && !t.is_building() && (water || !t.is_liquid());
}

std::vector<DangerTile> danger_of(const std::vector<Point>& tiles) {
  std::vector<DangerTile> out;
  for (Point p : tiles) out.push_back(DangerTile{p, kDamageDeath, true, -1});
  return out;
}

std::vector<Point> airstrike_cross(Point c) {
  return {c, step(c, Dir::Up), step(c, Dir::Right), step(c, Dir::Down), step(c, Dir::Left)};
}

// Env_Airstrike:SelectSpaces: a centre in x 3..5, y 1..6 whose five tiles
// are no building and no pod (water allowed), uniformly at random.
void plan_airstrike(const Board& b, PlanRng& rng, EnvPlan& out) {
  std::vector<Point> choices;
  for (int x = 3; x <= 5; ++x) {
    for (int y = 1; y <= 6; ++y) {
      bool ok = true;
      for (Point p : airstrike_cross(Point{x, y})) {
        ok = ok && p.valid() && b.tile(p).pod != PodState::Present && !b.tile(p).is_building();
      }
      if (ok) choices.push_back(Point{x, y});
    }
  }
  out.sampled = choices.size() > 1;
  if (!choices.empty()) out.hazard = airstrike_cross(rng.pick(choices));
}

// Env_Lightning:SelectSpaces: one valid tile per quarter (GetQuarters),
// uniformly at random.
void plan_lightning(const Board& b, PlanRng& rng, EnvPlan& out) {
  for (Point start : {Point{1, 1}, Point{1, 4}, Point{4, 1}, Point{4, 4}}) {
    std::vector<Point> choices;
    for (int x = start.x; x <= start.x + 2; ++x) {
      for (int y = start.y; y <= start.y + 2; ++y) {
        if (valid_env_target(b, Point{x, y}, false)) choices.push_back(Point{x, y});
      }
    }
    if (choices.empty()) continue;
    out.sampled = out.sampled || choices.size() > 1;
    out.hazard.push_back(rng.pick(choices));
  }
}

int manhattan(Point a, Point b) { return std::abs(a.x - b.x) + std::abs(a.y - b.y); }

// Env_Seismic: the next three tiles of the path GetCrossPath built at
// mission start. The path itself is hidden; the player sees the marked
// tiles. GetCrossPath walks from one edge tile to another (endpoints in
// 2..5 on opposite edges), each step to a neighbour closest to the far end
// (Manhattan, +0.5 on rows 0 and 7 off the end; ties at random). So the far
// end is sampled among the edge endpoints every marked step got closer to,
// and the walk continued from the last marked tile.
void plan_seismic(const Board& before, const Board& after, const MissionData& m, PlanRng& rng, EnvPlan& out) {
  std::vector<Point> chain = m.ordered_locations;
  if (chain.empty()) {
    const std::vector<Point> tiles = danger_points(m);
    chain = tiles.size() < 2 ? tiles : chain_order(tiles);
    if (chain.size() >= 2) {
      // This turn's order, as SeismicEnv reads it (ties: either way).
      std::vector<Point> rev(chain.rbegin(), chain.rend());
      const int a = seismic_start_score(before, chain), c = seismic_start_score(before, rev);
      if (c > a || (c == a && rng.below(2) == 1)) chain = rev;
      out.sampled = out.sampled || a == c;
    }
  }
  if (chain.empty()) return;  // nothing marked: the path is over (or unknown)
  // The tile before the chain: the previous turn's chasm next to its first
  // tile, when there is exactly one.
  std::vector<Point> known = chain;
  {
    std::vector<Point> prev;
    for (int d = 0; d < 4; ++d) {
      const Point q = step(chain.front(), static_cast<Dir>(d));
      if (q.valid() && !contains(chain, q) && before.tile(q).is_chasm()) prev.push_back(q);
    }
    if (prev.size() == 1) known.insert(known.begin(), prev.front());
  }
  std::vector<Point> ends;
  for (int i = 2; i <= 5; ++i) {
    for (Point e : {Point{i, 0}, Point{i, 7}, Point{0, i}, Point{7, i}}) {
      if (!valid_env_target(after, e, true) && !contains(chain, e)) continue;
      bool closer = true;
      for (size_t k = 1; k < known.size(); ++k) closer = closer && manhattan(known[k], e) == manhattan(known[k - 1], e) - 1;
      if (closer) ends.push_back(e);
    }
  }
  out.sampled = true;
  if (ends.empty()) return;
  const Point fin = rng.pick(ends);
  std::vector<Point> path = chain;
  Point cur = chain.back();
  while (cur != fin && out.hazard.size() < 3) {
    std::vector<Point> best;
    double closest = 1e9;
    for (int d = 0; d < 4; ++d) {
      const Point n = step(cur, static_cast<Dir>(d));
      if (!valid_env_target(after, n, true) || contains(path, n)) continue;
      double dist = manhattan(n, fin);
      if ((n.y == 7 || n.y == 0) && n != fin) dist += 0.5;
      if (dist < closest) {
        best = {n};
        closest = dist;
      } else if (dist == closest) {
        best.push_back(n);
      }
    }
    if (best.empty()) break;  // a dead end (the Lua backtracks; rare)
    cur = rng.pick(best);
    path.push_back(cur);
    out.hazard.push_back(cur);
  }
  out.mission.ordered_locations = out.hazard;
}

// Env_Tides / Env_Terratide: Plan advances Index; the next step floods
// (smokes) row Index + 1. The hazard: the row's tiles the step changes
// (Tides: buildings, and the tiles a building shadows, are spared).
void plan_tides(const Board& b, const MissionData& m, bool sand, EnvPlan& out) {
  int index = m.tides_index.value_or(-1);
  if (index < 0) {
    for (Point p : danger_points(m)) index = std::max(index, sand ? 7 - p.y : p.y);
  }
  if (index < 0) return;  // unknown
  ++index;
  out.mission.tides_index = index;
  if (index >= kBoardSize) return;
  const int y = sand ? 7 - index : index;
  for (int x = 0; x < kBoardSize; ++x) {
    bool shadowed = false;
    for (int i = 0; i <= index && !sand; ++i) shadowed = shadowed || b.tile(Point{x, i}).is_building();
    if (!shadowed) out.hazard.push_back(Point{x, y});
  }
}

// Env_Cataclysm: Plan advances Index; the next column (one to the left)
// collapses, buildings and chasms excepted (as MarkBoard marks it).
void plan_cataclysm(const Board& b, const MissionData& m, EnvPlan& out) {
  int column = -1;
  for (Point p : danger_points(m)) column = column < 0 ? p.x : std::min(column, p.x);
  if (column <= 0) return;  // unknown, or the last column collapsed
  for (int y = 0; y < kBoardSize; ++y) {
    const Point p{column - 1, y};
    if (!b.tile(p).is_chasm() && !b.tile(p).is_building()) out.hazard.push_back(p);
  }
}

// Env_RandomWind:Plan: UP or DOWN at random; two distinct columns in 1..5,
// neither of them this turn's (LastIndices).
void plan_wind(const MissionData& m, PlanRng& rng, EnvPlan& out) {
  std::map<int, int> rows;
  for (Point p : danger_points(m)) ++rows[p.x];
  std::vector<int> last, choices;
  for (const auto& [x, n] : rows) {
    if (n == kBoardSize) last.push_back(x);
  }
  for (int x = 1; x <= 5; ++x) {
    if (std::find(last.begin(), last.end(), x) == last.end()) choices.push_back(x);
  }
  out.sampled = true;
  out.mission.wind_dir = rng.below(2) == 0 ? Dir::Up : Dir::Down;
  for (int i = 0; i < 2 && !choices.empty(); ++i) {
    const int k = rng.below(static_cast<int>(choices.size()));
    for (int y = 0; y < kBoardSize; ++y) out.hazard.push_back(Point{choices[static_cast<size_t>(k)], y});
    choices.erase(choices.begin() + k);
  }
}

// Env_SnowStorm:SelectSpaces: a centre drawn from (3,2), (3,5), (2,3),
// (5,3) without repeat until all four were used (the remaining ones are
// hidden: any centre but this turn's), and its 3x3 block.
void plan_snowstorm(const MissionData& m, PlanRng& rng, EnvPlan& out) {
  std::vector<Point> centres;
  for (Point c : {Point{3, 2}, Point{3, 5}, Point{2, 3}, Point{5, 3}}) {
    if (!contains(m.freeze, c) || m.freeze.empty()) centres.push_back(c);
  }
  // The block of a centre covers the other centres only partly, so this
  // turn's centre is the one whose whole block is marked.
  std::vector<Point> rest;
  for (Point c : centres) {
    bool whole = !m.freeze.empty();
    for (int dx = -1; dx <= 1; ++dx) {
      for (int dy = -1; dy <= 1; ++dy) whole = whole && contains(m.freeze, Point{c.x + dx, c.y + dy});
    }
    if (!whole) rest.push_back(c);
  }
  if (rest.empty()) rest = centres;
  out.sampled = true;
  const Point c = rng.pick(rest);
  for (int dx = -1; dx <= 1; ++dx) {
    for (int dy = -1; dy <= 1; ++dy) out.hazard.push_back(Point{c.x + dx, c.y + dy});
  }
  out.mission.freeze = out.hazard;
}

}  // namespace

bool environment_plannable(const MissionData& m) {
  const std::string& id = m.mission_id;
  return id == "Mission_Airstrike" || id == "Mission_Lightning" || id == "Mission_LightningStorm" ||
         id == "Mission_Crack" || id == "Mission_Cataclysm" || id == "Mission_Tides" || id == "Mission_Terratide" ||
         id == "Mission_Wind" || id == "Mission_SnowStorm";
}

EnvPlan plan_next_environment(const Board& before, const Board& after, const MissionData& m, uint64_t seed) {
  EnvPlan out;
  out.mission = m;
  if (!environment_plannable(m)) return out;
  out.planned = true;
  PlanRng rng(seed);
  const std::string& id = m.mission_id;
  if (id == "Mission_Airstrike") {
    plan_airstrike(after, rng, out);
  } else if (id == "Mission_Lightning" || id == "Mission_LightningStorm") {
    plan_lightning(after, rng, out);
  } else if (id == "Mission_Crack") {
    out.mission.ordered_locations.clear();
    plan_seismic(before, after, m, rng, out);
  } else if (id == "Mission_Cataclysm") {
    plan_cataclysm(after, m, out);
  } else if (id == "Mission_Tides" || id == "Mission_Terratide") {
    plan_tides(after, m, id == "Mission_Terratide", out);
  } else if (id == "Mission_Wind") {
    plan_wind(m, rng, out);
  } else if (id == "Mission_SnowStorm") {
    plan_snowstorm(m, rng, out);
  }
  // What the bridge would record: the marked tiles (the ice storm's block
  // goes to environment_freeze instead).
  out.mission.danger = id == "Mission_SnowStorm" ? std::vector<DangerTile>{} : danger_of(out.hazard);
  return out;
}

}  // namespace itb
