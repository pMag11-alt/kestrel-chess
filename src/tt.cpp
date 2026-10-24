#include "tt.h"

#include <algorithm>

namespace kestrel {

void TranspositionTable::resize(size_t mb) {
    // Round down to a power of two so a key maps to a slot with a mask instead of a modulo.
    size_t entries = std::max<size_t>(1, mb) * 1024 * 1024 / sizeof(TTEntry);
    size_t pow2 = 1;
    while (pow2 * 2 <= entries) pow2 *= 2;
    table_.assign(pow2, TTEntry{});
    age_.assign(pow2, 0);
    mask_ = pow2 - 1;
    generation_ = 0;
}

void TranspositionTable::clear() {
    std::fill(table_.begin(), table_.end(), TTEntry{});
    std::fill(age_.begin(), age_.end(), 0);
    generation_ = 0;
}

void TranspositionTable::store(Key key, int score, int eval, Move move, int depth, Bound bound) {
    size_t i = key & mask_;
    TTEntry& e = table_[i];
    // Keep a deeper result for the same position from this search, unless the new one is exact.
    // Entries left over from earlier searches, or for other positions, are always replaced.
    bool same = e.key == key;
    if (same && age_[i] == generation_ && bound != BOUND_EXACT && depth + 2 < e.depth) return;
    if (move == NO_MOVE && same) move = e.move;  // don't lose a good move to a fail-low
    e.key = key;
    e.score = int16_t(score);
    e.eval = int16_t(eval);
    e.move = move;
    e.depth = uint8_t(std::max(0, depth));
    e.bound = bound;
    age_[i] = generation_;
}

int TranspositionTable::hashfull() const {
    int used = 0;
    size_t sample = std::min<size_t>(1000, table_.size());
    for (size_t i = 0; i < sample; ++i)
        if (table_[i].bound != BOUND_NONE && age_[i] == generation_) ++used;
    return int(used * 1000 / sample);
}

}  // namespace kestrel
