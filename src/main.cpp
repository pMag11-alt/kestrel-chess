// Kestrel: a UCI chess engine.
//
//   kestrel                 speak UCI on stdin/stdout (what chess GUIs expect)
//   kestrel play [--black]  play a game in the terminal
//   kestrel bench [depth]   fixed-depth benchmark; prints a node-count signature
//   kestrel perft <depth> [fen...]   count move-generation leaf nodes
//   kestrel "<uci command>"...       run UCI commands from the command line, then exit

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include "movegen.h"
#include "position.h"
#include "search.h"
#include "uci.h"

using namespace kestrel;

int main(int argc, char** argv) {
    init_tables();
    std::string mode = argc > 1 ? argv[1] : "";

    if (mode == "play") return play_interactive(argc, argv);

    if (mode == "bench") {
        int depth = argc > 2 ? std::atoi(argv[2]) : 13;
        run_bench(depth > 0 ? depth : 13, true);
        return 0;
    }

    if (mode == "perft") {
        if (argc < 3) {
            std::fprintf(stderr, "usage: kestrel perft <depth> [fen]\n");
            return 2;
        }
        UciEngine engine;
        if (argc > 3) {
            std::string fen;
            for (int i = 3; i < argc; ++i) fen += std::string(i > 3 ? " " : "") + argv[i];
            engine.handle("position fen " + fen);
        }
        engine.handle(std::string("perft ") + argv[2]);
        return 0;
    }

    if (mode == "-h" || mode == "--help" || mode == "help") {
        std::printf(
            "Kestrel, a UCI chess engine\n\n"
            "  kestrel                  speak UCI on stdin/stdout (for chess GUIs)\n"
            "  kestrel play [--black] [--time MS] [--unicode]\n"
            "                           play a game against the engine in the terminal\n"
            "  kestrel bench [depth]    run the fixed-depth benchmark\n"
            "  kestrel perft <depth> [fen]\n"
            "                           count legal move paths (move generator check)\n");
        return 0;
    }

    UciEngine engine;
    if (!mode.empty()) {
        // UCI commands given as arguments run in order and then exit, e.g.
        //   kestrel "position startpos moves e2e4" "go depth 12"
        for (int i = 1; i < argc; ++i) {
            if (!engine.handle(argv[i])) return 0;
            engine.wait_for_search();
        }
        return 0;
    }
    engine.loop(std::cin);
    return 0;
}
