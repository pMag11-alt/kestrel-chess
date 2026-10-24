#include "search.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "eval.h"
#include "movegen.h"

namespace kestrel {

namespace {

int LMR[64][64];  // late move reduction by [depth][move number]

// Victim values for move ordering (most valuable victim first).
const int ORDER_VALUE[7] = {100, 320, 330, 500, 900, 0, 0};
constexpr int HISTORY_MAX = 16384;

constexpr int SCORE_TT_MOVE = 4'000'000;
constexpr int SCORE_GOOD_CAPTURE = 2'000'000;
constexpr int SCORE_KILLER_1 = 1'000'000;
constexpr int SCORE_KILLER_2 = 900'000;
constexpr int SCORE_COUNTER = 800'000;
constexpr int SCORE_BAD_CAPTURE = -2'000'000;
constexpr int SCORE_UNDERPROMOTION = -3'000'000;

void init_lmr() {
    static bool done = false;
    if (done) return;
    done = true;
    for (int d = 1; d < 64; ++d)
        for (int m = 1; m < 64; ++m) LMR[d][m] = int(0.75 + std::log(d) * std::log(m) / 2.25);
}

// "History gravity": large bonuses saturate instead of growing without bound.
void add_history(int& entry, int bonus) { entry += bonus - entry * std::abs(bonus) / HISTORY_MAX; }

// Selection sort, one step at a time: the rest of the list is often never needed after a cutoff.
void pick_best(Move* moves, int* scores, int start, int count) {
    int best = start;
    for (int i = start + 1; i < count; ++i)
        if (scores[i] > scores[best]) best = i;
    std::swap(moves[start], moves[best]);
    std::swap(scores[start], scores[best]);
}

PieceType captured_type(const Position& pos, Move m) {
    if (move_flag(m) == EP_CAPTURE) return PAWN;
    if (!is_capture(m)) return NO_PIECE_TYPE;
    return type_of(pos.piece_on(move_to(m)));
}

}  // namespace

Searcher::Searcher(size_t hash_mb) : tt_(hash_mb) {
    init_lmr();
    new_game();
}

void Searcher::new_game() {
    tt_.clear();
    std::memset(killers_, 0, sizeof killers_);
    std::memset(counter_moves_, 0, sizeof counter_moves_);
    std::memset(history_, 0, sizeof history_);
}

int64_t Searcher::elapsed_ms() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_).count();
}

void Searcher::check_limits() {
    if (limits_.nodes && nodes_ >= limits_.nodes) aborted_ = true;
    if (hard_limit_ms_ && elapsed_ms() >= hard_limit_ms_) aborted_ = true;
}

bool Searcher::aborted() {
    if (!aborted_ && stop_requested()) aborted_ = true;
    return aborted_;
}

void Searcher::score_moves(const Position& pos, const Move* moves, int count, int* scores, Move tt_move, int ply) const {
    const Color us = pos.side_to_move();
    Move counter = NO_MOVE;
    if (Move prev = pos.last_move(); prev != NO_MOVE) counter = counter_moves_[pos.piece_on(move_to(prev))][move_to(prev)];

    for (int i = 0; i < count; ++i) {
        const Move m = moves[i];
        if (m == tt_move) {
            scores[i] = SCORE_TT_MOVE;
        } else if (is_promotion(m) && promotion_type(m) != QUEEN) {
            scores[i] = SCORE_UNDERPROMOTION;
        } else if (!is_quiet(m)) {
            // MVV-LVA: take the most valuable victim with the least valuable attacker first.
            PieceType victim = captured_type(pos, m);
            int mvv_lva = (victim == NO_PIECE_TYPE ? 0 : ORDER_VALUE[victim]) * 8 - type_of(pos.piece_on(move_from(m)));
            if (is_promotion(m)) mvv_lva += ORDER_VALUE[QUEEN];
            scores[i] = (pos.see(m) >= 0 ? SCORE_GOOD_CAPTURE : SCORE_BAD_CAPTURE) + mvv_lva;
        } else if (m == killers_[ply][0]) {
            scores[i] = SCORE_KILLER_1;
        } else if (m == killers_[ply][1]) {
            scores[i] = SCORE_KILLER_2;
        } else if (m == counter) {
            scores[i] = SCORE_COUNTER;
        } else {
            scores[i] = history_[us][move_from(m)][move_to(m)];
        }
    }
}

