#include "itb/threat.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <string>
#include <string_view>
#include <utility>

#include "itb/movement.hpp"
#include "itb/tile_rules.hpp"

namespace itb {
namespace {

using HitTable = std::array<uint64_t, kTileCount>;

// Reach class ids. The footprint ones index Footprints::tables.
enum ClassId : uint8_t {
  kNone = 0,
  kMelee,       // the adjacent tile (melee, and self-centred blasts over the 4 neighbours)
  kReach2,      // adjacent tile and the one behind it (Hornet beta)
  kReach3,      // three tiles in line (Hornet boss)
  kMeleeSides,  // adjacent tile and its two side neighbours (Burrower)
  kDiagonal,    // the 4 diagonal neighbours (Starfish)
  kArt5,        // artillery, range 2..5 (Scarab, Moth, Scarab boss)
  kCrab,        // artillery 2..5 and the tile behind the target (Crab)
  kSnowart,     // artillery 2..5 and one side tile of the target (Snow artillery, Bot boss)
  kCrabBoss,    // every tile from the adjacent one to the target, range 2..4 (Crab boss)
  kLeaperBoss,  // leaps 1..4 tiles in line, then hits the landing tile's neighbours
  kDung,        // rock on the adjacent tile, smashed: it and its neighbours (Tumblebug)
  kDungB,       // two rocks in line (Tumblebug boss)
  kAnyLine,     // unknown weapons: every tile in line, range 1..7
  kFootprints,  // (count)
  kProjectile = kFootprints,
  kSplash,  // projectile plus the two tiles beside its end (Centipede)
  kLaser,
};

struct Offset {
  int forward;
  int side;
};

HitTable build(std::initializer_list<Offset> offsets) {
  HitTable t{};
  for (int i = 0; i < kTileCount; ++i) {
    const Point o = Point::from_index(i);
    for (int d = 0; d < 4; ++d) {
      const Point f = kDirVectors[static_cast<size_t>(d)];
      const Point s = kDirVectors[static_cast<size_t>((d + 1) % 4)];
      for (const Offset& off : offsets) {
        const Point p = o + f * off.forward + s * off.side;
        if (p.valid() && !(p == o)) t[static_cast<size_t>(i)] |= uint64_t{1} << p.index();
      }
    }
  }
  return t;
}

struct Footprints {
  std::array<HitTable, kFootprints> tables{};
  Footprints() {
    tables[kMelee] = build({{1, 0}});
    tables[kReach2] = build({{1, 0}, {2, 0}});
    tables[kReach3] = build({{1, 0}, {2, 0}, {3, 0}});
    tables[kMeleeSides] = build({{1, 0}, {1, 1}, {1, -1}});
    tables[kDiagonal] = build({{1, 1}});
    tables[kArt5] = build({{2, 0}, {3, 0}, {4, 0}, {5, 0}});
    tables[kCrab] = build({{2, 0}, {3, 0}, {4, 0}, {5, 0}, {6, 0}});
    tables[kSnowart] = build({{2, 0}, {3, 0}, {4, 0}, {5, 0}, {2, 1}, {3, 1}, {4, 1}, {5, 1}});
    tables[kCrabBoss] = build({{1, 0}, {2, 0}, {3, 0}, {4, 0}});
    tables[kLeaperBoss] = build({{1, 0}, {2, 0}, {3, 0}, {4, 0}, {5, 0}, {1, 1}, {2, 1}, {3, 1}, {4, 1}, {1, -1},
                                 {2, -1}, {3, -1}, {4, -1}});
    tables[kDung] = build({{1, 0}, {2, 0}, {1, 1}, {1, -1}});
    tables[kDungB] = build({{1, 0}, {2, 0}, {3, 0}, {1, 1}, {1, -1}, {2, 1}, {2, -1}});
    tables[kAnyLine] = build({{1, 0}, {2, 0}, {3, 0}, {4, 0}, {5, 0}, {6, 0}, {7, 0}});
  }
};

const Footprints& footprints() {
  static const Footprints f;
  return f;
}

// Weapon table names: exact names first, then the family (the name without
// trailing digits and without an "_Acid" / "_Player" suffix).
ClassId classify(std::string_view name) {
  struct Entry {
    std::string_view name;
    ClassId id;
  };
  static constexpr Entry kExact[] = {
      {"HornetAtk2", kReach2},       {"HornetAtkB", kReach3},      {"CrabAtkB", kCrabBoss},
      {"LeaperAtkB", kLeaperBoss},   {"ScorpionAtkB", kMelee},
      {"DungAtkB", kDungB},          {"SnowBossAtk", kSnowart},    {"BeetleAtkB", kProjectile},
      {"FireflyAtkB", kProjectile},  {"CentipedeAtkB", kSplash},   {"BurnbugAtkB", kProjectile},
      {"BouncerAtkB", kMelee},       {"MosquitoAtkB", kMelee},     {"DiggerAtkB", kMelee},
      {"ScarabAtkB", kArt5},         {"BlobberAtkB", kNone},       {"BlobAtkB", kMelee},
      {"ShamanAtkB", kNone},         {"TotemAtkB", kProjectile},   {"BlobBossAtk", kMelee},
      {"BlobBossAtkMed", kMelee},    {"BlobBossAtkSmall", kMelee}, {"BossHeal", kNone},
  };
  static constexpr Entry kFamily[] = {
      {"SpiderlingAtk", kMelee},    {"ScorpionAtk", kMelee},   {"LeaperAtk", kMelee},
      {"HornetAtk", kMelee},        {"BouncerAtk", kMelee},    {"MosquitoAtk", kMelee},
      {"DiggerAtk", kMelee},        {"BlobAtk", kMelee},       {"Burrower_Atk", kMeleeSides},
      {"StarfishAtk", kDiagonal},   {"StarfishAtkB", kDiagonal}, {"DungAtk", kDung},
      {"FireflyAtk", kProjectile},  {"SnowtankAtk", kProjectile}, {"TotemAtk", kProjectile},
      {"BurnbugAtk", kProjectile},  {"BeetleAtk", kProjectile}, {"CentipedeAtk", kSplash},
      {"SnowlaserAtk", kLaser},     {"ScarabAtk", kArt5},      {"MothAtk", kArt5},
      {"CrabAtk", kCrab},           {"SnowartAtk", kSnowart},  {"SnowBossAtk", kSnowart},
      {"SpiderAtk", kNone},         {"BlobberAtk", kNone},     {"ShamanAtk", kNone},
      {"WebeggHatch", kNone},       {"SpiderlingHatch", kNone}, {"SnowmineAtk", kNone},
  };
  for (const Entry& e : kExact) {
    if (e.name == name) return e.id;
  }
  std::string_view family = name;
  for (std::string_view suffix : {"_Acid", "_Player"}) {
    if (family.ends_with(suffix)) family.remove_suffix(suffix.size());
  }
  while (!family.empty() && family.back() >= '0' && family.back() <= '9') family.remove_suffix(1);
  for (const Entry& e : kFamily) {
    if (e.name == family) return e.id;
  }
  if (name.ends_with("_Tooltip")) return kNone;  // psion and boss tooltips
  return kAnyLine;
}

// 0 = not classified yet, else ClassId + 1.
std::array<std::atomic<uint8_t>, 65536>& class_cache() {
  static std::array<std::atomic<uint8_t>, 65536> cache{};
  return cache;
}

ClassId class_of(Symbol w) {
  if (w == kNoSymbol) return kNone;
  std::atomic<uint8_t>& slot = class_cache()[w];
  uint8_t v = slot.load(std::memory_order_relaxed);
  if (v == 0) {
    v = static_cast<uint8_t>(classify(symbol_name(w)) + 1);
    slot.store(v, std::memory_order_relaxed);
  }
  return static_cast<ClassId>(v - 1);
}

bool scored_enemy(const Pawn& p) { return p.team == Team::Enemy && !p.neutral && !p.mech; }

// The units a mission's objective protects (objectives.cpp, protect() and
// the train, satellite and bomb lines). Player-team only where an enemy pawn
// shares the type.
bool protected_unit(const Pawn& p) {
  static const auto kTypes = [] {
    std::array<Symbol, 13> t{};
    const char* names[] = {"Terraformer", "Archive_Tank", "VIP_Truck",  "ProtoBomb",      "ArchiveArtillery",
                           "Filler_Pawn", "Disposal_Unit", "Train_Pawn", "Train_Armored", "Train_Damaged",
                           "Train_Armored_Damaged", "SatelliteRocket", "GlowingScorpion"};
    for (size_t i = 0; i < t.size(); ++i) t[i] = intern(names[i]);
    return t;
  }();
  static const Symbol kSnowmine = intern("Snowmine1"), kBigBomb = intern("BigBomb");
  if ((p.type == kSnowmine || p.type == kBigBomb) && p.team == Team::Player) return true;
  for (Symbol t : kTypes) {
    if (p.type == t) return true;
  }
  return false;
}

constexpr uint64_t bit(Point p) { return uint64_t{1} << p.index(); }

// Line weapons: the tiles a projectile / splash / laser shot from each tile
// can hit a target from (bit o set = some direction from o hits one).
struct LineOrigins {
  uint64_t projectile = 0, splash = 0, laser = 0;
};

LineOrigins line_origins(uint64_t targets, uint64_t stops, uint64_t walls) {
  LineOrigins out;
  for (int i = 0; i < kTileCount; ++i) {
    const Point o = Point::from_index(i);
    for (int d = 0; d < 4; ++d) {
      const Point step = kDirVectors[static_cast<size_t>(d)];
      Point t = o + step;
      if (!t.valid()) continue;
      // Projectile: the first stopping tile, or the last tile of the board.
      Point end = t;
      for (Point q = t; q.valid(); q = q + step) {
        end = q;
        if (stops & bit(q)) break;
      }
      // Laser: up to the first building or mountain, through pawns.
      bool laser_hit = false;
      for (Point q = t; q.valid(); q = q + step) {
        if (targets & bit(q)) laser_hit = true;
        if (walls & bit(q)) break;
      }
      if (targets & bit(end)) {
        out.projectile |= bit(o);
        out.splash |= bit(o);
      } else {
        const Point s = kDirVectors[static_cast<size_t>((d + 1) % 4)];
        for (Point side : {end + s, end - s}) {
          if (side.valid() && (targets & bit(side))) out.splash |= bit(o);
        }
      }
      if (laser_hit) out.laser |= bit(o);
    }
  }
  return out;
}

}  // namespace

ThreatClass threat_class(Symbol weapon) {
  const ClassId id = class_of(weapon);
  ThreatClass c;
  if (id == kNone) return c;
  if (id < kFootprints) {
    c.reach = ThreatReach::Footprint;
    c.hit = &footprints().tables[id];
  } else {
    c.reach = id == kProjectile ? ThreatReach::Projectile : id == kSplash ? ThreatReach::ProjectileSplash : ThreatReach::Laser;
  }
  return c;
}

namespace {

// Calls found(uid) for every enemy that threatens a building (units = false)
// or a protected mission unit (units = true), in board-list order.
template <class Found>
void scan(const Board& b, Found&& found, bool units = false) {
  uint64_t targets = 0, walls = 0, smoke = 0;
  for (int i = 0; i < kTileCount; ++i) {
    const Tile& t = b.tile(Point::from_index(i));
    if (t.is_building() && t.hp > 0) {
      walls |= uint64_t{1} << i;
      if (!t.frozen && !units) targets |= uint64_t{1} << i;
    } else if (t.is_mountain()) {
      walls |= uint64_t{1} << i;
    }
    if (t.smoke) smoke |= uint64_t{1} << i;
  }
  for (const Pawn& p : b.pawns()) {
    if (!units) break;
    if (!p.alive() || !p.pos.valid() || p.fallen || p.frozen || !protected_unit(p)) continue;
    targets |= bit(p.pos);
    if (p.multi_tile() && p.extra_tile().valid()) targets |= bit(p.extra_tile());
  }
  if (targets == 0) return;

  bool lines_done = false;
  LineOrigins lines;
  for (const Pawn& p : b.pawns()) {
    if (!scored_enemy(p) || !p.alive() || p.fallen || p.frozen) continue;
    const Point origin = p.pos.valid() ? p.pos : (p.burrows ? p.movement.prev_pos : kInvalidPoint);
    if (!origin.valid()) continue;
    ClassId ids[kMaxWeapons];
    int n = 0;
    bool lines_needed = false;
    for (Symbol w : p.weapons) {
      const ClassId id = class_of(w);
      if (id == kNone) continue;
      ids[n++] = id;
      if (id >= kFootprints) lines_needed = true;
    }
    if (n == 0) continue;
    const int speed = move_speed(b, p);
    uint64_t from = bit(origin);
    if (speed > 0) from |= reachable_points(b, origin, speed, path_profile(b, p));
    if (!p.ignore_smoke) from &= ~smoke;
    if (from == 0) continue;
    if (lines_needed && !lines_done) {
      // What stops a projectile: mountains, buildings and every pawn (or
      // corpse) that will still be there when the AI shoots, i.e. all but
      // enemies that are about to move.
      uint64_t stops = walls;
      for (const Pawn& q : b.pawns()) {
        if (!q.pos.valid() || q.fallen || !(q.alive() || is_corpse(b, q))) continue;
        if (scored_enemy(q) && q.alive() && !q.frozen && move_speed(b, q) > 0) continue;
        stops |= bit(q.pos);
        if (q.multi_tile() && q.extra_tile().valid()) stops |= bit(q.extra_tile());
      }
      lines = line_origins(targets, stops, walls);
      lines_done = true;
    }
    bool threat = false;
    for (int k = 0; k < n && !threat; ++k) {
      switch (ids[k]) {
        case kProjectile:
          threat = (from & lines.projectile) != 0;
          break;
        case kSplash:
          threat = (from & lines.splash) != 0;
          break;
        case kLaser:
          threat = (from & lines.laser) != 0;
          break;
        default: {
          const HitTable& hit = footprints().tables[ids[k]];
          for (uint64_t m = from; m && !threat; m &= m - 1) {
            threat = (hit[static_cast<size_t>(std::countr_zero(m))] & targets) != 0;
          }
        }
      }
    }
    if (threat) found(p.uid);
  }
}

}  // namespace

std::vector<int32_t> threatening_enemies(const Board& b) {
  std::vector<int32_t> out;
  scan(b, [&](int32_t uid) { out.push_back(uid); });
  return out;
}

int count_threats(const Board& b) {
  int n = 0;
  scan(b, [&](int32_t) { ++n; });
  return n;
}

std::vector<int32_t> unit_threatening_enemies(const Board& b) {
  std::vector<int32_t> out;
  scan(b, [&](int32_t uid) { out.push_back(uid); }, true);
  return out;
}

int count_unit_threats(const Board& b) {
  int n = 0;
  scan(b, [&](int32_t) { ++n; }, true);
  return n;
}

}  // namespace itb
