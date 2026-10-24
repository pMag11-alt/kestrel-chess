#include "position.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace kestrel {

namespace zobrist {
Key psq[12][64];
Key castling[16];
Key ep_file[8];
Key side;
}  // namespace zobrist

namespace {

const char PIECE_CHARS[] = "PNBRQKpnbrqk";

// Castling rights that survive a move touching each square: moving the king or a rook,
// or capturing a rook on its home square, removes the matching rights.
int CASTLE_MASK[64];

const int SEE_VALUE[7] = {100, 320, 330, 500, 900, 20000, 0};

void init_zobrist() {
    uint64_t s = 0x4B657374726C21ULL;  // "Kestrl!"
    auto next = [&s] {
        // splitmix64
        uint64_t z = (s += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    };
    for (auto& piece : zobrist::psq)
        for (Key& k : piece) k = next();
    for (Key& k : zobrist::castling) k = next();
    for (Key& k : zobrist::ep_file) k = next();
    zobrist::side = next();

    for (int& m : CASTLE_MASK) m = ALL_CASTLING;
    CASTLE_MASK[E1] = ~(WHITE_OO | WHITE_OOO) & ALL_CASTLING;
    CASTLE_MASK[H1] = ~WHITE_OO & ALL_CASTLING;
    CASTLE_MASK[A1] = ~WHITE_OOO & ALL_CASTLING;
    CASTLE_MASK[E8] = ~(BLACK_OO | BLACK_OOO) & ALL_CASTLING;
    CASTLE_MASK[H8] = ~BLACK_OO & ALL_CASTLING;
    CASTLE_MASK[A8] = ~BLACK_OOO & ALL_CASTLING;
}

}  // namespace

void init_tables() {
    static bool done = false;
    if (done) return;
    done = true;
    init_bitboards();
    init_zobrist();
}

Position::Position() {
    init_tables();
    clear();
    set_fen(START_FEN);
}

void Position::clear() {
    std::fill(std::begin(board_), std::end(board_), NO_PIECE);
    std::fill(std::begin(bb_), std::end(bb_), 0);
    by_color_[WHITE] = by_color_[BLACK] = 0;
    stm_ = WHITE;
    fullmove_ = 1;
    history_.clear();
}

void Position::put_piece(Piece p, int sq) {
    board_[sq] = p;
    bb_[p] |= bit(sq);
    by_color_[color_of(p)] |= bit(sq);
}

void Position::remove_piece(int sq) {
    Piece p = board_[sq];
    bb_[p] &= ~bit(sq);
    by_color_[color_of(p)] &= ~bit(sq);
    board_[sq] = NO_PIECE;
}

void Position::move_piece(int from, int to) {
    Piece p = board_[from];
    Bitboard both = bit(from) | bit(to);
    bb_[p] ^= both;
    by_color_[color_of(p)] ^= both;
    board_[from] = NO_PIECE;
    board_[to] = p;
}

bool Position::set_fen(const std::string& fen, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };

    std::istringstream in(fen);
    std::string placement, side, castling, ep, halfmove = "0", fullmove = "1";
    if (!(in >> placement >> side)) return fail("expected at least piece placement and side to move");
    if (!(in >> castling)) castling = "-";
    if (!(in >> ep)) ep = "-";
    in >> halfmove >> fullmove;

    Position next(*this);
    next.clear();

    int rank = 7, file = 0;
    for (char c : placement) {
        if (c == '/') {
            if (file != 8 || rank == 0) return fail("each rank must describe exactly 8 squares");
            --rank;
            file = 0;
        } else if (c >= '1' && c <= '8') {
            file += c - '0';
            if (file > 8) return fail("rank " + std::to_string(rank + 1) + " is too long");
        } else {
            const char* found = std::strchr(PIECE_CHARS, c);
            if (!found || c == '\0') return fail(std::string("unknown piece '") + c + "'");
            if (file > 7) return fail("rank " + std::to_string(rank + 1) + " is too long");
            next.put_piece(Piece(found - PIECE_CHARS), make_square(file, rank));
            ++file;
        }
    }
    if (rank != 0 || file != 8) return fail("piece placement must describe 8 ranks of 8 squares");
    if (popcount(next.bb_[W_KING]) != 1 || popcount(next.bb_[B_KING]) != 1)
        return fail("each side needs exactly one king");
    if ((next.bb_[W_PAWN] | next.bb_[B_PAWN]) & (RANK_1 | RANK_8)) return fail("pawns cannot stand on the first or last rank");

