// Core types shared by every part of the engine: squares, pieces, moves and bit tricks.
#pragma once

#include <cstdint>
#include <string>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace kestrel {

using Bitboard = uint64_t;
using Key = uint64_t;

enum Color : int { WHITE, BLACK };
constexpr Color operator~(Color c) { return Color(c ^ 1); }

enum PieceType : int { PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING, NO_PIECE_TYPE };

// White pieces are 0-5 and black pieces 6-11, so piece = color * 6 + type.
enum Piece : int {
    W_PAWN, W_KNIGHT, W_BISHOP, W_ROOK, W_QUEEN, W_KING,
    B_PAWN, B_KNIGHT, B_BISHOP, B_ROOK, B_QUEEN, B_KING,
    NO_PIECE
};

constexpr Piece make_piece(Color c, PieceType pt) { return Piece(c * 6 + pt); }
constexpr PieceType type_of(Piece p) { return PieceType(p % 6); }
constexpr Color color_of(Piece p) { return Color(p / 6); }

// Squares are numbered a1 = 0, b1 = 1, ..., h8 = 63.
enum Square : int {
    A1, B1, C1, D1, E1, F1, G1, H1,
    A2, B2, C2, D2, E2, F2, G2, H2,
    A3, B3, C3, D3, E3, F3, G3, H3,
    A4, B4, C4, D4, E4, F4, G4, H4,
    A5, B5, C5, D5, E5, F5, G5, H5,
    A6, B6, C6, D6, E6, F6, G6, H6,
    A7, B7, C7, D7, E7, F7, G7, H7,
    A8, B8, C8, D8, E8, F8, G8, H8,
    NO_SQ
};

constexpr int file_of(int sq) { return sq & 7; }
constexpr int rank_of(int sq) { return sq >> 3; }
constexpr int make_square(int file, int rank) { return rank * 8 + file; }
// The same square seen from black's side of the board (a1 <-> a8).
constexpr int flip_rank(int sq) { return sq ^ 56; }
// The rank counted from the given side: a white pawn on e2 and a black pawn on e7 are both on relative rank 1.
constexpr int relative_rank(Color c, int sq) { return c == WHITE ? rank_of(sq) : 7 - rank_of(sq); }

enum CastlingRight : int { WHITE_OO = 1, WHITE_OOO = 2, BLACK_OO = 4, BLACK_OOO = 8, ALL_CASTLING = 15 };

// A move packs into 16 bits: from (6) | to (6) | flags (4), using the flag scheme from the
// Chess Programming Wiki so that "is this a capture / promotion" are single bit tests.
using Move = uint16_t;
constexpr Move NO_MOVE = 0;

enum MoveFlag : int {
    QUIET = 0, DOUBLE_PUSH = 1, KING_CASTLE = 2, QUEEN_CASTLE = 3,
    CAPTURE = 4, EP_CAPTURE = 5,
    PROMO_N = 8, PROMO_B = 9, PROMO_R = 10, PROMO_Q = 11,
    PROMO_CAPTURE_N = 12, PROMO_CAPTURE_B = 13, PROMO_CAPTURE_R = 14, PROMO_CAPTURE_Q = 15
};

constexpr Move make_move(int from, int to, int flag = QUIET) { return Move(from | (to << 6) | (flag << 12)); }
constexpr int move_from(Move m) { return m & 63; }
constexpr int move_to(Move m) { return (m >> 6) & 63; }
constexpr int move_flag(Move m) { return m >> 12; }
constexpr bool is_capture(Move m) { return move_flag(m) & CAPTURE; }
constexpr bool is_promotion(Move m) { return move_flag(m) & 8; }
constexpr bool is_castle(Move m) { return move_flag(m) == KING_CASTLE || move_flag(m) == QUEEN_CASTLE; }
constexpr PieceType promotion_type(Move m) { return PieceType(KNIGHT + (move_flag(m) & 3)); }
// Captures and promotions change material, so the search treats them as "noisy".
constexpr bool is_quiet(Move m) { return !is_capture(m) && !is_promotion(m); }

// Scores are in centipawns. Mate scores count down from MATE so shorter mates score higher.
constexpr int INF = 32000;
constexpr int MATE = 31000;
constexpr int MATE_BOUND = MATE - 512;  // anything above this is "mate in N"
constexpr int MAX_PLY = 128;

constexpr Bitboard bit(int sq) { return Bitboard(1) << sq; }

inline int popcount(Bitboard b) {
#if defined(_MSC_VER)
    return int(__popcnt64(b));
#else
    return __builtin_popcountll(b);
#endif
}

// Index of the least significant set bit. b must be non-zero.
inline int lsb(Bitboard b) {
#if defined(_MSC_VER)
    unsigned long idx;
    _BitScanForward64(&idx, b);
    return int(idx);
#else
    return __builtin_ctzll(b);
#endif
}

inline int msb(Bitboard b) {
#if defined(_MSC_VER)
    unsigned long idx;
    _BitScanReverse64(&idx, b);
    return int(idx);
#else
    return 63 ^ __builtin_clzll(b);
#endif
}

inline int pop_lsb(Bitboard& b) {
    int sq = lsb(b);
    b &= b - 1;
    return sq;
}

inline bool more_than_one(Bitboard b) { return b & (b - 1); }

std::string square_name(int sq);
int parse_square(const std::string& s);  // "e4" -> E4, NO_SQ if malformed
std::string move_to_uci(Move m);

}  // namespace kestrel
