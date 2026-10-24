// Static evaluation: a hand-tuned, tapered score that blends middlegame and endgame terms by how
// much material is left on the board.
#pragma once

#include <string>

#include "position.h"

namespace kestrel {

// Centipawns from the side to move's point of view.
int evaluate(const Position& pos);

// Per-term breakdown for the "eval" command, from white's point of view.
std::string eval_trace(const Position& pos);

}  // namespace kestrel
