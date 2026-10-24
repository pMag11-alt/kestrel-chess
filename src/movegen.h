// Move generation. The generator produces pseudo-legal moves (they may leave the king in check);
// legality is settled by making the move and asking Position::last_move_legal().
#pragma once

#include <cstdint>

#include "position.h"

namespace kestrel {

struct MoveList {
    Move moves[256];  // the most moves any legal chess position has is 218
    int size = 0;

    void add(Move m) { moves[size++] = m; }
    Move* begin() { return moves; }
    Move* end() { return moves + size; }
    const Move* begin() const { return moves; }
    const Move* end() const { return moves + size; }
    bool contains(Move m) const;
};

enum class GenType {
    ALL,    // every pseudo-legal move
    NOISY,  // captures and promotions, for the quiescence search
};

void generate(const Position& pos, MoveList& list, GenType type = GenType::ALL);
// Only the moves that do not leave the mover in check.
void generate_legal(Position& pos, MoveList& list);

// Counts leaf nodes of the legal move tree to the given depth. The standard way to check a
// move generator: the right totals for well-known positions are published.
uint64_t perft(Position& pos, int depth);

}  // namespace kestrel
