#include "movegen.h"

namespace kestrel {

bool MoveList::contains(Move m) const {
    for (Move x : *this)
        if (x == m) return true;
    return false;
}

namespace {

void add_promotions(MoveList& list, int from, int to, bool capture, GenType type) {
    int base = capture ? PROMO_CAPTURE_N : PROMO_N;
    list.add(make_move(from, to, base + 3));  // queen first: it is almost always the right choice
    if (type == GenType::ALL || capture) {
        list.add(make_move(from, to, base + 0));
        list.add(make_move(from, to, base + 1));
        list.add(make_move(from, to, base + 2));
    }
}

void generate_pawn_moves(const Position& pos, MoveList& list, GenType type) {
    const Color us = pos.side_to_move(), them = ~us;
    const Bitboard pawns = pos.pieces(us, PAWN);
    const Bitboard enemies = pos.pieces(them);
    const Bitboard empty = ~pos.occupied();
    const Bitboard promo_rank = us == WHITE ? RANK_8 : RANK_1;
    const Bitboard third_rank = us == WHITE ? rank_bb(2) : rank_bb(5);
    const int up = us == WHITE ? 8 : -8;

    // Pushes.
    Bitboard single = push(us, pawns) & empty;
    Bitboard promos = single & promo_rank;
    for (Bitboard b = promos; b;) {
        int to = pop_lsb(b);
        add_promotions(list, to - up, to, false, type);
    }
    if (type == GenType::ALL) {
        for (Bitboard b = single & ~promo_rank; b;) {
            int to = pop_lsb(b);
            list.add(make_move(to - up, to, QUIET));
        }
        for (Bitboard b = push(us, single & third_rank) & empty; b;) {
            int to = pop_lsb(b);
            list.add(make_move(to - 2 * up, to, DOUBLE_PUSH));
        }
    }

    // Captures, including en passant.
    for (Bitboard b = pawns; b;) {
        int from = pop_lsb(b);
        Bitboard targets = PAWN_ATTACKS[us][from] & enemies;
        while (targets) {
            int to = pop_lsb(targets);
            if (bit(to) & promo_rank) add_promotions(list, from, to, true, type);
            else list.add(make_move(from, to, CAPTURE));
        }
        if (pos.ep_square() != NO_SQ && (PAWN_ATTACKS[us][from] & bit(pos.ep_square())))
            list.add(make_move(from, pos.ep_square(), EP_CAPTURE));
    }
}

void generate_castling(const Position& pos, MoveList& list) {
    const Color us = pos.side_to_move(), them = ~us;
    const int rights = pos.castling_rights();
    const Bitboard occ = pos.occupied();
    const int king = us == WHITE ? E1 : E8;
    if (!(rights & (us == WHITE ? WHITE_OO | WHITE_OOO : BLACK_OO | BLACK_OOO))) return;
    if (pos.is_attacked(king, them)) return;  // no castling out of check

    // The king may not pass through an attacked square; landing on one is caught by the legality test.
    if ((rights & (us == WHITE ? WHITE_OO : BLACK_OO)) && !(occ & (bit(king + 1) | bit(king + 2))) &&
        !pos.is_attacked(king + 1, them))
        list.add(make_move(king, king + 2, KING_CASTLE));
    if ((rights & (us == WHITE ? WHITE_OOO : BLACK_OOO)) &&
        !(occ & (bit(king - 1) | bit(king - 2) | bit(king - 3))) && !pos.is_attacked(king - 1, them))
        list.add(make_move(king, king - 2, QUEEN_CASTLE));
}

}  // namespace

void generate(const Position& pos, MoveList& list, GenType type) {
    const Color us = pos.side_to_move();
    const Bitboard own = pos.pieces(us), enemies = pos.pieces(~us), occ = pos.occupied();
    const Bitboard targets = type == GenType::ALL ? ~own : enemies;

    generate_pawn_moves(pos, list, type);
    for (int pt = KNIGHT; pt <= KING; ++pt) {
        for (Bitboard pieces = pos.pieces(us, PieceType(pt)); pieces;) {
            int from = pop_lsb(pieces);
            Bitboard moves = attacks_of(PieceType(pt), from, occ) & targets;
            while (moves) {
                int to = pop_lsb(moves);
                list.add(make_move(from, to, (enemies & bit(to)) ? CAPTURE : QUIET));
            }
        }
    }
    if (type == GenType::ALL) generate_castling(pos, list);
}

void generate_legal(Position& pos, MoveList& list) {
    MoveList pseudo;
    generate(pos, pseudo);
    for (Move m : pseudo) {
        pos.make_move(m);
        if (pos.last_move_legal()) list.add(m);
        pos.unmake_move();
    }
}

uint64_t perft(Position& pos, int depth) {
    if (depth == 0) return 1;
    MoveList pseudo;
    generate(pos, pseudo);
    uint64_t nodes = 0;
    for (Move m : pseudo) {
        pos.make_move(m);
        if (pos.last_move_legal()) nodes += depth == 1 ? 1 : perft(pos, depth - 1);
        pos.unmake_move();
    }
    return nodes;
}

}  // namespace kestrel