void Searcher::update_quiet_stats(const Position& pos, Move best, const Move* quiets, int quiet_count, int depth, int ply) {
    const Color us = pos.side_to_move();
    const int bonus = std::min(1536, 20 * depth * depth);
    if (killers_[ply][0] != best) {
        killers_[ply][1] = killers_[ply][0];
        killers_[ply][0] = best;
    }
    add_history(history_[us][move_from(best)][move_to(best)], bonus);
    // The quiet moves tried before it failed to cut off, so they are made a little less attractive.
    for (int i = 0; i < quiet_count; ++i) add_history(history_[us][move_from(quiets[i])][move_to(quiets[i])], -bonus);
    if (Move prev = pos.last_move(); prev != NO_MOVE) counter_moves_[pos.piece_on(move_to(prev))][move_to(prev)] = best;
}

int Searcher::quiescence(Position& pos, int alpha, int beta, int ply) {
    pv_length_[ply] = ply;
    if ((++nodes_ & 2047) == 0) check_limits();
    if (aborted()) return 0;
    seldepth_ = std::max(seldepth_, ply);
    if (pos.is_draw(ply)) return 0;

    const bool in_check = pos.in_check();
    if (ply >= MAX_PLY) return in_check ? 0 : evaluate(pos);

    const TTEntry* tte = tt_.probe(pos.key());
    if (tte && beta - alpha == 1) {
        int s = score_from_tt(tte->score, ply);
        if (tte->bound == BOUND_EXACT || (tte->bound == BOUND_LOWER && s >= beta) || (tte->bound == BOUND_UPPER && s <= alpha))
            return s;
    }

    // "Stand pat": the side to move can usually do at least as well as the static eval by not capturing.
    // In check there is no such option, so every evasion is searched.
    int best = -INF, stand_pat = 0;
    if (!in_check) {
        stand_pat = tte ? tte->eval : evaluate(pos);
        if (stand_pat >= beta) return stand_pat;
        alpha = std::max(alpha, stand_pat);
        best = stand_pat;
    }

    MoveList list;
    generate(pos, list, in_check ? GenType::ALL : GenType::NOISY);
    int scores[256];
    score_moves(pos, list.moves, list.size, scores, tte ? tte->move : NO_MOVE, ply);

    int legal = 0;
    for (int i = 0; i < list.size; ++i) {
        pick_best(list.moves, scores, i, list.size);
        const Move m = list.moves[i];
        if (!in_check) {
            PieceType victim = captured_type(pos, m);
            int gain = (victim == NO_PIECE_TYPE ? 0 : ORDER_VALUE[victim]) + (is_promotion(m) ? ORDER_VALUE[QUEEN] - ORDER_VALUE[PAWN] : 0);
            if (stand_pat + gain + 200 <= alpha) continue;  // delta pruning: hopeless even if it wins the piece
            if (pos.see(m) < 0) continue;                    // loses material on the exchange
        }
        pos.make_move(m);
        if (!pos.last_move_legal()) {
            pos.unmake_move();
            continue;
        }
        ++legal;
        int score = -quiescence(pos, -beta, -alpha, ply + 1);
        pos.unmake_move();
        if (aborted()) return 0;

        if (score > best) {
            best = score;
            if (score > alpha) {
                alpha = score;
                pv_[ply][ply] = m;
                for (int j = ply + 1; j < pv_length_[ply + 1]; ++j) pv_[ply][j] = pv_[ply + 1][j];
                pv_length_[ply] = std::max(ply + 1, pv_length_[ply + 1]);
                if (score >= beta) break;
            }
        }
    }
    if (in_check && legal == 0) return -MATE + ply;
    return best;
}

