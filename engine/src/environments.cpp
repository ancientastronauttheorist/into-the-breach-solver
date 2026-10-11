// Stage 7: native environments and mission combat hooks (environment.hpp).
//
// Each class mirrors one shipped Lua environment (scripts/environments.lua,
// scripts/missions/**, scripts/advanced/missions/**) and rebuilds the
// instance state the Lua keeps (Locations, Index, WindDir, Belts, ...) from
// what the bridge recorded. The effects are built with the same entries and
// delays the Lua builds; purely cosmetic entries (sounds, voices, weather and
// shake scripts, bounces, emitters) are left out because they change nothing
// on the board and take no time.

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

SkillEffect env_effect() {
  SkillEffect fx;
  fx.owner = kEnvOwner;
  return fx;
}

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
    auto score = [&](const std::vector<Point>& c) {
      // The first tile continues the previous turn's chasm, or starts at an edge.
      const Point first = c.front();
      int s = 0;
      for (int d = 0; d < 4; ++d) {
        const Point q = step(first, static_cast<Dir>(d));
        if (q.valid() && !contains(c, q) && host.board().tile(q).is_chasm()) s += 2;
      }
      if (first.x == 0 || first.x == 7 || first.y == 0 || first.y == 7) s += 1;
      return s;
    };
    const int a = score(chain), b = score(rev);
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
class VolcanoEnv final : public AttackEnv {
 public:
  explicit VolcanoEnv(const MissionData& m) {
    ordered_ = true;
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
    // Mission_Final_Cave:UpdateMission, on an idle board only.
    if (host.busy()) return;
    Board& b = host.board();
    for (const Pawn& p : b.pawns()) {
      if (p.team == Team::Player && detail::type_is(p, "BigBomb") && (p.alive() || is_corpse(b, p))) return;
    }
    if (!had_bomb_) return;  // no bomb at all: not a recorded final-cave board
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
    SkillEffect fx = env_effect();
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
    SkillEffect fx = env_effect();
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

// Mission_Reactivation (missions/snow/mission_reactivation.lua): NextTurn with
// the enemy team thaws up to two random frozen enemies.
class ReactivationMission final : public Environment {
 public:
  std::string name() const override { return "Mission_Reactivation"; }
  void next_turn(EnvHost& host) override {
    Board& b = host.board();
    std::vector<int32_t> frozen;
    for (const Pawn& p : b.pawns()) {
      if (p.team == Team::Enemy && p.alive() && p.frozen) frozen.push_back(p.uid);
    }
    std::vector<int32_t> thaw = frozen;
    if (frozen.size() > 2) {
      // random_removal over the mission's enemy list: any pair, uniformly.
      std::vector<std::pair<int32_t, int32_t>> pairs;
      for (size_t i = 0; i < frozen.size(); ++i) {
        for (size_t j = i + 1; j < frozen.size(); ++j) pairs.emplace_back(frozen[i], frozen[j]);
      }
      const auto [a, c] = pairs[static_cast<size_t>(
          host.choose(ChanceKind::MissionRandom, static_cast<int>(pairs.size())))];
      thaw = {a, c};
    }
    for (int32_t uid : thaw) {
      if (Pawn* p = b.find_pawn(uid)) {
        set_pawn_frozen(b, *p, false);
        host.note(PhaseEventType::Thawed, "", p->pos, uid);
      }
    }
  }
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
  if (id == "Mission_Reactivation") return std::make_unique<ReactivationMission>();
  if (!m.danger.empty() || !m.freeze.empty()) return std::make_unique<UnsupportedEnv>(m);
  return std::make_unique<NullEnv>(id.empty() ? "Env_Null" : id);
}

bool native_environment_has_update(const MissionData& m) {
  const std::string& id = m.mission_id;
  return id == "Mission_Final" || id == "Mission_Final_Cave" || id == "Mission_Dam" || id == "Mission_Train" ||
         id == "Mission_Armored_Train" || id == "Mission_Volatile" || id == "Mission_AcidStorm" ||
         id == "Mission_Shields" || id == "Mission_Hacking";
}

}  // namespace itb