    if (side == "w") next.stm_ = WHITE;
    else if (side == "b") next.stm_ = BLACK;
    else return fail("side to move must be 'w' or 'b'");

    StateInfo st;
    if (castling != "-") {
        for (char c : castling) {
            switch (c) {
                case 'K': st.castling |= WHITE_OO; break;
                case 'Q': st.castling |= WHITE_OOO; break;
                case 'k': st.castling |= BLACK_OO; break;
                case 'q': st.castling |= BLACK_OOO; break;
                default: return fail(std::string("bad castling character '") + c + "'");
            }
        }
    }
    // Quietly drop rights that the piece placement makes impossible.
    if (next.board_[E1] != W_KING) st.castling &= ~(WHITE_OO | WHITE_OOO);
    if (next.board_[H1] != W_ROOK) st.castling &= ~WHITE_OO;
    if (next.board_[A1] != W_ROOK) st.castling &= ~WHITE_OOO;
    if (next.board_[E8] != B_KING) st.castling &= ~(BLACK_OO | BLACK_OOO);
    if (next.board_[H8] != B_ROOK) st.castling &= ~BLACK_OO;
    if (next.board_[A8] != B_ROOK) st.castling &= ~BLACK_OOO;

    if (ep != "-") {
        int sq = parse_square(ep);
        if (sq == NO_SQ || rank_of(sq) != (next.stm_ == WHITE ? 5 : 2)) return fail("bad en passant square '" + ep + "'");
        // Keep it only if a pawn can actually take, so equal positions always hash equally.
        Color us = next.stm_;
        if (PAWN_ATTACKS[~us][sq] & next.pieces(us, PAWN)) st.ep = sq;
    }

    try {
        st.halfmove = std::max(0, std::stoi(halfmove));
        next.fullmove_ = std::max(1, std::stoi(fullmove));
    } catch (...) {
        return fail("move counters must be numbers");
    }

    next.history_.push_back(st);
    next.history_.back().key = next.compute_key();

    if (next.is_attacked(next.king_square(~next.stm_), next.stm_)) return fail("the side not to move is in check");

    *this = next;
    return true;
}

std::string Position::fen() const {
    std::string s;
    for (int rank = 7; rank >= 0; --rank) {
        int empty = 0;
        for (int file = 0; file < 8; ++file) {
            Piece p = board_[make_square(file, rank)];
            if (p == NO_PIECE) {
                ++empty;
                continue;
            }
            if (empty) s += char('0' + empty);
            empty = 0;
            s += PIECE_CHARS[p];
        }
        if (empty) s += char('0' + empty);
        if (rank) s += '/';
    }
    s += stm_ == WHITE ? " w " : " b ";
    int cr = castling_rights();
    if (!cr) s += '-';
    if (cr & WHITE_OO) s += 'K';
    if (cr & WHITE_OOO) s += 'Q';
    if (cr & BLACK_OO) s += 'k';
    if (cr & BLACK_OOO) s += 'q';
    s += ' ' + square_name(ep_square());
    s += ' ' + std::to_string(halfmove_clock()) + ' ' + std::to_string(fullmove_);
    return s;
}

std::string Position::pretty() const {
    std::ostringstream out;
    out << "   +---+---+---+---+---+---+---+---+\n";
    for (int rank = 7; rank >= 0; --rank) {
        out << ' ' << rank + 1 << " |";
        for (int file = 0; file < 8; ++file) {
            Piece p = board_[make_square(file, rank)];
            out << ' ' << (p == NO_PIECE ? ((file + rank) % 2 ? ' ' : '.') : PIECE_CHARS[p]) << " |";
        }
        out << "\n   +---+---+---+---+---+---+---+---+\n";
    }
    out << "     a   b   c   d   e   f   g   h\n\n";
    out << "FEN: " << fen() << "\n";
    char hex[17];
    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(key()));
    out << "Key: " << hex << "\n";
    return out.str();
}

Key Position::compute_key() const {
    Key k = 0;
    for (int sq = 0; sq < 64; ++sq)
        if (board_[sq] != NO_PIECE) k ^= zobrist::psq[board_[sq]][sq];
    k ^= zobrist::castling[castling_rights()];
    if (ep_square() != NO_SQ) k ^= zobrist::ep_file[file_of(ep_square())];
    if (stm_ == BLACK) k ^= zobrist::side;
    return k;
}

