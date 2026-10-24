// Standard Algebraic Notation (Nf3, exd5, O-O, e8=Q+) for the interactive mode.
#pragma once

#include <string>

#include "position.h"

namespace kestrel {

std::string move_to_san(Position& pos, Move m);

// Accepts either UCI ("g1f3", "e7e8q") or SAN ("Nf3", "e8=Q"). Returns NO_MOVE if the text
// doesn't describe exactly one legal move.
Move parse_move(Position& pos, const std::string& text);

enum class GameResult { ONGOING, WHITE_WINS, BLACK_WINS, DRAW };

// Decides whether the game is over, filling `reason` with e.g. "checkmate" or "threefold repetition".
GameResult game_result(Position& pos, std::string* reason = nullptr);

}  // namespace kestrel