int Searcher::negamax(Position& pos, int alpha, int beta, int depth, int ply, bool null_allowed) {
    const bool pv_node = beta - alpha > 1;
    const bool root = ply == 0;
    pv_length_[ply] = ply;

    if (!root) {
        if (pos.is_draw(ply)) return 0;
        // Mate distance pruning: no line from here can beat a mate already found closer to the root.
        alpha = std::max(alpha, -MATE + ply);
        beta = std::min(beta, MATE - ply - 1);
        if (alpha >= beta) return alpha;
    }

    const bool in_check = pos.in_check();
    if (in_check) ++depth;  // check extension: never stop searching while in check
    if (depth <= 0) return quiescence(pos, alpha, beta, ply);
    if (ply >= MAX_PLY) return in_check ? 0 : evaluate(pos);

    if ((++nodes_ & 2047) == 0) check_limits();
    if (aborted()) return 0;
    seldepth_ = std::max(seldepth_, ply);

    const TTEntry* tte = tt_.probe(pos.key());
    const Move tt_move = tte ? tte->move : NO_MOVE;
    if (tte && !pv_node && tte->depth >= depth) {
        int s = score_from_tt(tte->score, ply);
        if (tte->bound == BOUND_EXACT || (tte->bound == BOUND_LOWER && s >= beta) || (tte->bound == BOUND_UPPER && s <= alpha))
            return s;
    }

    const int static_eval = in_check ? -INF : (tte ? tte->eval : evaluate(pos));
    static_eval_[ply] = static_eval;
    // Is our position getting better than it was a full move ago? If so, prune less.
    const bool improving = !in_check && ply >= 2 && static_eval > static_eval_[ply - 2];

    if (!pv_node && !in_check && !root) {
        // Reverse futility pruning: far enough above beta that a shallow search won't change that.
        int margin = 80 * depth - (improving ? 40 : 0);
        if (depth <= 7 && static_eval - margin >= beta && std::abs(beta) < MATE_BOUND) return static_eval;

        // Null-move pruning: if passing still fails high, a real move almost certainly would too.
        // Skipped without pieces, where zugzwang makes passing a bad model.
        if (null_allowed && depth >= 3 && static_eval >= beta && pos.has_non_pawn_material(pos.side_to_move())) {
            int r = 3 + depth / 4 + std::min(3, (static_eval - beta) / 200);
            pos.make_null();
            int score = -negamax(pos, -beta, -beta + 1, depth - 1 - r, ply + 1, false);
            pos.unmake_null();
            if (aborted()) return 0;
            if (score >= beta) return score >= MATE_BOUND ? beta : score;
        }
    }

    // Internal iterative reduction: with no hash move the ordering is poor, so search a ply shallower.
    if (depth >= 4 && tt_move == NO_MOVE && !in_check) --depth;

    MoveList list;
    generate(pos, list);
    int scores[256];
    score_moves(pos, list.moves, list.size, scores, tt_move, ply);

    const Color us = pos.side_to_move();
    Move quiets[64];
    int quiet_count = 0;
    int best = -INF, legal = 0;
    Move best_move = NO_MOVE;

    for (int i = 0; i < list.size; ++i) {
        pick_best(list.moves, scores, i, list.size);
        const Move m = list.moves[i];
        const bool quiet = is_quiet(m);

        // Shallow pruning, only once a real score is in hand so a mate can never be missed.
        if (!root && !in_check && best > -MATE_BOUND) {
            if (quiet) {
                if (depth <= 4 && legal >= (4 + depth * depth) / (improving ? 1 : 2)) continue;  // late move pruning
                if (depth <= 5 && static_eval + 100 + 80 * depth <= alpha) continue;              // futility pruning
            } else if (depth <= 5 && pos.see(m) < -90 * depth) {
                continue;  // a capture that loses too much material at low depth
            }
        }

        pos.make_move(m);
        if (!pos.last_move_legal()) {
            pos.unmake_move();
            continue;
        }
        ++legal;
        const bool gives_check = pos.in_check();
        const int new_depth = depth - 1;
        int score;

        if (legal == 1) {
            score = -negamax(pos, -beta, -alpha, new_depth, ply + 1, true);
        } else {
            // Late move reductions: later quiet moves are probably worse, so try them shallower first.
            int r = 0;
            if (depth >= 3 && quiet) {
                r = LMR[std::min(depth, 63)][std::min(legal, 63)];
                r += !pv_node;
                r += !improving;
                r -= gives_check;
                r -= (m == killers_[ply][0] || m == killers_[ply][1]);
                r -= history_[us][move_from(m)][move_to(m)] / 8000;
                r = std::clamp(r, 0, new_depth - 1);
            }
            // Principal variation search: prove the move is no better with a null window, and only
            // re-search with the full window when it might be.
            score = -negamax(pos, -alpha - 1, -alpha, new_depth - r, ply + 1, true);
            if (score > alpha && r > 0) score = -negamax(pos, -alpha - 1, -alpha, new_depth, ply + 1, true);
            if (score > alpha && score < beta) score = -negamax(pos, -beta, -alpha, new_depth, ply + 1, true);
        }
        pos.unmake_move();
        if (aborted()) return 0;

        if (score > best) {
            best = score;
            if (score > alpha) {
                alpha = score;
                best_move = m;
                pv_[ply][ply] = m;
                for (int j = ply + 1; j < pv_length_[ply + 1]; ++j) pv_[ply][j] = pv_[ply + 1][j];
                pv_length_[ply] = std::max(ply + 1, pv_length_[ply + 1]);
                if (score >= beta) {
                    if (quiet) update_quiet_stats(pos, m, quiets, quiet_count, depth, ply);
                    break;
                }
            }
        }
        if (quiet && quiet_count < 64) quiets[quiet_count++] = m;
    }

    if (legal == 0) return in_check ? -MATE + ply : 0;  // checkmate or stalemate

    Bound bound = best >= beta ? BOUND_LOWER : best_move != NO_MOVE ? BOUND_EXACT : BOUND_UPPER;
    tt_.store(pos.key(), score_to_tt(best, ply), in_check ? 0 : static_eval, best_move, depth, bound);
    return best;
}

