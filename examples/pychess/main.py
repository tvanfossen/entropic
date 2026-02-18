"""PyChess — play chess against a local LLM via entropi.

Demonstrates a multi-tier agentic chess system:
    suggest → validate → execute

The LLM analyzes the position, validates its choice, then plays.
Routing and handoff are handled by the entropi engine automatically.

Usage:
    1. Edit config.yaml with your model paths (auto-seeded on first run)
    2. python main.py
"""

from __future__ import annotations

import asyncio
import sys

import chess


def print_board(board: chess.Board) -> None:
    """Print the board with coordinates and move history."""
    print()
    print(board)
    if board.move_stack:
        moves = []
        for i, move in enumerate(board.move_stack):
            ply = i + 1
            if ply % 2 == 1:
                moves.append(f"{(ply + 1) // 2}. {move.uci()}")
            else:
                moves[-1] += f" {move.uci()}"
        print(f"\nMoves: {' '.join(moves)}")
    print()


def get_human_move(board: chess.Board) -> chess.Move | None:
    """Prompt the human for a move. Returns None to quit."""
    while True:
        raw = input("Your move (UCI, e.g. e2e4): ").strip().lower()
        if raw in ("quit", "exit", "q"):
            return None
        try:
            move = chess.Move.from_uci(raw)
        except (chess.InvalidMoveError, ValueError):
            print(f"  Invalid UCI notation: '{raw}'")
            continue
        if move not in board.legal_moves:
            print(f"  Illegal move: '{raw}'")
            legal = " ".join(m.uci() for m in board.legal_moves)
            print(f"  Legal moves: {legal}")
            continue
        return move


def _print_game_result(board: chess.Board) -> None:
    """Print the game outcome."""
    result = board.result()
    print(f"Game over: {result}")
    outcome = board.outcome()
    if outcome and outcome.winner is not None:
        winner = "White" if outcome.winner == chess.WHITE else "Black"
        print(f"Winner: {winner}")
    else:
        print("Draw!")


async def _play_human_turn(board: chess.Board) -> bool:
    """Handle human's turn. Returns False if the player quit."""
    move = get_human_move(board)
    if move is None:
        print("Goodbye!")
        return False
    board.push(move)
    print_board(board)
    return True


async def _play_ai_turn(chess_engine: object) -> None:
    """Handle AI's turn via the entropi engine."""
    import engine as eng

    print("AI is thinking (suggest → validate → execute)...")
    ai_move = await eng.get_ai_move(chess_engine)
    if ai_move is None:
        print("AI failed to make a move. Try again or quit.")
        return
    print(f"AI plays: {ai_move}")
    print_board(chess_engine.chess_server.board)


async def game_loop() -> None:
    """Run the main game loop."""
    import engine as eng

    print("Loading model...")
    chess_engine = await eng.create_engine()
    board = chess_engine.chess_server.board

    print("PyChess — You are White, AI is Black.")
    print("AI uses three tiers: suggest → validate → execute")
    print("Enter moves in UCI notation (e.g. e2e4). Type 'quit' to exit.")
    print_board(board)

    while not board.is_game_over():
        if board.turn == chess.WHITE:
            if not await _play_human_turn(board):
                break
        else:
            await _play_ai_turn(chess_engine)

    if board.is_game_over():
        _print_game_result(board)

    await eng.shutdown(chess_engine)


def main() -> None:
    """Entry point."""
    try:
        asyncio.run(game_loop())
    except KeyboardInterrupt:
        print("\nInterrupted.")
        sys.exit(0)


if __name__ == "__main__":
    main()