void Position::make_move(Move m) {
    StateInfo st = history_.back();
    const int from = move_from(m), to = move_to(m), flag = move_flag(m);
    const Color us = stm_, them = ~stm_;
    const Piece piece = board_[from];
    Key k = st.key ^ zobrist::side;

    st.move = m;
    st.captured = NO_PIECE;
    ++st.halfmove;
    ++st.plies_from_null;
    if (st.ep != NO_SQ) {
        k ^= zobrist::ep_file[file_of(st.ep)];
        st.ep = NO_SQ;
    }

    if (flag == EP_CAPTURE) {
        int cap_sq = us == WHITE ? to - 8 : to + 8;
        st.captured = board_[cap_sq];
        remove_piece(cap_sq);
        k ^= zobrist::psq[st.captured][cap_sq];
    } else if (is_capture(m)) {
        st.captured = board_[to];
        remove_piece(to);
        k ^= zobrist::psq[st.captured][to];
    }
    if (st.captured != NO_PIECE) st.halfmove = 0;

    move_piece(from, to);
    k ^= zobrist::psq[piece][from] ^ zobrist::psq[piece][to];

    if (type_of(piece) == PAWN) {
        st.halfmove = 0;
        if (flag == DOUBLE_PUSH) {
            int ep = (from + to) / 2;
            if (PAWN_ATTACKS[us][ep] & pieces(them, PAWN)) {
                st.ep = ep;
                k ^= zobrist::ep_file[file_of(ep)];
            }
        } else if (is_promotion(m)) {
            Piece promoted = make_piece(us, promotion_type(m));
            remove_piece(to);
            put_piece(promoted, to);
            k ^= zobrist::psq[piece][to] ^ zobrist::psq[promoted][to];
        }
    } else if (is_castle(m)) {
        int rook_from = flag == KING_CASTLE ? to + 1 : to - 2;
        int rook_to = flag == KING_CASTLE ? to - 1 : to + 1;
        Piece rook = board_[rook_from];
        move_piece(rook_from, rook_to);
        k ^= zobrist::psq[rook][rook_from] ^ zobrist::psq[rook][rook_to];
    }

    k ^= zobrist::castling[st.castling];
    st.castling &= CASTLE_MASK[from] & CASTLE_MASK[to];
    k ^= zobrist::castling[st.castling];

    if (us == BLACK) ++fullmove_;
    stm_ = them;
    st.key = k;
    history_.push_back(st);
}

void Position::unmake_move() {
    const StateInfo& st = history_.back();
    const Move m = st.move;
    const int from = move_from(m), to = move_to(m), flag = move_flag(m);
    stm_ = ~stm_;
    const Color us = stm_;
    if (us == BLACK) --fullmove_;

    if (is_castle(m)) {
        int rook_from = flag == KING_CASTLE ? to + 1 : to - 2;
        int rook_to = flag == KING_CASTLE ? to - 1 : to + 1;
        move_piece(rook_to, rook_from);
    }
    if (is_promotion(m)) {
        remove_piece(to);
        put_piece(make_piece(us, PAWN), to);
    }
    move_piece(to, from);
    if (st.captured != NO_PIECE) {
        int cap_sq = flag == EP_CAPTURE ? (us == WHITE ? to - 8 : to + 8) : to;
        put_piece(st.captured, cap_sq);
    }
    history_.pop_back();
}

void Position::make_null() {
    StateInfo st = history_.back();
    st.key ^= zobrist::side;
    if (st.ep != NO_SQ) {
        st.key ^= zobrist::ep_file[file_of(st.ep)];
        st.ep = NO_SQ;
    }
    st.move = NO_MOVE;
    st.captured = NO_PIECE;
    ++st.halfmove;
    st.plies_from_null = 0;
    stm_ = ~stm_;
    history_.push_back(st);
}

void Position::unmake_null() {
    stm_ = ~stm_;
    history_.pop_back();
}

Bitboard Position::attackers_to(int sq, Bitboard occ) const {
    return (PAWN_ATTACKS[BLACK][sq] & bb_[W_PAWN]) | (PAWN_ATTACKS[WHITE][sq] & bb_[B_PAWN]) |
           (KNIGHT_ATTACKS[sq] & pieces(KNIGHT)) | (KING_ATTACKS[sq] & pieces(KING)) |
           (bishop_attacks(sq, occ) & (pieces(BISHOP) | pieces(QUEEN))) |
           (rook_attacks(sq, occ) & (pieces(ROOK) | pieces(QUEEN)));
}