SearchResult Searcher::search(Position& pos, const SearchLimits& limits,
                              const std::function<void(const SearchInfo&)>& on_iteration) {
    limits_ = limits;
    start_ = std::chrono::steady_clock::now();
    nodes_ = 0;
    aborted_ = false;
    tt_.new_search();
    std::memset(killers_, 0, sizeof killers_);
    for (auto& side : history_)
        for (auto& from : side)
            for (int& h : from) h /= 2;  // older history is less relevant, but still a good hint

    // Time management: aim to use a slice of the remaining clock, with a hard ceiling for safety.
    const Color us = pos.side_to_move();
    soft_limit_ms_ = hard_limit_ms_ = 0;
    if (limits.movetime > 0) {
        soft_limit_ms_ = hard_limit_ms_ = std::max<int64_t>(1, limits.movetime - limits.move_overhead);
    } else if (limits.time[us] > 0 && !limits.infinite) {
        int64_t available = std::max<int64_t>(1, limits.time[us] - limits.move_overhead);
        int moves_left = limits.movestogo > 0 ? std::min(limits.movestogo, 40) : 30;
        int64_t soft = available / moves_left + limits.inc[us] * 3 / 4;
        soft_limit_ms_ = std::max<int64_t>(1, std::min(soft, available * 3 / 5));
        hard_limit_ms_ = std::max<int64_t>(1, std::min(soft_limit_ms_ * 4, available * 4 / 5));
    }

    SearchResult result;
    MoveList root_moves;
    generate_legal(pos, root_moves);
    if (root_moves.size == 0) return result;  // checkmate or stalemate: nothing to play
    result.best_move = root_moves.moves[0];   // a legal fallback in case time runs out at once

    int score = 0;
    for (int depth = 1; depth <= limits.depth && depth < MAX_PLY; ++depth) {
        seldepth_ = 0;
        // Aspiration windows: expect a score near the last one, and widen the window on a miss.
        int window = 25;
        int alpha = -INF, beta = INF;
        if (depth >= 5) {
            alpha = std::max(-INF, score - window);
            beta = std::min(INF, score + window);
        }
        int s;
        while (true) {
            s = negamax(pos, alpha, beta, depth, 0, false);
            if (aborted()) break;
            if (s <= alpha) {
                beta = (alpha + beta) / 2;
                alpha = std::max(-INF, s - window);
            } else if (s >= beta) {
                beta = std::min(INF, s + window);
            } else {
                break;
            }
            window *= 2;
        }
        if (aborted()) break;  // an unfinished iteration can't be trusted

        score = s;
        result.best_move = pv_[0][0];
        result.ponder_move = pv_length_[0] > 1 ? pv_[0][1] : NO_MOVE;
        result.score = score;
        result.depth = depth;

        if (on_iteration) {
            SearchInfo info;
            info.depth = depth;
            info.seldepth = seldepth_;
            info.score = score;
            info.nodes = nodes_;
            info.time_ms = elapsed_ms();
            info.hashfull = tt_.hashfull();
            info.pv.assign(pv_[0], pv_[0] + pv_length_[0]);
            on_iteration(info);
        }

        if (limits.nodes && nodes_ >= limits.nodes) break;
        if (!limits.infinite && soft_limit_ms_) {
            // The next iteration usually takes longer than all the previous ones together.
            if (elapsed_ms() >= soft_limit_ms_ * 6 / 10) break;
            if (root_moves.size == 1 && depth >= 4) break;  // forced move: no need to think
        }
    }
    result.nodes = nodes_;
    return result;
}

