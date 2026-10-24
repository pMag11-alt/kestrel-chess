#include "eval.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

namespace kestrel {

namespace {

// A score with separate middlegame and endgame parts, blended by the game phase at the end.
struct Score {
    int mg = 0, eg = 0;
    Score() = default;
    constexpr Score(int m, int e) : mg(m), eg(e) {}
    Score& operator+=(Score o) { mg += o.mg; eg += o.eg; return *this; }
    Score& operator-=(Score o) { mg -= o.mg; eg -= o.eg; return *this; }
    Score operator+(Score o) const { return {mg + o.mg, eg + o.eg}; }
    Score operator-(Score o) const { return {mg - o.mg, eg - o.eg}; }
    Score operator*(int k) const { return {mg * k, eg * k}; }
};

const Score MATERIAL[6] = {{85, 110}, {330, 300}, {350, 315}, {470, 530}, {1000, 960}, {0, 0}};
const int PHASE_WEIGHT[6] = {0, 1, 1, 2, 4, 0};
constexpr int MAX_PHASE = 24;

// Piece-square tables, written from white's side with rank 8 on top so they read like a board.
// Black uses the same tables mirrored vertically.
// clang-format off
const int PAWN_MG[64] = {
     0,   0,   0,   0,   0,   0,   0,   0,
    30,  35,  40,  45,  45,  40,  35,  30,
    10,  15,  20,  30,  30,  20,  15,  10,
     2,   5,  10,  25,  25,  10,   5,   2,
     0,   0,   5,  20,  20,   0,   0,   0,
     4,  -2,  -5,   5,   5, -10,  -2,   4,
     4,   8,   8, -20, -20,   8,   8,   4,
     0,   0,   0,   0,   0,   0,   0,   0,
};
const int PAWN_EG[64] = {
     0,   0,   0,   0,   0,   0,   0,   0,
    60,  60,  60,  60,  60,  60,  60,  60,
    35,  35,  35,  35,  35,  35,  35,  35,
    18,  18,  18,  18,  18,  18,  18,  18,
     8,   8,   8,   8,   8,   8,   8,   8,
     2,   2,   2,   2,   2,   2,   2,   2,
     0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,   0,   0,   0,   0,   0,   0,
};
const int KNIGHT_PST[64] = {
   -50, -40, -30, -30, -30, -30, -40, -50,
   -40, -20,   0,   5,   5,   0, -20, -40,
   -30,   5,  10,  15,  15,  10,   5, -30,
   -30,   0,  15,  20,  20,  15,   0, -30,
   -30,   5,  15,  20,  20,  15,   5, -30,
   -30,   0,  10,  15,  15,  10,   0, -30,
   -40, -20,   0,   0,   0,   0, -20, -40,
   -50, -35, -30, -30, -30, -30, -35, -50,
};
const int BISHOP_PST[64] = {
   -20, -10, -10, -10, -10, -10, -10, -20,
   -10,   0,   0,   0,   0,   0,   0, -10,
   -10,   0,   5,  10,  10,   5,   0, -10,
   -10,   5,   5,  10,  10,   5,   5, -10,
   -10,   0,  10,  10,  10,  10,   0, -10,
   -10,  10,  10,  10,  10,  10,  10, -10,
   -10,   5,   0,   0,   0,   0,   5, -10,
   -20, -10, -15, -10, -10, -15, -10, -20,
};
const int ROOK_PST[64] = {
     0,   0,   0,   0,   0,   0,   0,   0,
    10,  15,  15,  15,  15,  15,  15,  10,
    -5,   0,   0,   0,   0,   0,   0,  -5,
    -5,   0,   0,   0,   0,   0,   0,  -5,
    -5,   0,   0,   0,   0,   0,   0,  -5,
    -5,   0,   0,   0,   0,   0,   0,  -5,
    -5,   0,   0,   0,   0,   0,   0,  -5,
     0,   0,   3,   8,   8,   5,   0,   0,
};
const int QUEEN_PST[64] = {
   -20, -10, -10,  -5,  -5, -10, -10, -20,
   -10,   0,   0,   0,   0,   0,   0, -10,
   -10,   0,   5,   5,   5,   5,   0, -10,
    -5,   0,   5,   5,   5,   5,   0,  -5,
     0,   0,   5,   5,   5,   5,   0,  -5,
   -10,   5,   5,   5,   5,   5,   0, -10,
   -10,   0,   5,   0,   0,   0,   0, -10,
   -20, -10, -10,  -5,  -5, -10, -10, -20,
};
const int KING_MG[64] = {
   -30, -40, -40, -50, -50, -40, -40, -30,
   -30, -40, -40, -50, -50, -40, -40, -30,
   -30, -40, -40, -50, -50, -40, -40, -30,
   -30, -40, -40, -50, -50, -40, -40, -30,
   -20, -30, -30, -40, -40, -30, -30, -20,
   -10, -20, -20, -20, -20, -20, -20, -10,
    15,  15,  -5, -10, -10,  -5,  15,  15,
    20,  30,  10, -10,   0,  -5,  35,  20,
};
const int KING_EG[64] = {
   -50, -40, -30, -20, -20, -30, -40, -50,
   -30, -20, -10,   0,   0, -10, -20, -30,
   -30, -10,  20,  30,  30,  20, -10, -30,
   -30, -10,  30,  40,  40,  30, -10, -30,
   -30, -10,  30,  40,  40,  30, -10, -30,
   -30, -10,  20,  30,  30,  20, -10, -30,
   -30, -30,   0,   0,   0,   0, -30, -30,
   -50, -30, -30, -30, -30, -30, -30, -50,
};
// clang-format on

Score PST[12][64];  // material + piece-square, indexed by actual piece and square

// Mobility is counted in safe squares, centred on a typical count so an average piece scores 0.
const Score MOBILITY_WEIGHT[6] = {{0, 0}, {4, 4}, {5, 5}, {2, 4}, {1, 2}, {0, 0}};
const int MOBILITY_BASE[6] = {0, 4, 6, 7, 13, 0};

const Score BISHOP_PAIR{30, 50};
const Score ROOK_OPEN_FILE{25, 10};
const Score ROOK_SEMI_OPEN_FILE{12, 6};
const Score DOUBLED_PAWN{-10, -20};
const Score ISOLATED_PAWN{-12, -15};
const Score PASSED_PAWN[8] = {{0, 0}, {5, 10}, {8, 15}, {12, 30}, {25, 55}, {45, 95}, {70, 140}, {0, 0}};
const int SHIELD_PAWN = 10;      // per pawn in front of a castled king (middlegame only)
const int KING_OPEN_FILE = -18;  // per open or half-open file next to the king
const int KING_ATTACK_WEIGHT[6] = {0, 2, 2, 3, 5, 0};
const int TEMPO = 10;

int distance(int a, int b) {
    return std::max(std::abs(file_of(a) - file_of(b)), std::abs(rank_of(a) - rank_of(b)));
}

int center_distance(int sq) {
    int f = file_of(sq), r = rank_of(sq);
    return std::max(3 - f, f - 4) + std::max(3 - r, r - 4);
}

// Squares in front of a pawn on its own file and the two neighbours, all the way to promotion.
Bitboard passed_mask(Color c, int sq) {
    Bitboard files = file_bb(file_of(sq)) | east(file_bb(file_of(sq))) | west(file_bb(file_of(sq)));
    Bitboard ahead = 0;
    if (c == WHITE) {
        for (int r = rank_of(sq) + 1; r < 8; ++r) ahead |= rank_bb(r);
    } else {
        for (int r = rank_of(sq) - 1; r >= 0; --r) ahead |= rank_bb(r);
    }
    return files & ahead;
}

Bitboard PASSED_MASK[2][64];
Bitboard ADJACENT_FILES[8];

void init_eval() {
    static bool done = false;
    if (done) return;
    done = true;
    const int* mg_tables[6] = {PAWN_MG, KNIGHT_PST, BISHOP_PST, ROOK_PST, QUEEN_PST, KING_MG};
    const int* eg_tables[6] = {PAWN_EG, KNIGHT_PST, BISHOP_PST, ROOK_PST, QUEEN_PST, KING_EG};
    for (int pt = PAWN; pt <= KING; ++pt) {
        for (int sq = 0; sq < 64; ++sq) {
            // The tables are drawn with rank 8 first, so white's square sq is entry flip_rank(sq).
            Score s = MATERIAL[pt] + Score(mg_tables[pt][flip_rank(sq)], eg_tables[pt][flip_rank(sq)]);
            PST[make_piece(WHITE, PieceType(pt))][sq] = s;
            PST[make_piece(BLACK, PieceType(pt))][flip_rank(sq)] = s;
        }
    }
    for (int sq = 0; sq < 64; ++sq) {
        PASSED_MASK[WHITE][sq] = passed_mask(WHITE, sq);
        PASSED_MASK[BLACK][sq] = passed_mask(BLACK, sq);
    }
    for (int f = 0; f < 8; ++f) ADJACENT_FILES[f] = east(file_bb(f)) | west(file_bb(f));
}

struct Terms {
    Score material_pst[2], mobility[2], pawns[2], pieces[2], king[2], passed[2];
};

void evaluate_side(const Position& pos, Color us, Terms& t) {
    const Color them = ~us;
    const Bitboard occ = pos.occupied();
    const Bitboard own_pawns = pos.pieces(us, PAWN), enemy_pawns = pos.pieces(them, PAWN);

    // Squares the enemy pawns attack are not counted as mobility.
    Bitboard enemy_pawn_attacks = 0;
    for (Bitboard b = enemy_pawns; b;) enemy_pawn_attacks |= PAWN_ATTACKS[them][pop_lsb(b)];
    const Bitboard safe = ~pos.pieces(us) & ~enemy_pawn_attacks;

    const int enemy_king = pos.king_square(them);
    const Bitboard enemy_king_zone = KING_ATTACKS[enemy_king] | bit(enemy_king);
    int attack_units = 0, attackers = 0;

    for (int pt = KNIGHT; pt <= QUEEN; ++pt) {
        for (Bitboard b = pos.pieces(us, PieceType(pt)); b;) {
            int sq = pop_lsb(b);
            Bitboard attacks = attacks_of(PieceType(pt), sq, occ);
            int moves = popcount(attacks & safe);
            t.mobility[us] += MOBILITY_WEIGHT[pt] * (moves - MOBILITY_BASE[pt]);
            if (attacks & enemy_king_zone) {
                ++attackers;
                attack_units += KING_ATTACK_WEIGHT[pt] * popcount(attacks & enemy_king_zone);
            }
            if (pt == ROOK) {
                Bitboard file = file_bb(file_of(sq));
                if (!(file & (own_pawns | enemy_pawns))) t.pieces[us] += ROOK_OPEN_FILE;
                else if (!(file & own_pawns)) t.pieces[us] += ROOK_SEMI_OPEN_FILE;
            }
        }
    }
    if (popcount(pos.pieces(us, BISHOP)) >= 2) t.pieces[us] += BISHOP_PAIR;

    // A lone attacker is rarely dangerous; two or more grow quickly. This lands on the enemy's score.
    if (attackers >= 2 && pos.pieces(us, QUEEN)) {
        int penalty = std::min(attack_units * attack_units * 2, 500);
        t.king[them] -= Score(penalty, 0);
    }

    // Pawn structure.
    for (Bitboard b = own_pawns; b;) {
        int sq = pop_lsb(b);
        int f = file_of(sq);
        if (!(ADJACENT_FILES[f] & own_pawns)) t.pawns[us] += ISOLATED_PAWN;
        if (!(PASSED_MASK[us][sq] & enemy_pawns)) {
            int rr = relative_rank(us, sq);
            Score bonus = PASSED_PAWN[rr];
            // In the endgame, a passer is worth more when our king is near it and theirs is far.
            int stop = us == WHITE ? sq + 8 : sq - 8;
            bonus.eg += (distance(pos.king_square(them), stop) * 5 - distance(pos.king_square(us), stop) * 2) * rr / 2;
            // A blocked passer is much less dangerous.
            if (pos.piece_on(stop) != NO_PIECE) bonus = Score(bonus.mg / 2, bonus.eg / 2);
            t.passed[us] += bonus;
        }
    }
    for (int f = 0; f < 8; ++f) {
        int n = popcount(own_pawns & file_bb(f));
        if (n > 1) t.pawns[us] += DOUBLED_PAWN * (n - 1);
    }

    // King shelter: pawns in front of the king, and open files next to it.
    const int king = pos.king_square(us);
    const int kf = file_of(king);
    if (relative_rank(us, king) <= 1) {
        Bitboard shield_files = file_bb(kf) | ADJACENT_FILES[kf];
        Bitboard shield_ranks = us == WHITE ? rank_bb(rank_of(king) + 1) | rank_bb(rank_of(king) + 2)
                                            : rank_bb(rank_of(king) - 1) | rank_bb(rank_of(king) - 2);
        int shield = popcount(own_pawns & shield_files & shield_ranks);
        t.king[us] += Score(SHIELD_PAWN * std::min(shield, 3), 0);
    }
    for (int f = std::max(0, kf - 1); f <= std::min(7, kf + 1); ++f)
        if (!(file_bb(f) & own_pawns)) t.king[us] += Score(KING_OPEN_FILE, 0);
}

int game_phase(const Position& pos) {
    int phase = 0;
    for (int pt = KNIGHT; pt <= QUEEN; ++pt) phase += PHASE_WEIGHT[pt] * popcount(pos.pieces(PieceType(pt)));
    return std::min(phase, MAX_PHASE);
}

int material_value(const Position& pos, Color c) {
    int v = 0;
    for (int pt = KNIGHT; pt <= QUEEN; ++pt) v += MATERIAL[pt].mg * popcount(pos.pieces(c, PieceType(pt)));
    return v;
}

// Endgames that the raw numbers misjudge. Returns a scale out of 64 for the stronger side's score.
int scale_factor(const Position& pos, Color strong) {
    Color weak = ~strong;
    if (!pos.pieces(strong, PAWN)) {
        int diff = material_value(pos, strong) - material_value(pos, weak);
        // Up only a minor piece (or less) with no pawns: usually impossible to win.
        if (diff <= MATERIAL[BISHOP].mg) return 8;
    }
    // Opposite-coloured bishops with nothing else but pawns are notoriously drawish.
    Bitboard wb = pos.pieces(WHITE, BISHOP), bb = pos.pieces(BLACK, BISHOP);
    const Bitboard DARK = 0xAA55AA55AA55AA55ULL;
    if (popcount(wb) == 1 && popcount(bb) == 1 && !(pos.pieces(KNIGHT) | pos.pieces(ROOK) | pos.pieces(QUEEN)) &&
        bool(wb & DARK) != bool(bb & DARK))
        return 32;
    return 64;
}

struct Breakdown {
    Terms terms;
    int phase = 0, mopup = 0, scale = 64, total = 0;  // total is from white's view
};

Breakdown compute(const Position& pos) {
    init_eval();
    Breakdown out;
    Terms& t = out.terms;
    for (int sq = 0; sq < 64; ++sq) {
        Piece p = pos.piece_on(sq);
        if (p != NO_PIECE) t.material_pst[color_of(p)] += PST[p][sq];
    }
    evaluate_side(pos, WHITE, t);
    evaluate_side(pos, BLACK, t);

    Score white = t.material_pst[WHITE] + t.mobility[WHITE] + t.pawns[WHITE] + t.pieces[WHITE] + t.king[WHITE] + t.passed[WHITE];
    Score black = t.material_pst[BLACK] + t.mobility[BLACK] + t.pawns[BLACK] + t.pieces[BLACK] + t.king[BLACK] + t.passed[BLACK];
    Score s = white - black;

    out.phase = game_phase(pos);
    int score = (s.mg * out.phase + s.eg * (MAX_PHASE - out.phase)) / MAX_PHASE;

    // Mop-up: with a lone enemy king, drive it to the edge and bring our king closer, so that
    // simple wins like K+Q v K or K+R v K are actually converted.
    for (Color strong : {WHITE, BLACK}) {
        Color weak = ~strong;
        if (pos.pieces(weak) == pos.pieces(weak, KING) && material_value(pos, strong) >= MATERIAL[ROOK].mg) {
            int bonus = 10 * center_distance(pos.king_square(weak)) + 6 * (7 - distance(pos.king_square(strong), pos.king_square(weak)));
            out.mopup = strong == WHITE ? bonus : -bonus;
            score += out.mopup;
        }
    }

    Color strong = score >= 0 ? WHITE : BLACK;
    out.scale = scale_factor(pos, strong);
    score = score * out.scale / 64;
    out.total = score;
    return out;
}

}  // namespace

int evaluate(const Position& pos) {
    int white_view = compute(pos).total;
    int score = pos.side_to_move() == WHITE ? white_view : -white_view;
    return score + TEMPO;
}

std::string eval_trace(const Position& pos) {
    Breakdown b = compute(pos);
    const Terms& t = b.terms;
    char line[160];
    std::string out = "Term          |   White mg  eg |   Black mg  eg |  Total mg  eg\n";
    out += "--------------+----------------+----------------+--------------\n";
    auto row = [&](const char* name, Score w, Score bl) {
        std::snprintf(line, sizeof line, "%-13s | %8d %4d | %8d %4d | %7d %4d\n", name, w.mg, w.eg, bl.mg, bl.eg, w.mg - bl.mg, w.eg - bl.eg);
        out += line;
    };
    row("Material+PST", t.material_pst[WHITE], t.material_pst[BLACK]);
    row("Mobility", t.mobility[WHITE], t.mobility[BLACK]);
    row("Pawns", t.pawns[WHITE], t.pawns[BLACK]);
    row("Passed pawns", t.passed[WHITE], t.passed[BLACK]);
    row("Pieces", t.pieces[WHITE], t.pieces[BLACK]);
    row("King safety", t.king[WHITE], t.king[BLACK]);
    std::snprintf(line, sizeof line, "\nPhase %d/24, mop-up %d, scale %d/64\n", b.phase, b.mopup, b.scale);
    out += line;
    std::snprintf(line, sizeof line, "Final: %+.2f (white's view), %+d cp for the side to move incl. tempo\n", b.total / 100.0, evaluate(pos));
    out += line;
    return out;
}

}  // namespace kestrel
