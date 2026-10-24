// Unit tests for Kestrel. No framework: each TEST registers itself, and CHECK records failures
// without stopping, so one run reports everything that is wrong.
//
// Set KESTREL_QUICK_TESTS=1 to use smaller perft depths (for the slow sanitizer build).

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "bitboard.h"
#include "eval.h"
#include "movegen.h"
#include "notation.h"
#include "position.h"
#include "search.h"
#include "tt.h"
#include "uci.h"

using namespace kestrel;

namespace {

int g_checks = 0, g_failures = 0;
const char* g_current = "";
bool g_quick = false;

struct TestCase {
    const char* name;
    void (*fn)();
};

std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

template <typename A, typename B>
void check_eq(const A& actual, const B& expected, const char* expr, const char* file, int line) {
    ++g_checks;
    if (actual == expected) return;
    ++g_failures;
    std::ostringstream out;
    out << "  FAIL [" << g_current << "] " << file << ":" << line << ": " << expr << "\n"
        << "       got:      " << actual << "\n       expected: " << expected << "\n";
    std::cout << out.str();
}

#define TEST(name)                                     \
    void name();                                       \
    const Registrar registrar_##name(#name, name);     \
    void name()

#define CHECK(cond)                                                                                 \
    do {                                                                                            \
        ++g_checks;                                                                                 \
        if (!(cond)) {                                                                              \
            ++g_failures;                                                                           \
            std::printf("  FAIL [%s] %s:%d: %s\n", g_current, __FILE__, __LINE__, #cond);           \
        }                                                                                           \
    } while (0)

#define CHECK_EQ(actual, expected) check_eq((actual), (expected), #actual " == " #expected, __FILE__, __LINE__)

Position from_fen(const std::string& fen) {
    Position pos;
    std::string error;
    if (!pos.set_fen(fen, &error)) {
        ++g_failures;
        std::printf("  FAIL [%s] bad test FEN '%s': %s\n", g_current, fen.c_str(), error.c_str());
    }
    return pos;
}

Move uci(Position& pos, const std::string& text) {
    Move m = parse_move(pos, text);
    if (m == NO_MOVE) {
        ++g_failures;
        std::printf("  FAIL [%s] '%s' is not legal in %s\n", g_current, text.c_str(), pos.fen().c_str());
    }
    return m;
}

void play(Position& pos, const std::string& moves) {
    std::istringstream in(moves);
    std::string m;
    while (in >> m) pos.make_move(uci(pos, m));
}

SearchResult search_depth(Position& pos, int depth) {
    Searcher searcher(8);
    SearchLimits limits;
    limits.depth = depth;
    return searcher.search(pos, limits);
}

// The same position with the colours swapped and the board turned upside down.
std::string mirror_fen(const std::string& fen) {
    std::istringstream in(fen);
    std::string placement, side, castling, ep, half, full;
    in >> placement >> side >> castling >> ep >> half >> full;
    std::vector<std::string> ranks;
    std::stringstream ps(placement);
    std::string rank;
    while (std::getline(ps, rank, '/')) ranks.push_back(rank);
    std::string out;
    for (int i = int(ranks.size()) - 1; i >= 0; --i) {
        for (char c : ranks[i]) out += std::isupper(static_cast<unsigned char>(c)) ? char(std::tolower(c)) : char(std::toupper(c));
        if (i) out += '/';
    }
    std::string cr;
    for (char c : std::string("KQkq")) {
        char swapped = std::isupper(static_cast<unsigned char>(c)) ? char(std::tolower(c)) : char(std::toupper(c));
        if (castling.find(swapped) != std::string::npos) cr += c;
    }
    if (cr.empty()) cr = "-";
    if (ep != "-") ep = std::string{ep[0], ep[1] == '3' ? '6' : '3'};
    return out + (side == "w" ? " b " : " w ") + cr + " " + ep + " " + half + " " + full;
}

// Independent brute-force check, with no pruning: can the side to move force mate in n moves?
bool replies_all_lose(Position& pos, int n);
bool can_force_mate(Position& pos, int n) {
    MoveList moves;
    generate_legal(pos, moves);
    for (Move m : moves) {
        pos.make_move(m);
        bool mates = replies_all_lose(pos, n);
        pos.unmake_move();
        if (mates) return true;
    }
    return false;
}
bool replies_all_lose(Position& pos, int n) {
    MoveList replies;
    generate_legal(pos, replies);
    if (replies.size == 0) return pos.in_check();
    if (n == 1) return false;
    for (Move r : replies) {
        pos.make_move(r);
        bool mated = can_force_mate(pos, n - 1);
        pos.unmake_move();
        if (!mated) return false;
    }
    return true;
}

const char* PERFT_FENS[] = {
    START_FEN,
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
    "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
    "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
    "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
    "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
};

// ------------------------------------------------------------------------------------------------
// Board and move generation

TEST(magic_attacks_match_ray_walking) {
    std::mt19937_64 rng(42);
    for (int trial = 0; trial < 2000; ++trial) {
        Bitboard occ = rng() & rng();  // about a quarter of the squares occupied
        for (int sq = 0; sq < 64; ++sq) {
            CHECK_EQ(rook_attacks(sq, occ), sliding_attacks_slow(ROOK, sq, occ));
            CHECK_EQ(bishop_attacks(sq, occ), sliding_attacks_slow(BISHOP, sq, occ));
        }
    }
}

TEST(between_and_line_tables) {
    CHECK_EQ(BETWEEN[A1][D4], bit(B2) | bit(C3));
    CHECK_EQ(BETWEEN[E1][E8], bit(E2) | bit(E3) | bit(E4) | bit(E5) | bit(E6) | bit(E7));
    CHECK_EQ(BETWEEN[A1][B3], Bitboard(0));  // not aligned
    CHECK(LINE[C3][E5] & bit(H8));
    CHECK(LINE[C3][E5] & bit(A1));
}

TEST(fen_round_trip) {
    const char* fens[] = {
        START_FEN,
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
        "rnbqkbnr/ppp1p1pp/8/3pPp2/8/8/PPPP1PPP/RNBQKBNR w KQkq f6 0 3",
        "8/8/8/8/8/8/6k1/4K2R b K - 12 57",
        "4k3/8/8/8/8/8/8/4K3 w - - 99 120",
    };
    for (const char* fen : fens) CHECK_EQ(from_fen(fen).fen(), std::string(fen));
}

TEST(fen_drops_impossible_rights_and_useless_en_passant) {
    // No rook on h1, so white can't castle king-side; no white pawn can take on d6.
    Position pos = from_fen("4k3/8/8/3p4/8/8/8/R3K3 w KQ d6 0 1");
    CHECK_EQ(pos.fen(), std::string("4k3/8/8/3p4/8/8/8/R3K3 w Q - 0 1"));
}

TEST(fen_rejects_malformed_input) {
    const char* bad[] = {
        "",
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP w KQkq - 0 1",        // 7 ranks
        "rnbqkbnr/pppppppp/9/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", // 9 squares
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR x KQkq - 0 1", // side
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQ1BNR w kq - 0 1",   // no white king
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNX w KQkq - 0 1", // unknown piece
        "4k3/8/8/8/8/8/8/P3K3 w - - 0 1",                          // pawn on rank 1
        "4k3/4Q3/8/8/8/8/8/4K3 w - - 0 1",                          // black in check, white to move
        "4k3/8/8/8/8/8/8/4K3 w - e9 0 1",                           // bad en passant square
    };
    for (const char* fen : bad) {
        Position pos;
        std::string error;
        CHECK(!pos.set_fen(fen, &error));
        CHECK(!error.empty());
        CHECK_EQ(pos.fen(), std::string(START_FEN));  // unchanged on failure
    }
}

TEST(perft_reference_positions) {
    // Published totals from the Chess Programming Wiki's perft results page.
    const uint64_t expected[6][6] = {
        {20, 400, 8902, 197281, 4865609, 119060324},
        {48, 2039, 97862, 4085603, 193690690, 0},
        {14, 191, 2812, 43238, 674624, 11030083},
        {6, 264, 9467, 422333, 15833292, 0},
        {44, 1486, 62379, 2103487, 89941194, 0},
        {46, 2079, 89890, 3894594, 164075551, 0},
    };
    const int depth_limit[6] = {5, 4, 6, 5, 4, 4};
    for (int i = 0; i < 6; ++i) {
        Position pos = from_fen(PERFT_FENS[i]);
        int max_depth = g_quick ? std::min(depth_limit[i], 3) : depth_limit[i];
        for (int d = 1; d <= max_depth; ++d) CHECK_EQ(perft(pos, d), expected[i][d - 1]);
    }
}

TEST(perft_special_rules) {
    // Small positions that each stress one rule: en passant discovering check, castling into or
    // through check, promotions, stalemate. Totals cross-checked against python-chess.
    struct Case {
        const char* fen;
        int depth;
        uint64_t nodes;
    } cases[] = {
        {"3k4/3p4/8/K1P4r/8/8/8/8 b - - 0 1", 6, 1134888},     // illegal en passant (pinned)
        {"8/8/4k3/8/2p5/8/B2P2K1/8 w - - 0 1", 6, 1015133},     // illegal en passant (bishop)
        {"8/8/1k6/2b5/2pP4/8/5K2/8 b - d3 0 1", 6, 1440467},    // en passant that gives check
        {"5k2/8/8/8/8/8/8/4K2R w K - 0 1", 6, 661072},          // short castling gives check
        {"3k4/8/8/8/8/8/8/R3K3 w Q - 0 1", 6, 803711},          // long castling gives check
        {"r3k2r/1b4bq/8/8/8/8/7B/R3K2R w KQkq - 0 1", 4, 1274206},  // castling rights
        {"r3k2r/8/3Q4/8/8/5q2/8/R3K2R b KQkq - 0 1", 4, 1720476},   // castling prevented
        {"2K2r2/4P3/8/8/8/8/8/3k4 w - - 0 1", 6, 3821001},      // promote out of check
        {"8/8/1P2K3/8/2n5/1q6/8/5k2 b - - 0 1", 5, 1004658},    // discovered check
        {"4k3/1P6/8/8/8/8/K7/8 w - - 0 1", 6, 217342},          // promote to give check
        {"8/P1k5/K7/8/8/8/8/8 w - - 0 1", 6, 92683},            // under-promote to give check
        {"K1k5/8/P7/8/8/8/8/8 w - - 0 1", 6, 2217},             // self stalemate
        {"8/k1P5/8/1K6/8/8/8/8 w - - 0 1", 7, 567584},          // stalemate and checkmate
        {"8/8/2k5/5q2/5n2/8/5K2/8 b - - 0 1", 4, 23527},        // checkmate
    };
    for (const Case& c : cases) {
        if (g_quick && c.nodes > 1000000) continue;
        Position pos = from_fen(c.fen);
        CHECK_EQ(perft(pos, c.depth), c.nodes);
    }
}

// Walks the move tree checking that make/unmake leaves no trace and the incremental hash
// always equals one computed from scratch.
void walk_tree(Position& pos, int depth) {
    CHECK_EQ(pos.key(), pos.compute_key());
    if (depth == 0) return;
    const std::string before = pos.fen();
    const Key key = pos.key();
    MoveList moves;
    generate_legal(pos, moves);
    for (Move m : moves) {
        pos.make_move(m);
        walk_tree(pos, depth - 1);
        pos.unmake_move();
        if (pos.fen() != before || pos.key() != key) {
            CHECK_EQ(pos.fen(), before);
            return;
        }
    }
}

TEST(make_unmake_restores_position_and_hash) {
    for (const char* fen : PERFT_FENS) {
        Position pos = from_fen(fen);
        walk_tree(pos, g_quick ? 2 : 3);
    }
}

TEST(transpositions_hash_equal) {
    Position a, b;
    play(a, "g1f3 g8f6 b1c3 b8c6");
    play(b, "b1c3 b8c6 g1f3 g8f6");
    CHECK_EQ(a.key(), b.key());
    // Same placement, different side to move: different hash.
    Position c = from_fen("4k3/8/8/8/8/8/8/4K3 w - - 0 1"), d = from_fen("4k3/8/8/8/8/8/8/4K3 b - - 0 1");
    CHECK(c.key() != d.key());
}

TEST(null_move_round_trip) {
    Position pos = from_fen("rnbqkbnr/ppp1p1pp/8/3pPp2/8/8/PPPP1PPP/RNBQKBNR w KQkq f6 0 3");
    const std::string fen = pos.fen();
    const Key key = pos.key();
    pos.make_null();
    CHECK(pos.side_to_move() == BLACK);
    CHECK_EQ(pos.ep_square(), int(NO_SQ));  // the en passant chance is gone after a pass
    CHECK_EQ(pos.key(), pos.compute_key());
    pos.unmake_null();
    CHECK_EQ(pos.fen(), fen);
    CHECK_EQ(pos.key(), key);
}

TEST(castling_rights_update) {
    Position pos = from_fen("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
    play(pos, "h1h8");  // rook takes rook, with check: white loses K, black loses k
    CHECK_EQ(pos.castling_rights(), int(WHITE_OOO | BLACK_OOO));
    play(pos, "e8e7");  // the king moves: black loses q too
    CHECK_EQ(pos.castling_rights(), int(WHITE_OOO));
    Position castled = from_fen("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
    play(castled, "e1g1");
    CHECK(castled.piece_on(G1) == W_KING && castled.piece_on(F1) == W_ROOK && castled.piece_on(H1) == NO_PIECE);
    play(castled, "e8c8");
    CHECK(castled.piece_on(C8) == B_KING && castled.piece_on(D8) == B_ROOK && castled.piece_on(A8) == NO_PIECE);
}

// ------------------------------------------------------------------------------------------------
// Notation

TEST(uci_move_parsing) {
    Position pos;
    CHECK_EQ(move_to_uci(uci(pos, "e2e4")), std::string("e2e4"));
    CHECK(parse_move(pos, "e2e5") == NO_MOVE);
    CHECK(parse_move(pos, "e7e5") == NO_MOVE);  // not white's piece
    CHECK(parse_move(pos, "zz") == NO_MOVE);
    Position promo = from_fen("8/4P3/8/8/8/8/k7/4K3 w - - 0 1");
    Move m = uci(promo, "e7e8n");
    CHECK(is_promotion(m) && promotion_type(m) == KNIGHT);
}

TEST(san_output) {
    Position pos;
    CHECK_EQ(move_to_san(pos, uci(pos, "g1f3")), std::string("Nf3"));
    CHECK_EQ(move_to_san(pos, uci(pos, "e2e4")), std::string("e4"));

    Position knights = from_fen("4k3/8/8/8/8/2N3N1/8/4K3 w - - 0 1");
    CHECK_EQ(move_to_san(knights, uci(knights, "c3e4")), std::string("Nce4"));
    CHECK_EQ(move_to_san(knights, uci(knights, "g3e4")), std::string("Nge4"));

    Position rooks = from_fen("4k3/8/8/R7/8/8/8/R3K3 w - - 0 1");
    CHECK_EQ(move_to_san(rooks, uci(rooks, "a1a3")), std::string("R1a3"));
    CHECK_EQ(move_to_san(rooks, uci(rooks, "a5a3")), std::string("R5a3"));

    // Three queens reach d4: a4 shares a file with a1 and a rank with h4, so it needs the full square.
    Position queens = from_fen("6k1/8/8/8/Q6Q/8/8/Q3K3 w - - 0 1");
    CHECK_EQ(move_to_san(queens, uci(queens, "a4d4")), std::string("Qa4d4"));
    CHECK_EQ(move_to_san(queens, uci(queens, "h4d4")), std::string("Qhd4"));

    Position castle = from_fen("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
    CHECK_EQ(move_to_san(castle, uci(castle, "e1g1")), std::string("O-O"));
    CHECK_EQ(move_to_san(castle, uci(castle, "e1c1")), std::string("O-O-O"));

    Position promo = from_fen("4k3/P7/8/8/8/8/8/4K3 w - - 0 1");
    CHECK_EQ(move_to_san(promo, uci(promo, "a7a8q")), std::string("a8=Q+"));

    Position mate = from_fen("6k1/5ppp/8/8/8/8/8/R5K1 w - - 0 1");
    CHECK_EQ(move_to_san(mate, uci(mate, "a1a8")), std::string("Ra8#"));

    Position ep = from_fen("rnbqkbnr/ppp1p1pp/8/3pPp2/8/8/PPPP1PPP/RNBQKBNR w KQkq f6 0 3");
    CHECK_EQ(move_to_san(ep, uci(ep, "e5f6")), std::string("exf6"));
}

TEST(san_input) {
    Position pos;
    CHECK_EQ(move_to_uci(parse_move(pos, "Nf3")), std::string("g1f3"));
    CHECK_EQ(move_to_uci(parse_move(pos, "e4")), std::string("e2e4"));
    CHECK(parse_move(pos, "Ke2") == NO_MOVE);
    Position knights = from_fen("4k3/8/8/8/8/2N3N1/8/4K3 w - - 0 1");
    CHECK(parse_move(knights, "Ne4") == NO_MOVE);  // ambiguous
    CHECK_EQ(move_to_uci(parse_move(knights, "Nge4")), std::string("g3e4"));
    Position castle = from_fen("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
    CHECK_EQ(move_to_uci(parse_move(castle, "O-O")), std::string("e1g1"));
    CHECK_EQ(move_to_uci(parse_move(castle, "0-0-0")), std::string("e1c1"));
    Position promo = from_fen("4k3/P7/8/8/8/8/8/4K3 w - - 0 1");
    CHECK_EQ(move_to_uci(parse_move(promo, "a8=N")), std::string("a7a8n"));
    CHECK_EQ(move_to_uci(parse_move(promo, "a8Q+")), std::string("a7a8q"));
}

TEST(game_results) {
    std::string reason;
    Position mated = from_fen("R5k1/5ppp/8/8/8/8/8/6K1 b - - 1 1");
    CHECK(game_result(mated, &reason) == GameResult::WHITE_WINS);
    CHECK_EQ(reason, std::string("checkmate"));

    Position stalemate = from_fen("k7/2Q5/1K6/8/8/8/8/8 b - - 0 1");
    CHECK(game_result(stalemate, &reason) == GameResult::DRAW);
    CHECK_EQ(reason, std::string("stalemate"));

    Position bare = from_fen("8/8/4k3/8/8/2N5/4K3/8 w - - 0 1");
    CHECK(game_result(bare, &reason) == GameResult::DRAW);
    CHECK_EQ(reason, std::string("insufficient material"));

    Position fifty = from_fen("4k3/8/8/8/8/8/R7/4K3 w - - 100 80");
    CHECK(game_result(fifty, &reason) == GameResult::DRAW);

    Position shuffle;
    play(shuffle, "g1f3 g8f6 f3g1 f6g8 g1f3 g8f6 f3g1");
    CHECK(game_result(shuffle) == GameResult::ONGOING);  // the start position has occurred twice
    play(shuffle, "f6g8");
    CHECK(game_result(shuffle, &reason) == GameResult::DRAW);  // and now three times
    CHECK_EQ(reason, std::string("threefold repetition"));
}

TEST(insufficient_material_cases) {
    CHECK(from_fen("8/8/4k3/8/8/8/4K3/8 w - - 0 1").insufficient_material());
    CHECK(from_fen("8/8/4k3/8/8/2B5/4K3/8 w - - 0 1").insufficient_material());
    CHECK(from_fen("8/2b5/4k3/8/8/2B5/4K3/8 w - - 0 1").insufficient_material());   // same-coloured bishops
    CHECK(!from_fen("8/3b4/4k3/8/8/2B5/4K3/8 w - - 0 1").insufficient_material());  // opposite colours
    CHECK(!from_fen("8/8/4k3/8/8/2BN4/4K3/8 w - - 0 1").insufficient_material());
    CHECK(!from_fen("8/8/4k3/8/8/2P5/4K3/8 w - - 0 1").insufficient_material());
}

// ------------------------------------------------------------------------------------------------
// Static exchange evaluation

TEST(see_values) {
    // Pawn takes an undefended knight.
    Position a = from_fen("4k3/8/8/3n4/4P3/8/8/4K3 w - - 0 1");
    CHECK_EQ(a.see(uci(a, "e4d5")), 320);
    // Queen takes a pawn defended by a pawn.
    Position b = from_fen("4k3/8/2p5/3p4/8/8/8/3QK3 w - - 0 1");
    CHECK_EQ(b.see(uci(b, "d1d5")), 100 - 900);
    // Rook takes rook; a bishop recaptures; our queen, behind our rook, takes the bishop (x-ray).
    Position c = from_fen("4k3/1b6/8/3r4/8/8/3R4/3QK3 w - - 0 1");
    CHECK_EQ(c.see(uci(c, "d2d5")), 500 - 500 + 330);
    // A quiet move onto a square a pawn attacks just loses the piece.
    Position d = from_fen("4k3/8/8/8/1p6/8/8/1N2K3 w - - 0 1");
    CHECK_EQ(d.see(uci(d, "b1a3")), -320);
    CHECK_EQ(d.see(uci(d, "b1d2")), 0);
    // The king may recapture a pawn on e3...
    Position e = from_fen("8/8/8/4k3/3p4/4P3/5K2/8 b - - 0 1");
    CHECK_EQ(e.see(uci(e, "d4e3")), 0);
    // ...but not when the pawn move uncovers a bishop that also hits e3.
    Position f = from_fen("8/8/8/2b1k3/3p4/4P3/5K2/8 b - - 0 1");
    CHECK_EQ(f.see(uci(f, "d4e3")), 100);
}

// ------------------------------------------------------------------------------------------------
// Evaluation

TEST(eval_is_colour_symmetric) {
    const char* fens[] = {
        START_FEN,
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
        "r1bq1rk1/pp2bppp/2n1pn2/3p4/2PP4/2N1PN2/PP2BPPP/R1BQ1RK1 w - - 0 9",
        "4rrk1/pp1n3p/3q2pQ/2p1pb2/2PP4/2P3N1/P2B2PP/4RRK1 b - - 7 19",
        "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
        "6k1/5p1p/6p1/8/1P6/P4P2/5KPP/8 w - - 0 40",
        "8/8/8/4k3/8/8/2Q5/4K3 w - - 0 1",
        "2r3k1/p4p2/3Rp2p/1p2P1pK/8/1P4P1/P3Q2P/1q6 b - - 0 1",
        "8/5bk1/8/2Pp4/8/1K6/8/8 w - d6 0 1",
    };
    for (const char* fen : fens) {
        Position pos = from_fen(fen);
        Position mirrored = from_fen(mirror_fen(fen));
        CHECK_EQ(evaluate(mirrored), evaluate(pos));
    }
}

TEST(eval_sanity) {
    // Up a queen is clearly winning; the start position is roughly level.
    CHECK(evaluate(from_fen("rnb1kbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1")) > 600);
    CHECK(std::abs(evaluate(from_fen(START_FEN))) < 50);
    // A far-advanced passed pawn is worth more than one at home.
    int advanced = evaluate(from_fen("4k3/8/1P6/8/8/8/8/4K3 w - - 0 1"));
    int home = evaluate(from_fen("4k3/8/8/8/8/8/1P6/4K3 w - - 0 1"));
    CHECK(advanced > home);
    // K+R v K+B is drawish, so it scores far below the material difference.
    CHECK(evaluate(from_fen("4k3/8/8/2b5/8/8/8/3RK3 w - - 0 1")) < 150);
}

// ------------------------------------------------------------------------------------------------
// Transposition table

TEST(tt_store_and_probe) {
    TranspositionTable tt(1);
    CHECK(tt.probe(0x1234) == nullptr);
    tt.store(0x1234, 55, 40, make_move(E2, E4, DOUBLE_PUSH), 7, BOUND_EXACT);
    const TTEntry* e = tt.probe(0x1234);
    CHECK(e != nullptr);
    if (e) {
        CHECK_EQ(e->score, 55);
        CHECK_EQ(e->eval, 40);
        CHECK_EQ(e->depth, 7);
        CHECK_EQ(move_to_uci(e->move), std::string("e2e4"));
    }
    // A shallower non-exact result for the same position doesn't evict a deeper one...
    tt.store(0x1234, 10, 40, NO_MOVE, 2, BOUND_LOWER);
    CHECK_EQ(tt.probe(0x1234)->depth, 7);
    // ...but after a new search starts, it does, and the old best move is kept.
    tt.new_search();
    tt.store(0x1234, 10, 40, NO_MOVE, 2, BOUND_LOWER);
    CHECK_EQ(tt.probe(0x1234)->depth, 2);
    CHECK_EQ(move_to_uci(tt.probe(0x1234)->move), std::string("e2e4"));
    tt.clear();
    CHECK(tt.probe(0x1234) == nullptr);
}

TEST(mate_scores_are_ply_relative_in_tt) {
    int mate_in_3_from_root = MATE - 5;
    int stored = score_to_tt(mate_in_3_from_root, 2);  // seen 2 plies below the root
    CHECK_EQ(stored, MATE - 3);                        // mate in 3 plies from that node
    CHECK_EQ(score_from_tt(stored, 4), MATE - 7);      // reached again 4 plies from a root
    CHECK_EQ(score_to_tt(123, 9), 123);
}

// ------------------------------------------------------------------------------------------------
// Search

TEST(finds_forced_mates) {
    struct Puzzle {
        const char* fen;
        int mate_in;
    } puzzles[] = {
        {"6k1/5ppp/8/8/8/8/8/R5K1 w - - 0 1", 1},
        {"r1bqkb1r/pppp1ppp/2n2n2/4p2Q/2B1P3/8/PPPP1PPP/RNB1K1NR w KQkq - 4 4", 1},
        {"r2qkb1r/pp2nppp/3p4/2pNN1B1/2BnP3/3P4/PPP2PPP/R2bK2R w KQkq - 1 1", 2},
        {"6k1/pp4p1/2p5/2bp4/8/P5Pb/1P3rrP/2BRRN1K b - - 0 1", 2},
        {"r5rk/5p1p/5R2/4B3/8/8/7P/7K w - - 0 1", 3},
        {"2r3k1/p4p2/3Rp2p/1p2P1pK/8/1P4P1/P3Q2P/1q6 b - - 0 1", 3},
    };
    for (const Puzzle& p : puzzles) {
        Position pos = from_fen(p.fen);
        SearchResult r = search_depth(pos, 2 * p.mate_in + 4);
        CHECK_EQ(r.score, MATE - (2 * p.mate_in - 1));
        CHECK_EQ(format_score(r.score), "mate " + std::to_string(p.mate_in));
        // The move it chose must really force mate in that many moves (checked without pruning).
        if (p.mate_in <= 2 || !g_quick) {
            pos.make_move(r.best_move);
            bool forced = replies_all_lose(pos, p.mate_in);
            pos.unmake_move();
            CHECK(forced);
        }
    }
}

TEST(sees_it_is_getting_mated) {
    Position pos = from_fen("7k/5Q2/8/8/8/8/q4rr1/6K1 w - - 0 1");  // any move allows mate in 1
    SearchResult r = search_depth(pos, 6);
    CHECK_EQ(format_score(r.score), std::string("mate -1"));
}

TEST(no_moves_at_checkmate_or_stalemate) {
    Position mated = from_fen("R5k1/5ppp/8/8/8/8/8/6K1 b - - 1 1");
    CHECK(search_depth(mated, 5).best_move == NO_MOVE);
    Position stalemate = from_fen("k7/2Q5/1K6/8/8/8/8/8 b - - 0 1");
    CHECK(search_depth(stalemate, 5).best_move == NO_MOVE);
}

TEST(wins_hanging_material) {
    Position pos = from_fen("4k3/8/8/3q4/8/8/8/3RK3 w - - 0 1");
    SearchResult r = search_depth(pos, 6);
    CHECK_EQ(move_to_uci(r.best_move), std::string("d1d5"));
    CHECK(r.score > 400);
    // ...but not when the "free" piece is defended and taking it loses the exchange.
    Position defended = from_fen("4k3/4r3/8/4n3/8/8/8/4RK2 w - - 0 1");
    r = search_depth(defended, 6);
    CHECK(move_to_uci(r.best_move) != "e1e5");
}

TEST(avoids_stalemating_a_lone_king) {
    // Qb6?? and Qc7?? are stalemate; anything sensible keeps the win.
    Position pos = from_fen("k7/8/2K5/8/8/8/8/1Q6 w - - 0 1");
    SearchResult r = search_depth(pos, 8);
    CHECK(r.score > 500);
    pos.make_move(r.best_move);
    MoveList replies;
    generate_legal(pos, replies);
    CHECK(replies.size > 0 || pos.in_check());
}

TEST(converts_king_and_queen_against_king) {
    Position pos = from_fen("8/8/8/4k3/8/8/8/3QK3 w - - 0 1");
    Searcher searcher(8);
    SearchLimits limits;
    limits.depth = 8;
    // Let the engine play both sides; white must deliver mate well within 50 moves.
    std::string reason;
    int plies = 0;
    while (game_result(pos, &reason) == GameResult::ONGOING && plies < 100) {
        SearchResult r = searcher.search(pos, limits);
        pos.make_move(r.best_move);
        ++plies;
    }
    CHECK_EQ(reason, std::string("checkmate"));
    CHECK(plies < 60);
}

TEST(repetition_is_a_draw_in_search) {
    // White has an extra queen. After both knights shuffle twice, black can claim a threefold
    // repetition with Ng8, so a losing black takes the draw...
    Position losing = from_fen("rnb1kbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    play(losing, "g1f3 g8f6 f3g1 f6g8 g1f3 g8f6 f3g1");
    SearchResult r = search_depth(losing, 6);
    CHECK_EQ(move_to_uci(r.best_move), std::string("f6g8"));
    CHECK_EQ(r.score, 0);
    // ...while a winning black (here white is the one missing a queen) avoids it.
    Position winning = from_fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNB1KBNR w KQkq - 0 1");
    play(winning, "g1f3 g8f6 f3g1 f6g8 g1f3 g8f6 f3g1");
    SearchResult w = search_depth(winning, 6);
    CHECK(move_to_uci(w.best_move) != "f6g8");
    CHECK(w.score > 500);
}

TEST(fifty_move_rule_in_search) {
    // Up a rook, but the next move is the hundredth without a capture or pawn move.
    Position pos = from_fen("4k3/8/8/8/8/8/1R6/4K3 w - - 99 120");
    SearchResult r = search_depth(pos, 4);
    CHECK_EQ(r.score, 0);
}

TEST(time_and_node_limits) {
    Searcher searcher(8);
    Position pos = from_fen("r1bq1rk1/pp2bppp/2n1pn2/3p4/2PP4/2N1PN2/PP2BPPP/R1BQ1RK1 w - - 0 9");

    SearchLimits timed;
    timed.movetime = 200;
    timed.move_overhead = 0;
    auto start = std::chrono::steady_clock::now();
    SearchResult r = searcher.search(pos, timed);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    CHECK(r.best_move != NO_MOVE);
    CHECK(ms <= 200 + 100);

    SearchLimits clock;
    clock.time[WHITE] = 1000;  // one second left on the clock: must move quickly
    clock.move_overhead = 0;
    start = std::chrono::steady_clock::now();
    r = searcher.search(pos, clock);
    ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    CHECK(r.best_move != NO_MOVE);
    CHECK(ms < 900);

    SearchLimits nodes;
    nodes.nodes = 20000;
    r = searcher.search(pos, nodes);
    CHECK(r.nodes >= 20000 && r.nodes < 20000 + 4096);
}

TEST(search_is_deterministic) {
    // With a fixed depth the node count must not depend on timing: bench signatures rely on it.
    Position a = from_fen(PERFT_FENS[1]), b = from_fen(PERFT_FENS[1]);
    SearchResult ra = search_depth(a, 7), rb = search_depth(b, 7);
    CHECK_EQ(ra.nodes, rb.nodes);
    CHECK_EQ(move_to_uci(ra.best_move), move_to_uci(rb.best_move));
}

TEST(search_leaves_position_untouched) {
    Position pos = from_fen(PERFT_FENS[1]);
    std::string fen = pos.fen();
    Key key = pos.key();
    search_depth(pos, 6);
    CHECK_EQ(pos.fen(), fen);
    CHECK_EQ(pos.key(), key);
}

// ------------------------------------------------------------------------------------------------
// UCI front end

std::string run_uci(const std::vector<std::string>& commands) {
    std::ostringstream captured;
    std::streambuf* old = std::cout.rdbuf(captured.rdbuf());
    {
        UciEngine engine;
        for (const std::string& c : commands) {
            engine.handle(c);
            engine.wait_for_search();
        }
    }
    std::cout.rdbuf(old);
    return captured.str();
}

bool contains(const std::string& haystack, const std::string& needle) { return haystack.find(needle) != std::string::npos; }

TEST(uci_handshake) {
    std::string out = run_uci({"uci", "isready"});
    CHECK(contains(out, "id name Kestrel"));
    CHECK(contains(out, "option name Hash type spin"));
    CHECK(contains(out, "uciok"));
    CHECK(contains(out, "readyok"));
}

TEST(uci_position_and_go) {
    std::string out = run_uci({"position startpos moves e2e4 e7e5 g1f3", "d"});
    CHECK(contains(out, "FEN: rnbqkbnr/pppp1ppp/8/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R b KQkq - 1 2"));

    out = run_uci({"position fen 6k1/5ppp/8/8/8/8/8/R5K1 w - - 0 1", "go depth 4"});
    CHECK(contains(out, "score mate 1"));
    CHECK(contains(out, "bestmove a1a8"));

    out = run_uci({"position fen R5k1/5ppp/8/8/8/8/8/6K1 b - - 1 1", "go depth 3"});
    CHECK(contains(out, "bestmove 0000"));  // checkmated: no move to make
}

TEST(uci_errors_are_reported_not_fatal) {
    std::string out = run_uci({"position fen not/a/fen w - - 0 1", "position startpos moves e2e4 e2e4", "d", "frobnicate"});
    CHECK(contains(out, "info string invalid FEN"));
    CHECK(contains(out, "info string illegal move e2e4"));
    CHECK(contains(out, "FEN: rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1"));  // first e2e4 applied
    CHECK(contains(out, "info string unknown command: frobnicate"));
}

TEST(uci_setoption) {
    std::string out = run_uci({"setoption name Hash value 4", "setoption name Move Overhead value 50",
                               "setoption name Clear Hash", "setoption name Nope value 1", "isready"});
    CHECK(contains(out, "unknown option: Nope"));
    CHECK(contains(out, "readyok"));
}

TEST(uci_stop_ends_infinite_search) {
    std::ostringstream captured;
    std::streambuf* old = std::cout.rdbuf(captured.rdbuf());
    {
        UciEngine engine;
        engine.handle("position startpos");
        engine.handle("go infinite");
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        engine.handle("stop");  // joins the search thread
    }
    std::cout.rdbuf(old);
    std::string out = captured.str();
    CHECK(contains(out, "info depth"));
    CHECK(contains(out, "bestmove "));
}

TEST(uci_perft_divide) {
    std::string out = run_uci({"position startpos", "go perft 3"});
    CHECK(contains(out, "e2e4: 600"));
    CHECK(contains(out, "Nodes searched: 8902"));
}

}  // namespace

int main() {
    init_tables();
    const char* quick = std::getenv("KESTREL_QUICK_TESTS");
    g_quick = quick && *quick && std::string(quick) != "0";

    auto start = std::chrono::steady_clock::now();
    for (const TestCase& t : registry()) {
        g_current = t.name;
        int before = g_failures;
        auto t0 = std::chrono::steady_clock::now();
        t.fn();
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("%s %-45s %8.0f ms\n", g_failures == before ? "ok  " : "FAIL", t.name, ms);
    }
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("\n%zu tests, %d checks, %d failures (%.1fs)%s\n", registry().size(), g_checks, g_failures, seconds,
                g_quick ? " [quick mode]" : "");
    return g_failures ? 1 : 0;
}
