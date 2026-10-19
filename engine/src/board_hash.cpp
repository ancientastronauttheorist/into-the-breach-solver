#include "itb/board_hash.hpp"

#include <string_view>

#include <array>
#include <cstring>
#include <type_traits>

#include "itb/symbols.hpp"

namespace itb {
namespace {

// Every Tile (as raw bytes) and Pawn field is hashed below. If one of these
// fails, a field was added: hash it, then update the size.
static_assert(sizeof(Tile) == 30, "Tile changed: update hash_board");
static_assert(sizeof(Pawn) == 120, "Pawn changed: update hash_board");
static_assert(sizeof(MoveState) == 16, "MoveState changed: update hash_board");
static_assert(sizeof(QueuedShot) == 20, "QueuedShot changed: update hash_board");

inline uint64_t mix64(uint64_t x) {
  // splitmix64 finalizer
  x ^= x >> 30;
  x *= 0xBF58476D1CE4E5B9ull;
  x ^= x >> 27;
  x *= 0x94D049BB133111EBull;
  x ^= x >> 31;
  return x;
}

// Two independent 64-bit lanes over a stream of 64-bit words.
struct Hasher {
  uint64_t a = 0x243F6A8885A308D3ull;
  uint64_t b = 0x13198A2E03707344ull;
  uint64_t n = 0;

