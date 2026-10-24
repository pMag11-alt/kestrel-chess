// Precomputed attack tables. Sliding pieces use "fancy" magic bitboards: the blockers on a
// piece's rays are hashed by a multiply-and-shift into a per-square table of attack sets.
#pragma once

#include "types.h"

namespace kestrel {

constexpr Bitboard FILE_A = 0x0101010101010101ULL;
constexpr Bitboard FILE_H = FILE_A << 7;
constexpr Bitboard RANK_1 = 0xFFULL;
constexpr Bitboard RANK_8 = RANK_1 << 56;

constexpr Bitboard file_bb(int file) { return FILE_A << file; }
constexpr Bitboard rank_bb(int rank) { return RANK_1 << (8 * rank); }

constexpr Bitboard north(Bitboard b) { return b << 8; }
constexpr Bitboard south(Bitboard b) { return b >> 8; }
constexpr Bitboard east(Bitboard b) { return (b & ~FILE_H) << 1; }
constexpr Bitboard west(Bitboard b) { return (b & ~FILE_A) >> 1; }
// One step "forward" for the given side.
constexpr Bitboard push(Color c, Bitboard b) { return c == WHITE ? north(b) : south(b); }

struct Magic {
    Bitboard mask;     // relevant blocker squares (rays without the board edge)
    Bitboard magic;
    Bitboard* attacks; // this square's slice of the shared table
    int shift;

    unsigned index(Bitboard occupied) const { return unsigned(((occupied & mask) * magic) >> shift); }
};

extern Bitboard PAWN_ATTACKS[2][64];
extern Bitboard KNIGHT_ATTACKS[64];
extern Bitboard KING_ATTACKS[64];
extern Bitboard BETWEEN[64][64];  // squares strictly between two aligned squares, else 0
extern Bitboard LINE[64][64];     // the full line through two aligned squares, else 0
extern Magic ROOK_MAGICS[64];
extern Magic BISHOP_MAGICS[64];

// Must run once before anything else touches a board. Safe to call more than once.
void init_bitboards();

inline Bitboard rook_attacks(int sq, Bitboard occupied) {
    const Magic& m = ROOK_MAGICS[sq];
    return m.attacks[m.index(occupied)];
}

inline Bitboard bishop_attacks(int sq, Bitboard occupied) {
    const Magic& m = BISHOP_MAGICS[sq];
    return m.attacks[m.index(occupied)];
}

inline Bitboard queen_attacks(int sq, Bitboard occupied) {
    return rook_attacks(sq, occupied) | bishop_attacks(sq, occupied);
}

inline Bitboard attacks_of(PieceType pt, int sq, Bitboard occupied) {
    switch (pt) {
        case KNIGHT: return KNIGHT_ATTACKS[sq];
        case BISHOP: return bishop_attacks(sq, occupied);
        case ROOK: return rook_attacks(sq, occupied);
        case QUEEN: return queen_attacks(sq, occupied);
        case KING: return KING_ATTACKS[sq];
        default: return 0;
    }
}

// Slow ray-walking reference used to build the magic tables (and by the tests to check them).
Bitboard sliding_attacks_slow(PieceType pt, int sq, Bitboard occupied);

}  // namespace kestrel