uint64_t run_bench(int depth, bool verbose) {
    static const char* BENCH_FENS[] = {
        START_FEN,
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
        "r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 2 3",
        "r1bq1rk1/pp2bppp/2n1pn2/3p4/2PP4/2N1PN2/PP2BPPP/R1BQ1RK1 w - - 0 9",
        "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
        "4rrk1/pp1n3p/3q2pQ/2p1pb2/2PP4/2P3N1/P2B2PP/4RRK1 b - - 7 19",
        "r3r1k1/2p2ppp/p1p1bn2/8/1q2P3/2NPQN2/PPP3PP/R4RK1 b - - 2 15",
        "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
        "6k1/5p1p/6p1/8/1P6/P4P2/5KPP/8 w - - 0 40",
        "8/8/8/4k3/8/8/2Q5/4K3 w - - 0 1",
    };
    Searcher searcher(16);
    SearchLimits limits;
    limits.depth = depth;
    uint64_t total = 0;
    auto start = std::chrono::steady_clock::now();
    for (const char* fen : BENCH_FENS) {
        Position pos;
        pos.set_fen(fen);
        searcher.new_game();
        SearchResult r = searcher.search(pos, limits);
        total += r.nodes;
        if (verbose)
            std::printf("%-72s %-6s %10llu nodes\n", fen, move_to_uci(r.best_move).c_str(), static_cast<unsigned long long>(r.nodes));
    }
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("\nNodes searched: %llu\nNodes/second: %llu\nTime: %.2fs\n", static_cast<unsigned long long>(total),
                static_cast<unsigned long long>(total / std::max(seconds, 1e-3)), seconds);
    return total;
}

}  // namespace kestrel
