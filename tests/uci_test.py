#!/usr/bin/env python3
"""Integration tests that drive the real ./kestrel binary through python-chess, an independent
UCI client and rules implementation.

    pip install chess
    python3 tests/uci_test.py [path/to/kestrel]
"""
import os
import random
import subprocess
import sys
import unittest

import chess
import chess.engine

ENGINE = os.environ.get("KESTREL", "./kestrel")


def perft(board, depth):
    if depth == 0:
        return 1
    total = 0
    for move in board.legal_moves:
        board.push(move)
        total += perft(board, depth - 1)
        board.pop()
    return total


def random_position(rng, max_plies):
    """A position reached by random legal moves, avoiding ones where the game is already over."""
    board = chess.Board()
    for _ in range(rng.randint(0, max_plies)):
        moves = list(board.legal_moves)
        if not moves:
            break
        board.push(rng.choice(moves))
    while board.is_game_over() and board.move_stack:
        board.pop()
    return board


class UciTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.engine = chess.engine.SimpleEngine.popen_uci(ENGINE)

    @classmethod
    def tearDownClass(cls):
        cls.engine.quit()

    def test_identifies_itself(self):
        self.assertTrue(self.engine.id["name"].startswith("Kestrel"))
        self.assertIn("Hash", self.engine.options)
        self.assertEqual(self.engine.options["Hash"].default, 16)

    def test_configures_options(self):
        self.engine.configure({"Hash": 32, "Move Overhead": 10})
        self.engine.ping()

    def test_analysis_returns_a_legal_principal_variation(self):
        board = chess.Board("r1bq1rk1/pp2bppp/2n1pn2/3p4/2PP4/2N1PN2/PP2BPPP/R1BQ1RK1 w - - 0 9")
        info = self.engine.analyse(board, chess.engine.Limit(depth=8))
        self.assertEqual(info["depth"], 8)
        self.assertGreater(len(info["pv"]), 0)
        for move in info["pv"]:
            self.assertIn(move, board.legal_moves)
            board.push(move)

    def test_reports_mate_scores(self):
        board = chess.Board("r5rk/5p1p/5R2/4B3/8/8/7P/7K w - - 0 1")
        info = self.engine.analyse(board, chess.engine.Limit(depth=10))
        self.assertEqual(info["score"].white(), chess.engine.Mate(3))
        line = info["pv"][:5]
        for move in line:
            board.push(move)
        self.assertTrue(board.is_checkmate())

    def test_infinite_analysis_can_be_stopped(self):
        board = chess.Board()
        with self.engine.analysis(board) as analysis:
            for info in analysis:
                if info.get("depth", 0) >= 6:
                    break
        self.assertGreaterEqual(info["depth"], 6)
        self.engine.ping()  # still responsive afterwards

    def test_plays_complete_games_with_legal_moves(self):
        rng = random.Random(7)
        for game in range(2):
            board = chess.Board()
            kestrel_side = chess.WHITE if game == 0 else chess.BLACK
            while not board.is_game_over(claim_draw=True) and board.ply() < 300:
                if board.turn == kestrel_side:
                    result = self.engine.play(board, chess.engine.Limit(time=0.02))
                    self.assertIn(result.move, board.legal_moves)
                    board.push(result.move)
                else:
                    board.push(rng.choice(list(board.legal_moves)))
            # Against random moves the engine should always win.
            self.assertTrue(board.is_checkmate(), board.fen())
            self.assertEqual(board.outcome().winner, kestrel_side)

    def test_plays_under_clock(self):
        board = chess.Board()
        limit = chess.engine.Limit(white_clock=1.0, black_clock=1.0, white_inc=0.01, black_inc=0.01)
        result = self.engine.play(board, limit)
        self.assertIn(result.move, board.legal_moves)


class PerftFuzzTest(unittest.TestCase):
    def kestrel_perft(self, board, depth):
        out = subprocess.run([ENGINE, "perft", str(depth), board.fen()], capture_output=True, text=True, check=True).stdout
        divide = {}
        for line in out.splitlines():
            if ":" in line and not line.startswith("Nodes"):
                move, count = line.split(":")
                divide[move.strip()] = int(count)
            if line.startswith("Nodes searched:"):
                total = int(line.split(":")[1])
        return total, divide

    def test_random_positions_match_python_chess(self):
        rng = random.Random(2024)
        for _ in range(60):
            board = random_position(rng, 80)
            depth = 2 if rng.random() < 0.7 else 3
            total, divide = self.kestrel_perft(board, depth)
            expected = {}
            for move in board.legal_moves:
                board.push(move)
                expected[move.uci()] = perft(board, depth - 1)
                board.pop()
            self.assertEqual(divide, expected, f"divide mismatch in {board.fen()}")
            self.assertEqual(total, sum(expected.values()))

    def test_tricky_positions_match_python_chess(self):
        fens = [
            "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
            "rnbqkbnr/ppp1p1pp/8/3pPp2/8/8/PPPP1PPP/RNBQKBNR w KQkq f6 0 3",
            "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
            "n1n5/PPPk4/8/8/8/8/4Kppp/5N1N b - - 0 1",
            "r3k2r/8/3Q4/8/8/5q2/8/R3K2R b KQkq - 0 1",
        ]
        for fen in fens:
            board = chess.Board(fen)
            total, _ = self.kestrel_perft(board, 3)
            self.assertEqual(total, perft(board, 3), fen)


if __name__ == "__main__":
    if len(sys.argv) > 1 and not sys.argv[1].startswith("-"):
        ENGINE = sys.argv.pop(1)
    unittest.main(verbosity=2)
