// The Universal Chess Interface front end (what chess GUIs such as Cute Chess, Arena, BanksiaGUI
// or lichess-bot talk to), plus a human-friendly terminal mode.
#pragma once

#include <iosfwd>
#include <mutex>
#include <string>
#include <thread>

#include "position.h"
#include "search.h"

namespace kestrel {

class UciEngine {
public:
    UciEngine();
    ~UciEngine();

    // Reads commands until "quit" or end of input.
    void loop(std::istream& in);
    // Handles one command line. Returns false when the engine should exit.
    bool handle(const std::string& line);
    // Lets a running search finish on its own (for commands given on the command line).
    void wait_for_search();

private:
    void cmd_uci();
    void cmd_setoption(std::istringstream& args);
    void cmd_position(std::istringstream& args);
    void cmd_go(std::istringstream& args);
    void cmd_perft(int depth);
    void stop_search();
    void send(const std::string& line);

    Position pos_;
    Searcher searcher_;
    std::thread search_thread_;
    std::mutex out_mutex_;
    int64_t move_overhead_ = 30;
};

std::string format_score(int score);  // "cp 35" or "mate -3"

// "kestrel play": a game against the engine in the terminal.
int play_interactive(int argc, char** argv);

}  // namespace kestrel