  void word(uint64_t w) {
    ++n;
    a = mix64(a ^ (w + 0x9E3779B97F4A7C15ull * n));
    b = mix64(b + (w ^ 0xC2B2AE3D27D4EB4Full) * (2 * n + 1));
  }
  // Packs small values 8 bits at a time.
  struct Packer {
    Hasher& h;
    uint64_t acc = 0;
    int used = 0;
    void put(uint64_t v, int bits) {
      if (used + bits > 64) flush();
      acc |= (v & ((bits == 64) ? ~0ull : ((1ull << bits) - 1))) << used;
      used += bits;
    }
    void flush() {
      if (used) h.word(acc);
      acc = 0;
      used = 0;
    }
    ~Packer() { flush(); }
  };
  BoardHash done() const { return BoardHash{mix64(a ^ n), mix64(b ^ (n << 1))}; }
};

inline uint64_t u8(int v) { return static_cast<uint8_t>(v); }
inline uint64_t pt(Point p) { return (static_cast<uint64_t>(static_cast<uint32_t>(p.x)) << 32) | static_cast<uint32_t>(p.y); }

// The 64 tiles as raw bytes, 16 at a time, in two independent lanes of
// wyhash-style multiply-fold mixing (a 64x64 -> 128-bit product folded to 64
// bits). Tile has no padding (Board compares tiles with memcmp), so its bytes
// are exactly its fields.
static_assert(std::has_unique_object_representations_v<Tile>, "tiles are hashed as raw bytes");

inline uint64_t mum(uint64_t a, uint64_t b) {
  const unsigned __int128 r = static_cast<unsigned __int128>(a) * b;
  return static_cast<uint64_t>(r) ^ static_cast<uint64_t>(r >> 64);
}

void hash_tiles(Hasher& h, const Board& b) {
  constexpr size_t kBytes = sizeof(Tile) * kTileCount;
  static_assert(kBytes % 16 == 0);
  const auto* bytes = reinterpret_cast<const unsigned char*>(&b.tile(Point::from_index(0)));
  uint64_t a = 0xA0761D6478BD642Full, c = 0xE7037ED1A0B428DBull;
  for (size_t i = 0; i < kBytes; i += 16) {
    uint64_t w0, w1;
    std::memcpy(&w0, bytes + i, 8);
    std::memcpy(&w1, bytes + i + 8, 8);
    a = mum(w0 ^ 0x8EBC6AF09C88C6E3ull, w1 ^ a);
    c = mum(w1 ^ 0x589965CC75374CC3ull, w0 ^ c ^ 0x1D8E4E27C47D124Full);
  }
  h.word(a);
  h.word(c);
}

void hash_pawn(Hasher& h, const Pawn& p, uint32_t tile_rank, HashMode mode) {
  const bool end_turn = mode == HashMode::EndTurn;
  const bool player = p.team == Team::Player;
  h.word(static_cast<uint32_t>(p.uid));
  h.word(pt(p.pos));
  h.word(pt(p.queued.origin));
  h.word(pt(p.queued.target));
  h.word(pt(p.movement.prev_pos));
  Hasher::Packer k{h};
  k.put(p.type, 16);
  for (Symbol w : p.weapons) k.put(w, 16);
  for (int8_t u : p.uses) k.put(u8(u), 8);
  k.put(p.pilot_abilities, 32);
  k.put(u8(p.pilot_level), 8);
  k.put(u8(p.pilot_skill1), 8);
  k.put(u8(p.pilot_skill2), 8);
  k.put(static_cast<uint16_t>(p.pilot_xp), 16);
  k.put(u8(p.extra_dx), 8);
  k.put(u8(p.extra_dy), 8);
  k.put(tile_rank, 16);
  k.put(u8(p.hp), 8);
  k.put(u8(p.max_hp), 8);
  k.put(u8(p.move), 8);
  k.put(u8(static_cast<int>(p.team)), 8);
  k.put(u8(static_cast<int>(p.faction)), 8);
  k.put(u8(static_cast<int>(p.leader)), 8);
  k.put(u8(p.queued.weapon), 8);
  const MoveState& m = p.movement;
  k.put(u8(end_turn && player ? 0 : m.bonus_shift), 8);
  k.put(u8(end_turn && player ? 0 : m.kickoff_bonus), 8);
  k.put(u8(m.pilot_bonus), 8);
  k.put(u8(m.turn_count), 8);
  k.put(m.move_upgrade, 1);
  k.put(m.reset_bonus, 1);
  k.put(m.powered, 1);
  k.put(mode == HashMode::Exact ? m.undo_ready : false, 1);
  k.put(p.mech, 1);
  k.put(p.massive, 1);
  k.put(p.flying, 1);
  k.put(p.pushable, 1);
  k.put(p.armor, 1);
  k.put(p.ignore_smoke, 1);
  k.put(p.ignore_fire, 1);
  k.put(p.minor, 1);
  k.put(p.corpse, 1);
  k.put(p.burrows, 1);
  k.put(p.jumper, 1);
  k.put(p.teleporter, 1);
  k.put(p.explodes, 1);
  k.put(p.burns, 1);
  k.put(p.ignore_flip, 1);
  k.put(p.neutral, 1);
  k.put(p.non_grid, 1);
  k.put(p.mission_critical, 1);
  k.put(end_turn && player ? false : p.active, 1);
  k.put(p.moved, 1);
  k.put(p.fire, 1);
  k.put(p.frozen, 1);
  k.put(p.acid, 1);
  k.put(p.shield, 1);
  k.put(p.boosted, 1);
  k.put(p.webbed, 1);
  k.put(p.infected, 1);
  k.put(p.injured, 1);
  k.put(p.dying, 1);
  k.put(p.health_bonus, 1);
  k.put(p.fallen, 1);
}

}  // namespace

BoardHash hash_board(const Board& b, HashMode mode) {
  Hasher h;
  hash_tiles(h, b);
  {
    Hasher::Packer k{h};
    k.put(static_cast<uint32_t>(b.grid_power), 32);
    k.put(static_cast<uint32_t>(b.grid_power_max), 32);
    k.put(static_cast<uint32_t>(b.grid_defense), 32);
    k.put(static_cast<uint32_t>(b.turn), 32);
    k.put(static_cast<uint32_t>(b.total_turns), 32);
    k.put(b.passives, 32);
    // next_uid only labels pawns created later; kept so that equal hashes
    // mean equal futures, uids included.
    k.put(static_cast<uint32_t>(b.next_uid), 32);
    // Scored (Mission_AcidTank), so boards that differ only here differ.
    k.put(static_cast<uint32_t>(b.acid_destroyed), 32);
    k.put(b.player_phase, 1);
    k.put(u8(static_cast<int>(b.psion)), 8);
    k.put(b.spawn_points.size(), 16);
    k.put(b.teleporters.size(), 16);
    k.put(b.teleporter_occupants.size(), 16);
  }
  for (Point p : b.spawn_points) h.word(pt(p));
  for (Point p : b.teleporters) h.word(pt(p));
  for (int32_t u : b.teleporter_occupants) h.word(static_cast<uint32_t>(u));

  // Arrival stamps as ranks among the pawns sharing a tile (stable on list
  // order, as Board::pawns_at sorts them).
  const std::vector<Pawn>& pawns = b.pawns();
  h.word(pawns.size());
  for (size_t i = 0; i < pawns.size(); ++i) {
    const Pawn& p = pawns[i];
    uint32_t rank = 0;
    for (size_t j = 0; j < pawns.size(); ++j) {
      if (j == i || !(pawns[j].pos == p.pos)) continue;
      const Pawn& q = pawns[j];
      if (q.arrival < p.arrival || (q.arrival == p.arrival && j < i)) ++rank;
    }
    hash_pawn(h, p, rank, mode);
    // Only when set, so boards outside Mission_Reactivation hash as before.
    if (p.reactivation_listed) h.word(0x52454143u);
  }
  return h.done();
}

// hash_board_stable: names instead of symbol ids (which depend on the
// order a process interned them), folded in beside the ordinary hash of a
// copy whose symbols are all kNoSymbol.
BoardHash hash_board_stable(const Board& board, HashMode mode) {
  Board b = board;
  uint64_t names = 0xCBF29CE484222325ull;  // FNV-1a over the names, in board order
  auto mix = [&names](Symbol& s) {
    if (s != kNoSymbol) {
      for (char c : symbol_name(s)) {
        names ^= static_cast<unsigned char>(c);
        names *= 0x100000001B3ull;
      }
    }
    names ^= 0xFF;  // separator: an empty name still counts
    names *= 0x100000001B3ull;
    s = kNoSymbol;
  };
  for (int i = 0; i < kTileCount; ++i) {
    Tile& t = b.tile(Point::from_index(i));
    mix(t.item);
    mix(t.unique_building);
    mix(t.custom_tile);
    mix(t.special_tag);
  }
  for (Pawn& p : b.pawns()) {
    mix(p.type);
    for (Symbol& w : p.weapons) mix(w);
  }
  BoardHash h = hash_board(b, mode);
  h.lo = mix64(h.lo ^ names);
  h.hi = mix64(h.hi ^ (names * 0x9E3779B97F4A7C15ull));
  return h;
}

}  // namespace itb
