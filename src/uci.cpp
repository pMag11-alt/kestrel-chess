#include "uci.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <vector>

#include "eval.h"
#include "movegen.h"
#include "notation.h"

namespace kestrel {

constexpr const char* ENGINE_NAME = "Kestrel 1.0";
constexpr const char* ENGINE_AUTHOR = "Parshwa Magdum";

std::string format_score(int score) {
    if (score >= MATE_BOUND) return "mate " + std::to_string((MATE - score + 1) / 2);
    if (score <= -MATE_BOUND) return "mate " + std::to_string(-(MATE + score) / 2);
    return "cp " + std::to_string(score);
}

namespace {

std::string format_info(const SearchInfo& info) {
    std::ostringstream out;
    uint64_t nps = info.time_ms > 0 ? info.nodes * 1000 / uint64_t(info.time_ms) : info.nodes * 1000;
    out << "info depth " << info.depth << " seldepth " << info.seldepth << " score " << format_score(info.score)
        << " nodes " << info.nodes << " nps " << nps << " hashfull " << info.hashfull << " time " << info.time_ms << " pv";
    for (Move m : info.pv) out << ' ' << move_to_uci(m);
    return out.str();
}

}  // namespace

UciEngine::UciEngine() : searcher_(16) {}

UciEngine::~UciEngine() { stop_search(); }

void UciEngine::send(const std::string& line) {
    std::lock_guard<std::mutex> lock(out_mutex_);
    std::cout << line << std::endl;
}

void UciEngine::stop_search() {
    searcher_.stop();
    if (search_thread_.joinable()) search_thread_.join();
}

void UciEngine::wait_for_search() {
    if (search_thread_.joinable()) search_thread_.join();
}

void UciEngine::loop(std::istream& in) {
    std::string line;
    while (std::getline(in, line))
        if (!handle(line)) break;
    stop_search();
}

bool UciEngine::handle(const std::string& line) {
    std::istringstream args(line);
    std::string cmd;
    if (!(args >> cmd)) return true;

    if (cmd == "uci") cmd_uci();
    else if (cmd == "isready") send("readyok");
    else if (cmd == "setoption") cmd_setoption(args);
    else if (cmd == "ucinewgame") {
        stop_search();
        searcher_.new_game();
        pos_.set_fen(START_FEN);
    } else if (cmd == "position") {
        stop_search();
        cmd_position(args);
    } else if (cmd == "go") {
        stop_search();
        cmd_go(args);
    } else if (cmd == "stop") stop_search();
    else if (cmd == "ponderhit") { /* pondering isn't advertised, so there is nothing to do */ }
    else if (cmd == "quit") return false;
    // Extensions for humans and scripts.
    else if (cmd == "d") send(pos_.pretty());
    else if (cmd == "eval") send(eval_trace(pos_));
    else if (cmd == "perft") {
        int depth = 1;
        args >> depth;
        stop_search();
        cmd_perft(depth);
    } else if (cmd == "bench") {
        int depth = 13;
        args >> depth;
        stop_search();
        run_bench(depth, true);
        std::cout.flush();
    } else send("info string unknown command: " + line);
    return true;
}

void UciEngine::cmd_uci() {
    send(std::string("id name ") + ENGINE_NAME);
    send(std::string("id author ") + ENGINE_AUTHOR);
    send("option name Hash type spin default 16 min 1 max 4096");
    send("option name Move Overhead type spin default 30 min 0 max 5000");
    send("option name Clear Hash type button");
    send("uciok");
}

void UciEngine::cmd_setoption(std::istringstream& args) {
    // setoption name <name, may contain spaces> [value <value>]
    std::string token, name, value;
    args >> token;  // "name"
    while (args >> token && token != "value") name += (name.empty() ? "" : " ") + token;
    while (args >> token) value += (value.empty() ? "" : " ") + token;

    stop_search();
    try {
        if (name == "Hash") searcher_.set_hash_size(std::max(1, std::min(4096, std::stoi(value))));
        else if (name == "Move Overhead") move_overhead_ = std::max(0, std::min(5000, std::stoi(value)));
        else if (name == "Clear Hash") searcher_.new_game();
        else send("info string unknown option: " + name);
    } catch (...) {
        send("info string bad value for " + name + ": " + value);
    }
}

void UciEngine::cmd_position(std::istringstream& args) {
    std::string token, fen;
    args >> token;
    if (token == "startpos") {
        fen = START_FEN;
        args >> token;  // "moves", if present
    } else if (token == "fen") {
        while (args >> token && token != "moves") fen += (fen.empty() ? "" : " ") + token;
    } else {
        send("info string expected 'position startpos' or 'position fen ...'");
        return;
    }

    Position next;
    std::string error;
    if (!next.set_fen(fen, &error)) {
        send("info string invalid FEN: " + error);
        return;
    }
    while (args >> token) {
        Move m = parse_move(next, token);
        if (m == NO_MOVE) {
            send("info string illegal move " + token + ", ignoring the rest");
            break;
        }
        next.make_move(m);
    }
    pos_ = next;
}

void UciEngine::cmd_go(std::istringstream& args) {
    SearchLimits limits;
    limits.move_overhead = move_overhead_;
    std::string token;
    while (args >> token) {
        auto next_int = [&args]() -> int64_t {
            int64_t v = 0;
            args >> v;
            return v;
        };
        if (token == "depth") limits.depth = int(std::max<int64_t>(1, std::min<int64_t>(MAX_PLY - 1, next_int())));
        else if (token == "movetime") limits.movetime = next_int();
        else if (token == "wtime") limits.time[WHITE] = next_int();
        else if (token == "btime") limits.time[BLACK] = next_int();
        else if (token == "winc") limits.inc[WHITE] = next_int();
        else if (token == "binc") limits.inc[BLACK] = next_int();
        else if (token == "movestogo") limits.movestogo = int(next_int());
        else if (token == "nodes") limits.nodes = uint64_t(next_int());
        else if (token == "infinite" || token == "ponder") limits.infinite = true;
        else if (token == "perft") {
            cmd_perft(int(next_int()));
            return;
        }
    }

    Position root = pos_;
    searcher_.clear_stop();
    search_thread_ = std::thread([this, root, limits]() mutable {
        SearchResult r = searcher_.search(root, limits, [this](const SearchInfo& info) { send(format_info(info)); });
        // In infinite mode the GUI decides when we are done, even if the search finished early.
        while (limits.infinite && !searcher_.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::string line = "bestmove " + move_to_uci(r.best_move);
        if (r.ponder_move != NO_MOVE) line += " ponder " + move_to_uci(r.ponder_move);
        send(line);
    });
}

void UciEngine::cmd_perft(int depth) {
    if (depth < 1) depth = 1;
    auto start = std::chrono::steady_clock::now();
    MoveList moves;
    generate_legal(pos_, moves);
    uint64_t total = 0;
    for (Move m : moves) {
        pos_.make_move(m);
        uint64_t n = perft(pos_, depth - 1);
        pos_.unmake_move();
        total += n;
        send(move_to_uci(m) + ": " + std::to_string(n));
    }
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    char speed[64];
    std::snprintf(speed, sizeof speed, "%.0f", total / std::max(seconds, 1e-6));
    send("\nNodes searched: " + std::to_string(total) + "\nNodes/second: " + speed);
}

// ---------------------------------------------------------------------------------------------
// Interactive play in the terminal.

namespace {

const char* UNICODE_PIECES[12] = {"♙", "♘", "♗", "♖", "♕", "♔", "♟", "♞", "♝", "♜", "♛", "♚"};

void print_board(const Position& pos, Color view, bool unicode, Move last) {
    std::printf("\n");
    for (int r = 7; r >= 0; --r) {
        int rank = view == WHITE ? r : 7 - r;
        std::printf("  %d ", rank + 1);
        for (int f = 0; f < 8; ++f) {
            int file = view == WHITE ? f : 7 - f;
            int sq = make_square(file, rank);
            Piece p = pos.piece_on(sq);
            bool highlight = last != NO_MOVE && (sq == move_from(last) || sq == move_to(last));
            const char* open = highlight ? "[" : " ";
            const char* close = highlight ? "]" : " ";
            if (p == NO_PIECE) std::printf("%s%s%s", open, (file + rank) % 2 ? " " : "·", close);
            else if (unicode) std::printf("%s%s%s", open, UNICODE_PIECES[p], close);
            else std::printf("%s%c%s", open, "PNBRQKpnbrqk"[p], close);
        }
        std::printf("\n");
    }
    std::printf("    ");
    for (int f = 0; f < 8; ++f) std::printf(" %c ", view == WHITE ? 'a' + f : 'h' - f);
    std::printf("\n\n");
}

void print_help() {
    std::printf(
        "Enter moves in SAN (e4, Nf3, O-O, exd5, e8=Q) or UCI (e2e4, e7e8q).\n"
        "Commands:\n"
        "  hint          ask the engine for a suggestion\n"
        "  undo          take back your last move (and the engine's reply)\n"
        "  moves         show the game so far\n"
        "  fen           print the position as FEN\n"
        "  flip          turn the board around\n"
        "  new           start a new game\n"
        "  help          this text\n"
        "  quit          leave\n\n");
}

std::string pgn_moves(const std::vector<std::string>& sans, int first_fullmove, Color first_side) {
    std::string out;
    int fullmove = first_fullmove;
    Color side = first_side;
    for (size_t i = 0; i < sans.size(); ++i) {
        if (side == WHITE) out += std::to_string(fullmove) + ". ";
        else if (i == 0) out += std::to_string(fullmove) + "... ";
        out += sans[i] + " ";
        if (side == BLACK) ++fullmove;
        side = ~side;
    }
    return out.empty() ? "(no moves yet)" : out;
}

std::string score_text(int score) {
    char buf[32];
    if (score >= MATE_BOUND) std::snprintf(buf, sizeof buf, "mate in %d", (MATE - score + 1) / 2);
    else if (score <= -MATE_BOUND) std::snprintf(buf, sizeof buf, "mated in %d", (MATE + score) / 2);
    else std::snprintf(buf, sizeof buf, "%+.2f", score / 100.0);
    return buf;
}

}  // namespace

int play_interactive(int argc, char** argv) {
    Color human = WHITE;
    int64_t think_ms = 1000;
    bool unicode = false;
    std::string fen = START_FEN;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--black" || a == "black") human = BLACK;
        else if (a == "--white" || a == "white") human = WHITE;
        else if (a == "--unicode") unicode = true;
        else if (a == "--time" && i + 1 < argc) think_ms = std::max<int64_t>(10, std::atoll(argv[++i]));
        else if (a == "--fen" && i + 1 < argc) fen = argv[++i];
        else {
            std::fprintf(stderr, "usage: kestrel play [--black] [--time MS] [--unicode] [--fen FEN]\n");
            return 2;
        }
    }

    Position pos;
    std::string error;
    if (!pos.set_fen(fen, &error)) {
        std::fprintf(stderr, "invalid FEN: %s\n", error.c_str());
        return 2;
    }
    const Position start = pos;
    Searcher searcher(64);
    std::vector<std::string> sans;
    Color view = human;

    std::printf("Kestrel: you play %s, the engine thinks %lld ms per move. Type 'help' for commands.\n",
                human == WHITE ? "white" : "black", static_cast<long long>(think_ms));

    bool redraw = true;  // only redraw the board when the position or view has changed
    while (true) {
        std::string reason;
        GameResult result = game_result(pos, &reason);
        if (redraw) {
            print_board(pos, view, unicode, pos.last_move());
            if (result != GameResult::ONGOING) {
                const char* score = result == GameResult::WHITE_WINS ? "1-0" : result == GameResult::BLACK_WINS ? "0-1" : "1/2-1/2";
                std::printf("Game over: %s (%s)\n%s\n\nType 'new', 'undo' or 'quit'.\n", score, reason.c_str(),
                            pgn_moves(sans, start.fullmove_number(), start.side_to_move()).c_str());
            }
            redraw = false;
        }
        if (result == GameResult::ONGOING && pos.side_to_move() != human) {
            std::printf("Kestrel is thinking...\n");
            SearchLimits limits;
            limits.movetime = think_ms;
            limits.move_overhead = 0;
            SearchResult r = searcher.search(pos, limits);
            std::string san = move_to_san(pos, r.best_move);
            std::printf("Kestrel plays %s   (eval %s for Kestrel, depth %d, %llu nodes)\n", san.c_str(),
                        score_text(r.score).c_str(), r.depth, static_cast<unsigned long long>(r.nodes));
            sans.push_back(san);
            pos.make_move(r.best_move);
            redraw = true;
            continue;
        }

        std::printf("%s> ", result == GameResult::ONGOING ? "Your move" : "");
        std::fflush(stdout);
        std::string line;
        if (!std::getline(std::cin, line)) break;
        std::istringstream words(line);
        std::string word;
        if (!(words >> word)) continue;

        if (word == "quit" || word == "exit") break;
        if (word == "help") {
            print_help();
        } else if (word == "fen") {
            std::printf("%s\n", pos.fen().c_str());
        } else if (word == "moves") {
            std::printf("%s\n", pgn_moves(sans, start.fullmove_number(), start.side_to_move()).c_str());
        } else if (word == "flip") {
            view = ~view;
            redraw = true;
        } else if (word == "new") {
            pos = start;
            sans.clear();
            searcher.new_game();
            redraw = true;
        } else if (word == "undo") {
            // Take back to the last position where it was the human's turn.
            int undone = 0;
            while (pos.game_ply() > start.game_ply() && (undone == 0 || pos.side_to_move() != human)) {
                pos.unmake_move();
                sans.pop_back();
                ++undone;
            }
            if (!undone) std::printf("Nothing to undo.\n");
            redraw = undone > 0;
        } else if (word == "hint") {
            if (result != GameResult::ONGOING) continue;
            SearchLimits limits;
            limits.movetime = think_ms;
            limits.move_overhead = 0;
            SearchResult r = searcher.search(pos, limits);
            std::printf("Kestrel suggests %s (eval %s for you)\n", move_to_san(pos, r.best_move).c_str(), score_text(r.score).c_str());
        } else if (result != GameResult::ONGOING) {
            std::printf("The game is over. Type 'new', 'undo' or 'quit'.\n");
        } else {
            Move m = parse_move(pos, word);
            if (m == NO_MOVE) {
                std::printf("'%s' isn't a legal move here. Legal moves:", word.c_str());
                MoveList legal;
                generate_legal(pos, legal);
                for (Move lm : legal) std::printf(" %s", move_to_san(pos, lm).c_str());
                std::printf("\n");
                continue;
            }
            sans.push_back(move_to_san(pos, m));
            pos.make_move(m);
            redraw = true;
        }
    }
    return 0;
}

}  // namespace kestrel
