#include "notation.h"

#include <algorithm>

#include "movegen.h"

namespace kestrel {

std::string move_to_san(Position& pos, Move m) {
    std::string san;
    const int from = move_from(m), to = move_to(m);
    const PieceType pt = type_of(pos.piece_on(from));

    if (is_castle(m)) {
        san = move_flag(m) == KING_CASTLE ? "O-O" : "O-O-O";
    } else {
        if (pt == PAWN) {
            if (is_capture(m)) san += char('a' + file_of(from));
        } else {
            san += "PNBRQK"[pt];
            // Disambiguate when another piece of the same kind could also reach the square.
            MoveList legal;
            generate_legal(pos, legal);
            bool clash = false, same_file = false, same_rank = false;
            for (Move other : legal) {
                int ofrom = move_from(other);
                if (other == m || move_to(other) != to || ofrom == from || type_of(pos.piece_on(ofrom)) != pt) continue;
                clash = true;
                same_file |= file_of(ofrom) == file_of(from);
                same_rank |= rank_of(ofrom) == rank_of(from);
            }
            if (clash) {
                if (!same_file) san += char('a' + file_of(from));
                else if (!same_rank) san += char('1' + rank_of(from));
                else san += square_name(from);
            }
        }
        if (is_capture(m)) san += 'x';
        san += square_name(to);
        if (is_promotion(m)) {
            san += '=';
            san += "PNBRQK"[promotion_type(m)];
        }
    }

    pos.make_move(m);
    if (pos.in_check()) {
        MoveList replies;
        generate_legal(pos, replies);
        san += replies.size == 0 ? '#' : '+';
    }
    pos.unmake_move();
    return san;
}

namespace {

std::string strip_decorations(std::string s) {
    s.erase(std::remove_if(s.begin(), s.end(), [](char c) { return c == '+' || c == '#' || c == '!' || c == '?'; }), s.end());
    // Accept 0-0 as well as O-O, and "e8Q" as well as "e8=Q".
    for (char& c : s)
        if (c == '0') c = 'O';
    return s;
}

}  // namespace

Move parse_move(Position& pos, const std::string& text) {
    MoveList legal;
    generate_legal(pos, legal);
    for (Move m : legal)
        if (move_to_uci(m) == text) return m;

    std::string want = strip_decorations(text);
    Move found = NO_MOVE;
    for (Move m : legal) {
        std::string san = strip_decorations(move_to_san(pos, m));
        std::string loose = san;
        loose.erase(std::remove(loose.begin(), loose.end(), '='), loose.end());
        if (san == want || loose == want) {
            if (found != NO_MOVE) return NO_MOVE;  // ambiguous
            found = m;
        }
    }
    return found;
}

GameResult game_result(Position& pos, std::string* reason) {
    auto set = [&](const char* why) {
        if (reason) *reason = why;
    };
    MoveList legal;
    generate_legal(pos, legal);
    if (legal.size == 0) {
        if (pos.in_check()) {
            set("checkmate");
            return pos.side_to_move() == WHITE ? GameResult::BLACK_WINS : GameResult::WHITE_WINS;
        }
        set("stalemate");
        return GameResult::DRAW;
    }
    if (pos.halfmove_clock() >= 100) {
        set("fifty-move rule");
        return GameResult::DRAW;
    }
    if (pos.insufficient_material()) {
        set("insufficient material");
        return GameResult::DRAW;
    }
    // ply = 0 means only real repetitions in the game count: threefold.
    if (pos.is_repetition(0)) {
        set("threefold repetition");
        return GameResult::DRAW;
    }
    return GameResult::ONGOING;
}

}  // namespace kestrel
