#!/usr/bin/env python3
"""Play a match between Kestrel and another UCI engine, and estimate the Elo difference.

    pip install chess
    python3 scripts/match.py --opponent stockfish --elo 1800 --games 40 --tc 10+0.1

Stockfish's UCI_LimitStrength / UCI_Elo options make it a calibrated sparring partner; pass
--elo 0 to play it at full strength (you will lose). Use --opponent random for a quick sanity
check against a player that picks legal moves at random.
"""
import argparse
import concurrent.futures
import math
import random
import sys
import time

import chess
import chess.engine

# Balanced, varied starting positions, so the games aren't all the same. Each is played twice
# with colours reversed.
OPENINGS = [
    "e2e4 e7e5 g1f3 b8c6 f1b5",          # Ruy Lopez
    "e2e4 c7c5 g1f3 d7d6 d2d4 c5d4",     # Sicilian
    "d2d4 d7d5 c2c4 e7e6 b1c3 g8f6",     # Queen's Gambit Declined
    "d2d4 g8f6 c2c4 g7g6 b1c3 f8g7",     # King's Indian
    "e2e4 e7e6 d2d4 d7d5 b1c3",          # French
    "e2e4 c7c6 d2d4 d7d5 e4e5",          # Caro-Kann, advance
    "c2c4 e7e5 b1c3 g8f6 g1f3 b8c6",     # English
    "g1f3 d7d5 g2g3 g8f6 f1g2 e7e6",     # Reti
    "e2e4 e7e5 g1f3 b8c6 f1c4 f8c5",     # Italian
    "d2d4 g8f6 c2c4 e7e6 g1f3 b7b6",     # Queen's Indian
]


class RandomPlayer:
    def play(self, board, limit):
        return chess.engine.PlayResult(random.choice(list(board.legal_moves)), None)

    def quit(self):
        pass


def open_engine(path, options):
    if path == "random":
        return RandomPlayer()
    engine = chess.engine.SimpleEngine.popen_uci(path)
    for name, value in options.items():
        if name in engine.options:
            engine.configure({name: value})
    return engine


def play_game(args, opening, kestrel_white):
    kestrel = open_engine(args.engine, {"Hash": 32})
    opp_options = {"Hash": 32, "Threads": 1}
    if args.elo:
        opp_options.update({"UCI_LimitStrength": True, "UCI_Elo": args.elo})
    opponent = open_engine(args.opponent, opp_options)
    players = {chess.WHITE: kestrel if kestrel_white else opponent, chess.BLACK: opponent if kestrel_white else kestrel}

    base, inc = args.tc
    clock = {chess.WHITE: base, chess.BLACK: base}
    board = chess.Board()
    for uci in opening.split():
        board.push_uci(uci)

    result, reason = "*", ""
    try:
        while not board.is_game_over(claim_draw=True):
            side = board.turn
            limit = chess.engine.Limit(white_clock=clock[chess.WHITE], black_clock=clock[chess.BLACK], white_inc=inc, black_inc=inc)
            start = time.monotonic()
            played = players[side].play(board, limit)
            clock[side] -= time.monotonic() - start
            if clock[side] < 0:
                result = "0-1" if side == chess.WHITE else "1-0"
                reason = "time forfeit"
                break
            clock[side] += inc
            if played.move is None or played.move not in board.legal_moves:
                result = "0-1" if side == chess.WHITE else "1-0"
                reason = f"illegal move {played.move}"
                break
            board.push(played.move)
        else:
            outcome = board.outcome(claim_draw=True)
            result = outcome.result()
            reason = outcome.termination.name.lower()
    finally:
        kestrel.quit()
        opponent.quit()

    if result == "1/2-1/2":
        points = 0.5
    else:
        points = 1.0 if (result == "1-0") == kestrel_white else 0.0
    return points, result, reason, board.ply(), kestrel_white


def elo_estimate(score, games):
    """Elo difference and 95% error margin from a score fraction (logistic model)."""
    p = min(max(score / games, 1e-3), 1 - 1e-3)
    elo = -400 * math.log10(1 / p - 1)
    # Standard error of the mean score, assuming the worst-case per-game variance of 0.25.
    se = math.sqrt(0.25 / games)
    lo = min(max(p - 1.96 * se, 1e-3), 1 - 1e-3)
    hi = min(max(p + 1.96 * se, 1e-3), 1 - 1e-3)
    return elo, -400 * math.log10(1 / lo - 1), -400 * math.log10(1 / hi - 1)


def parse_tc(text):
    base, _, inc = text.partition("+")
    return float(base), float(inc or 0)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--engine", default="./kestrel")
    parser.add_argument("--opponent", default="stockfish", help="path to a UCI engine, or 'random'")
    parser.add_argument("--elo", type=int, default=1800, help="UCI_Elo for the opponent (0 = full strength)")
    parser.add_argument("--games", type=int, default=20)
    parser.add_argument("--tc", type=parse_tc, default=(10.0, 0.1), help="seconds+increment, e.g. 10+0.1")
    parser.add_argument("--concurrency", type=int, default=4)
    args = parser.parse_args()

    jobs = []
    for i in range(args.games):
        opening = OPENINGS[(i // 2) % len(OPENINGS)]
        jobs.append((opening, i % 2 == 0))

    opponent_name = args.opponent if not args.elo else f"{args.opponent} (UCI_Elo {args.elo})"
    print(f"Kestrel vs {opponent_name}: {args.games} games at {args.tc[0]:g}+{args.tc[1]:g}s\n")
    score = wins = draws = losses = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.concurrency) as pool:
        futures = [pool.submit(play_game, args, opening, white) for opening, white in jobs]
        for n, future in enumerate(concurrent.futures.as_completed(futures), 1):
            points, result, reason, plies, white = future.result()
            score += points
            wins += points == 1.0
            draws += points == 0.5
            losses += points == 0.0
            colour = "white" if white else "black"
            print(f"game {n:3d}: Kestrel {colour:5s}  {result:7s}  {reason:22s} {plies:3d} plies   +{wins} ={draws} -{losses}")
            sys.stdout.flush()

    elo, lo, hi = elo_estimate(score, args.games)
    print(f"\nScore {score:g}/{args.games} (+{wins} ={draws} -{losses})")
    print(f"Elo difference: {elo:+.0f}  (95% interval {lo:+.0f} .. {hi:+.0f})")


if __name__ == "__main__":
    main()
