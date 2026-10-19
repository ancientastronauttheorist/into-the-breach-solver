// Content hashes of a Board for search (stage 9 transposition tables).
//
// A BoardHash is a 128-bit hash of every field of the board: tiles, the pawn
// list in board-list order with every pawn field, and the board's public
// state. Two boards with equal hashes behave the same for every rule in the
// engine (up to a 128-bit collision, ~2^-64 at 10^9 boards).
//
// One canonicalization makes boards that differ only in history hash equal:
// Pawn::arrival stamps are replaced by each pawn's rank among the pawns on
// its tile. The engine only ever compares stamps of pawns that share a tile,
// and a new stamp is always larger than every existing one, so the ranks are
// all that the stamps can decide. (Moving A then B or B then A gives the same
// board, but not the same stamps.)
#pragma once

#include <cstddef>
#include <cstdint>

#include "itb/board.hpp"

namespace itb {

struct BoardHash {
  uint64_t lo = 0;
  uint64_t hi = 0;

  bool operator==(const BoardHash&) const = default;
};

struct BoardHashOf {
  size_t operator()(const BoardHash& h) const { return static_cast<size_t>(h.lo ^ (h.hi * 0x9E3779B97F4A7C15ull)); }
};

enum class HashMode : uint8_t {
  // Every field (arrival stamps as tile ranks).
  Exact,
  // Every field except the move-undo flags (MoveState::undo_ready). They
  // only gate Pawn::Undo (and Lua IsUndoPossible, read by the tutorial
  // mission alone); a search that never undoes can ignore them, so moving A
  // then firing B and firing B then moving A reach the same key.
  Search,
  // The board as End Turn sees it: Search, minus the player-turn
  // bookkeeping that Engine::end_turn resets before anything else happens
  // (player-team pawns' active, bonus shift and Kickoff bonus). Boards equal
  // in this mode have the same enemy phase.
  EndTurn,
};

BoardHash hash_board(const Board& board, HashMode mode = HashMode::Exact);

// The same equality, but the same value in every process: hash_board folds
// in symbol ids, which depend on the order a process interned the names, so
// its values only compare within one process (the search's tables). This
// one hashes the names (slower: it copies the board). For keys that cross
// processes (itb_live replies and requests: outcome keys, end hashes).
BoardHash hash_board_stable(const Board& board, HashMode mode = HashMode::Exact);

}  // namespace itb