bool Position::is_attacked(int sq, Color by) const {
    Bitboard occ = occupied();
    return (PAWN_ATTACKS[~by][sq] & pieces(by, PAWN)) || (KNIGHT_ATTACKS[sq] & pieces(by, KNIGHT)) ||
           (KING_ATTACKS[sq] & pieces(by, KING)) ||
           (bishop_attacks(sq, occ) & (pieces(by, BISHOP) | pieces(by, QUEEN))) ||
           (rook_attacks(sq, occ) & (pieces(by, ROOK) | pieces(by, QUEEN)));
}

bool Position::has_non_pawn_material(Color c) const {
    return pieces(c) & ~pieces(c, PAWN) & ~pieces(c, KING);
}

bool Position::insufficient_material() const {
    if (pieces(PAWN) | pieces(ROOK) | pieces(QUEEN)) return false;
    // Only kings and minor pieces left: K v K, K+minor v K, and K+B v K+B with same-coloured bishops.
    Bitboard minors = pieces(KNIGHT) | pieces(BISHOP);
    if (popcount(minors) <= 1) return true;
    const Bitboard DARK = 0xAA55AA55AA55AA55ULL;
    Bitboard bishops = pieces(BISHOP);
    if (!pieces(KNIGHT) && ((bishops & DARK) == bishops || (bishops & ~DARK) == bishops)) return true;
    return false;
}

bool Position::is_repetition(int ply) const {
    const StateInfo& st = history_.back();
    int last = int(history_.size()) - 1;
    int end = std::min({st.halfmove, st.plies_from_null, last});
    int seen = 0;
    for (int back = 4; back <= end; back += 2) {
        if (history_[last - back].key == st.key) {
            if (back <= ply) return true;  // repeated inside the search tree
            if (++seen == 2) return true;  // threefold repetition in the game itself
        }
    }
    return false;
}

bool Position::is_draw(int ply) const {
    if (halfmove_clock() >= 100) {
        // Checkmate on the hundredth ply still wins, so only call it a draw when not in check
        // (the search handles the mated case before it gets here).
        if (!in_check()) return true;
    }
    return insufficient_material() || is_repetition(ply);
}

int Position::see(Move m) const {
    if (is_castle(m)) return 0;
    const int from = move_from(m), to = move_to(m);
    int gain[32];
    int depth = 0;

    Bitboard occ = occupied();
    Bitboard from_set = bit(from);
    PieceType attacker = type_of(board_[from]);
    gain[0] = move_flag(m) == EP_CAPTURE ? SEE_VALUE[PAWN] : (board_[to] == NO_PIECE ? 0 : SEE_VALUE[type_of(board_[to])]);
    if (is_promotion(m)) {
        gain[0] += SEE_VALUE[promotion_type(m)] - SEE_VALUE[PAWN];
        attacker = promotion_type(m);
    }
    if (move_flag(m) == EP_CAPTURE) occ ^= bit(stm_ == WHITE ? to - 8 : to + 8);

    Bitboard attackers = attackers_to(to, occ);
    const Bitboard diagonal = pieces(BISHOP) | pieces(QUEEN);
    const Bitboard straight = pieces(ROOK) | pieces(QUEEN);
    Color side = stm_;

    do {
        ++depth;
        gain[depth] = SEE_VALUE[attacker] - gain[depth - 1];
        if (std::max(-gain[depth - 1], gain[depth]) < 0) break;  // neither side wants to continue
        attackers &= ~from_set;
        occ &= ~from_set;
        // Removing a piece may reveal a slider behind it.
        attackers |= (bishop_attacks(to, occ) & diagonal) | (rook_attacks(to, occ) & straight);
        attackers &= occ;
        side = ~side;

        from_set = 0;
        for (int pt = PAWN; pt <= KING; ++pt) {
            Bitboard candidates = attackers & pieces(side, PieceType(pt));
            if (candidates) {
                from_set = candidates & (0 - candidates);
                attacker = PieceType(pt);
                break;
            }
        }
    } while (from_set && depth < 31);

    while (--depth) gain[depth - 1] = -std::max(-gain[depth - 1], gain[depth]);
    return gain[0];
}

}  // namespace kestrel
