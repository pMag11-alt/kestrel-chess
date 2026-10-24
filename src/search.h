// Iterative-deepening alpha-beta search with a transposition table, quiescence search and the
// usual selectivity: null-move pruning, late move reductions, futility and SEE pruning.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <vector>

#include "position.h"
#include "tt.h"

namespace kestrel {

struct SearchLimits {
    int depth = MAX_PLY - 1;
    int64_t movetime = 0;         // fixed milliseconds for this move
    int64_t time[2] = {0, 0};     // remaining clock per side, milliseconds
    int64_t inc[2] = {0, 0};
    int movestogo = 0;
    uint64_t nodes = 0;           // 0 = no node limit
    bool infinite = false;
    int64_t move_overhead = 30;   // milliseconds kept back for GUI/network lag
};

// One completed iteration, as reported in a UCI "info" line.
struct SearchInfo {
    int depth = 0;
    int seldepth = 0;
    int score = 0;         // centipawns, or +-(MATE - plies) for a forced mate
    uint64_t nodes = 0;
    int64_t time_ms = 0;
    int hashfull = 0;
    std::vector<Move> pv;
};

struct SearchResult {
    Move best_move = NO_MOVE;
    Move ponder_move = NO_MOVE;
    int score = 0;
    int depth = 0;
    uint64_t nodes = 0;
};

class Searcher {
public:
    explicit Searcher(size_t hash_mb = 16);

    void set_hash_size(size_t mb) { tt_.resize(mb); }
    size_t hash_size() const { return tt_.size_mb(); }
    void new_game();  // forget everything learned from earlier positions

    SearchResult search(Position& pos, const SearchLimits& limits,
                        const std::function<void(const SearchInfo&)>& on_iteration = nullptr);

    // Ask a running search to stop as soon as possible. Safe to call from another thread.
    void stop() { stop_.store(true, std::memory_order_relaxed); }
    // Re-arm after a stop. Done by whoever starts the search, before it starts, so that a stop
    // arriving straight after "go" can never be lost.
    void clear_stop() { stop_.store(false, std::memory_order_relaxed); }
    bool stop_requested() const { return stop_.load(std::memory_order_relaxed); }

private:
    int negamax(Position& pos, int alpha, int beta, int depth, int ply, bool null_allowed);
    int quiescence(Position& pos, int alpha, int beta, int ply);
    void check_limits();
    bool aborted();
    void score_moves(const Position& pos, const Move* moves, int count, int* scores, Move tt_move, int ply) const;
    void update_quiet_stats(const Position& pos, Move best, const Move* quiets, int quiet_count, int depth, int ply);
    int64_t elapsed_ms() const;

    TranspositionTable tt_;
    std::atomic<bool> stop_{false};  // stop requested from outside (the UCI "stop" command)
    bool aborted_ = false;           // this search is over: stopped, or out of time or nodes

    Move killers_[MAX_PLY][2];
    Move counter_moves_[12][64];
    int history_[2][64][64];
    Move pv_[MAX_PLY + 1][MAX_PLY + 1];
    int pv_length_[MAX_PLY + 1];
    int static_eval_[MAX_PLY + 1];

    uint64_t nodes_ = 0;
    int seldepth_ = 0;
    SearchLimits limits_;
    std::chrono::steady_clock::time_point start_;
    int64_t soft_limit_ms_ = 0;  // don't start another iteration after this
    int64_t hard_limit_ms_ = 0;  // abort the search at this point
};

// Fixed-depth search over a set of positions; prints the total node count and speed. The node
// count is deterministic, so it doubles as a quick "did the search change?" signature.
uint64_t run_bench(int depth, bool verbose);

}  // namespace kestrel
