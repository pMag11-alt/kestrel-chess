// Transposition table: a hash map from position key to what an earlier search learned about it.
#pragma once

#include <cstddef>
#include <vector>

#include "types.h"

namespace kestrel {

enum Bound : uint8_t { BOUND_NONE, BOUND_UPPER, BOUND_LOWER, BOUND_EXACT };

struct TTEntry {
    Key key = 0;
    int16_t score = 0;
    int16_t eval = 0;
    Move move = NO_MOVE;
    uint8_t depth = 0;
    uint8_t bound = BOUND_NONE;
};

class TranspositionTable {
public:
    explicit TranspositionTable(size_t mb = 16) { resize(mb); }

    void resize(size_t mb);
    void clear();
    void new_search() { ++generation_; }

    // Returns the entry for this key, or nullptr if the slot holds a different position.
    const TTEntry* probe(Key key) const {
        const TTEntry& e = table_[key & mask_];
        return e.key == key && e.bound != BOUND_NONE ? &e : nullptr;
    }

    void store(Key key, int score, int eval, Move move, int depth, Bound bound);

    // Permille of a sample of slots written during the current search, for "info hashfull".
    int hashfull() const;
    size_t size_mb() const { return table_.size() * sizeof(TTEntry) / (1024 * 1024); }

private:
    std::vector<TTEntry> table_;
    std::vector<uint8_t> age_;  // generation that last wrote each slot
    size_t mask_ = 0;
    uint8_t generation_ = 0;
};

// Mate scores are stored relative to the node rather than the root, so a mate found through
// one path is still scored correctly when the position is reached at a different ply.
inline int score_to_tt(int score, int ply) {
    if (score >= MATE_BOUND) return score + ply;
    if (score <= -MATE_BOUND) return score - ply;
    return score;
}

inline int score_from_tt(int score, int ply) {
    if (score >= MATE_BOUND) return score - ply;
    if (score <= -MATE_BOUND) return score + ply;
    return score;
}

}  // namespace kestrel
