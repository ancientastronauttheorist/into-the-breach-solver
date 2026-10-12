#include "itb/board.hpp"

#include <algorithm>
#include <cstring>
#include <type_traits>

namespace itb {
namespace {

// Board::AddPawn rebuilds the list in three passes, each keeping the previous
// relative order:
//   0: controlled player units, bots-faction player units, and Filler_Pawn
//   1: every non-player-team pawn (Vek, neutrals, structures)
//   2: the remaining (neutral) player-team units
int list_pass(const Pawn& p) {
  if (p.team != Team::Player) return 1;
  static const Symbol filler = intern("Filler_Pawn");
  if (!p.neutral || p.faction == Faction::Bots || p.type == filler) return 0;
  return 2;
}

}  // namespace

Pawn& Board::add_pawn(const Pawn& pawn) {
  next_uid = std::max(next_uid, pawn.uid + 1);
  pawns_.push_back(pawn);
  const int32_t uid = pawn.uid;
  std::stable_sort(pawns_.begin(), pawns_.end(), [](const Pawn& a, const Pawn& b) {
    return list_pass(a) < list_pass(b);
  });
  // The appended pawn is the last one in its pass.
  for (auto it = pawns_.rbegin(); it != pawns_.rend(); ++it) {
    if (it->uid == uid && list_pass(*it) == list_pass(pawn)) return *it;
  }
  return pawns_.back();  // unreachable
}

void Board::remove_pawn(int32_t uid) {
  std::erase_if(pawns_, [uid](const Pawn& p) { return p.uid == uid; });
}

std::vector<Pawn*> Board::pawns_at(Point p) {
  std::vector<Pawn*> out;
  for (Pawn& pawn : pawns_) {
    if (pawn.occupies(p)) out.push_back(&pawn);
  }
  std::stable_sort(out.begin(), out.end(),
                   [](const Pawn* a, const Pawn* b) { return a->arrival < b->arrival; });
  return out;
}

Pawn* Board::pawn_at(Point p) {
  Pawn* first = nullptr;
  for (Pawn& pawn : pawns_) {
    if (pawn.occupies(p) && (!first || pawn.arrival < first->arrival)) first = &pawn;
  }
  return first;
}

const Pawn* Board::pawn_at(Point p) const {
  return const_cast<Board*>(this)->pawn_at(p);
}

// memcmp on the tiles equals comparing every field: Tile has unique object
// representations (no padding, no floating point).
static_assert(std::has_unique_object_representations_v<Tile>, "Tile must stay free of padding");

bool Board::operator==(const Board& o) const {
  return std::memcmp(tiles_.data(), o.tiles_.data(), sizeof(tiles_)) == 0 && pawns_ == o.pawns_ &&
         arrival_clock_ == o.arrival_clock_ && grid_power == o.grid_power && grid_power_max == o.grid_power_max &&
         grid_defense == o.grid_defense && turn == o.turn && total_turns == o.total_turns &&
         player_phase == o.player_phase && passives == o.passives && next_uid == o.next_uid && psion == o.psion &&
         spawn_points == o.spawn_points && teleporters == o.teleporters &&
         teleporter_occupants == o.teleporter_occupants && acid_destroyed == o.acid_destroyed;
}

Pawn* Board::find_pawn(int32_t uid) {
  for (Pawn& pawn : pawns_) {
    if (pawn.uid == uid) return &pawn;
  }
  return nullptr;
}

const Pawn* Board::find_pawn(int32_t uid) const {
  return const_cast<Board*>(this)->find_pawn(uid);
}

}  // namespace itb
