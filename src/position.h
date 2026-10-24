// The board: piece placement, side to move, castling/en passant state, and the move history
// needed to take moves back and to spot repetitions.
#pragma once

#include <string>
#include <vector>

#include "bitboard.h"
#include "types.h"

namespace kestrel {

namespace zobrist {
extern Key psq[12][64];
extern Key castling[16];
extern Key ep_file[8];
extern Key side;
}  // namespace zobrist

// Builds the attack tables and Zobrist keys. Call once at startup, before creating a Position.
void init_tables();

constexpr const char* START_FEN ="rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

// Everything make_move changes that unmake_move cannot work out again on its own.
struct StateInfo {
    Key key = 0;
    int castling = 0;
    int ep = NO_SQ;            // only set when an enemy pawn could actually capture there
    int halfmove = 0;          // plies since the last capture or pawn move (fifty-move rule)
    int plies_from_null = 0;   // repetition checks must not look back past a null move
    Piece captured = NO_PIECE;
    Move move = NO_MOVE;
};

class Position {
public:
    Position();  // the standard starting position

    // Returns false (and leaves the position unchanged) if the FEN is malformed.
    bool set_fen(const std::string& fen, std::string* error = nullptr);
    std::string fen() const;
    std::string pretty() const;  // ASCII diagram for the "d" command

    void make_move(Move m);
    void unmake_move();
    void make_null();
    void unmake_null();

    // After make_move: false if the move left the mover's own king in check.
    bool last_move_legal() const { return !is_attacked(king_square(~stm_), stm_); }

    Piece piece_on(int sq) const { return board_[sq]; }
    Bitboard pieces(Color c, PieceType pt) const { return bb_[make_piece(c, pt)]; }
    Bitboard pieces(PieceType pt) const { return bb_[pt] | bb_[pt + 6]; }
    Bitboard pieces(Color c) const { return by_color_[c]; }
    Bitboard occupied() const { return by_color_[WHITE] | by_color_[BLACK]; }
    Color side_to_move() const { return stm_; }
    int king_square(Color c) const { return lsb(bb_[make_piece(c, KING)]); }

    const StateInfo& state() const { return history_.back(); }
    Key key() const { return state().key; }
    int castling_rights() const { return state().castling; }
    int ep_square() const { return state().ep; }
    int halfmove_clock() const { return state().halfmove; }
    int fullmove_number() const { return fullmove_; }
    Move last_move() const { return state().move; }
    int game_ply() const { return int(history_.size()) - 1; }

    Bitboard attackers_to(int sq, Bitboard occupied) const;
    bool is_attacked(int sq, Color by) const;
    bool in_check() const { return is_attacked(king_square(stm_), ~stm_); }

    // Draw by the fifty-move rule, insufficient material, or repetition. `ply` is the distance
    // from the search root: a single repeat inside the search tree already counts, but a position
    // from the game before the root has to have occurred twice before (threefold repetition).
    bool is_draw(int ply) const;
    bool is_repetition(int ply) const;
    bool insufficient_material() const;
    bool has_non_pawn_material(Color c) const;

    // Static exchange evaluation: the material balance, in centipawns, of the capture sequence
    // that starts with `m` on its target square, assuming both sides recapture sensibly.
    int see(Move m) const;

    Key compute_key() const;  // from scratch; the tests check it against the incremental key

private:
    void put_piece(Piece p, int sq);
    void remove_piece(int sq);
    void move_piece(int from, int to);
    void clear();

    Piece board_[64];
    Bitboard bb_[12];
    Bitboard by_color_[2];
    Color stm_ = WHITE;
    int fullmove_ = 1;
    std::vector<StateInfo> history_;
};

}  // namespace kestrel
