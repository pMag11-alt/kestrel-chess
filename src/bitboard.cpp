#include "bitboard.h"

#include <cstdlib>
#include <initializer_list>
#include <vector>

namespace kestrel {

Bitboard PAWN_ATTACKS[2][64];
Bitboard KNIGHT_ATTACKS[64];
Bitboard KING_ATTACKS[64];
Bitboard BETWEEN[64][64];
Bitboard LINE[64][64];
Magic ROOK_MAGICS[64];
Magic BISHOP_MAGICS[64];

namespace {

// 102,400 rook entries and 5,248 bishop entries cover every blocker subset of every square.
Bitboard rook_table[0x19000];
Bitboard bishop_table[0x1480];

const int ROOK_DIRS[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
const int BISHOP_DIRS[4][2] = {{1, 1}, {1, -1}, {-1, 1}, {-1, -1}};

// xorshift64*: small, fast and deterministic, so the magics are the same on every run.
struct Rng {
    uint64_t s;
    uint64_t next() {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return s * 2685821657736338717ULL;
    }
    // Magics with few set bits are found much faster.
    uint64_t sparse() { return next() & next() & next(); }
};

Bitboard step_attacks(int sq, const int (*deltas)[2], int count) {
    Bitboard b = 0;
    for (int i = 0; i < count; ++i) {
        int f = file_of(sq) + deltas[i][0], r = rank_of(sq) + deltas[i][1];
        if (f >= 0 && f < 8 && r >= 0 && r < 8) b |= bit(make_square(f, r));
    }
    return b;
}

void init_magics(PieceType pt, Magic* magics, Bitboard* table) {
    Rng rng{0x9E3779B97F4A7C15ULL ^ uint64_t(pt)};
    std::vector<Bitboard> occupancy(4096), reference(4096);
    std::vector<int> epoch(4096, 0);
    int attempt = 0;
    Bitboard* next_slice = table;

    for (int sq = 0; sq < 64; ++sq) {
        // Edge squares never change what a slider can reach, so they are left out of the mask
        // unless the piece itself stands on that edge.
        Bitboard edges = ((RANK_1 | RANK_8) & ~rank_bb(rank_of(sq))) | ((FILE_A | FILE_H) & ~file_bb(file_of(sq)));
        Magic& m = magics[sq];
        m.mask = sliding_attacks_slow(pt, sq, 0) & ~edges;
        m.shift = 64 - popcount(m.mask);
        m.attacks = next_slice;

        // Enumerate every subset of the mask with the Carry-Rippler trick.
        int size = 0;
        Bitboard subset = 0;
        do {
            occupancy[size] = subset;
            reference[size] = sliding_attacks_slow(pt, sq, subset);
            ++size;
            subset = (subset - m.mask) & m.mask;
        } while (subset);

        // Try random candidates until one maps every subset without a destructive collision.
        for (int i = 0; i < size;) {
            do {
                m.magic = rng.sparse();
            } while (popcount((m.mask * m.magic) >> 56) < 6);
            ++attempt;
            for (i = 0; i < size; ++i) {
                unsigned idx = m.index(occupancy[i]);
                if (epoch[idx] < attempt) {
                    epoch[idx] = attempt;
                    m.attacks[idx] = reference[i];
                } else if (m.attacks[idx] != reference[i]) {
                    break;
                }
            }
        }
        next_slice += size;
    }
}

}  // namespace

std::string square_name(int sq) {
    if (sq < 0 || sq >= 64) return "-";
    return std::string{char('a' + file_of(sq)), char('1' + rank_of(sq))};
}

int parse_square(const std::string& s) {
    if (s.size() != 2 || s[0] < 'a' || s[0] > 'h' || s[1] < '1' || s[1] > '8') return NO_SQ;
    return make_square(s[0] - 'a', s[1] - '1');
}

std::string move_to_uci(Move m) {
    if (m == NO_MOVE) return "0000";
    std::string s = square_name(move_from(m)) + square_name(move_to(m));
    if (is_promotion(m)) s += "nbrq"[promotion_type(m) - KNIGHT];
    return s;
}

Bitboard sliding_attacks_slow(PieceType pt, int sq, Bitboard occupied) {
    const int(*dirs)[2] = pt == ROOK ? ROOK_DIRS : BISHOP_DIRS;
    Bitboard attacks = 0;
    for (int d = 0; d < 4; ++d) {
        int f = file_of(sq), r = rank_of(sq);
        while (true) {
            f += dirs[d][0];
            r += dirs[d][1];
            if (f < 0 || f > 7 || r < 0 || r > 7) break;
            int to = make_square(f, r);
            attacks |= bit(to);
            if (occupied & bit(to)) break;
        }
    }
    return attacks;
}

void init_bitboards() {
    static bool done = false;
    if (done) return;
    done = true;

    const int knight[8][2] = {{1, 2}, {2, 1}, {2, -1}, {1, -2}, {-1, -2}, {-2, -1}, {-2, 1}, {-1, 2}};
    const int king[8][2] = {{1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}};
    const int white_pawn[2][2] = {{-1, 1}, {1, 1}};
    const int black_pawn[2][2] = {{-1, -1}, {1, -1}};
    for (int sq = 0; sq < 64; ++sq) {
        KNIGHT_ATTACKS[sq] = step_attacks(sq, knight, 8);
        KING_ATTACKS[sq] = step_attacks(sq, king, 8);
        PAWN_ATTACKS[WHITE][sq] = step_attacks(sq, white_pawn, 2);
        PAWN_ATTACKS[BLACK][sq] = step_attacks(sq, black_pawn, 2);
    }

    init_magics(ROOK, ROOK_MAGICS, rook_table);
    init_magics(BISHOP, BISHOP_MAGICS, bishop_table);

    for (int a = 0; a < 64; ++a) {
        for (int b = 0; b < 64; ++b) {
            BETWEEN[a][b] = LINE[a][b] = 0;
            if (a == b) continue;
            for (PieceType pt : {BISHOP, ROOK}) {
                if (sliding_attacks_slow(pt, a, 0) & bit(b)) {
                    BETWEEN[a][b] = sliding_attacks_slow(pt, a, bit(b)) & sliding_attacks_slow(pt, b, bit(a));
                    LINE[a][b] = (sliding_attacks_slow(pt, a, 0) & sliding_attacks_slow(pt, b, 0)) | bit(a) | bit(b);
                }
            }
        }
    }
}

}  // namespace kestrel
