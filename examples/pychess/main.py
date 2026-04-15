# SPDX-License-Identifier: LGPL-3.0-or-later
"""PyChess — play chess against a local LLM via entropic.

Two-tier chess engine: thinker analyzes, auto-chain hands off to executor.

Usage:
    1. Edit data/default_config.yaml with your model paths
    2. python main.py

@brief Interactive chess game against C engine. Synchronous, no asyncio.
@version 2
"""

from __future__ import annotations

import sys

import chess
from chess_server import format_board_text


## @brief Display board state and move list to stdout.
## @utility
## @version 1
def print_board(board: chess.Board) -> None:
    """Print the board with coordinates and move history.

    @brief Display board state and move list to stdout.
    @version 1
    """
    print()
    print(format_board_text(board))
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


## @brief Read UCI move from stdin, validate, return or re-prompt.
## @utility
## @return Validated Move or None to quit.
## @version 1
def get_human_move(board: chess.Board, prompt: str | None = None) -> chess.Move | None:
    """Prompt the human for a move. Returns None to quit.

    @brief Read UCI move from stdin, validate, return or re-prompt.
    @version 1
    """
    prompt = prompt or "Your move (UCI, e.g. e2e4): "
    while True:
        raw = input(prompt).strip().lower()
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


## @brief Display winner or draw status.
## @utility
## @version 1
def _print_game_result(board: chess.Board) -> None:
    """Print the game outcome.

    @brief Display winner or draw status.
    @version 1
    """
    result = board.result()
    print(f"Game over: {result}")
    outcome = board.outcome()
    if outcome and outcome.winner is not None:
        winner = "White" if outcome.winner == chess.WHITE else "Black"
        print(f"Winner: {winner}")
    else:
        print("Draw!")


## @brief Prompt for move, push to board, display.
## @utility
## @return False if the player quit, True otherwise.
## @version 2
def _play_human_turn(board: chess.Board) -> bool:
    """Handle human's turn. Returns False if the player quit.

    @brief Prompt for move, push to board, display.
    @version 2
    """
    move = get_human_move(board)
    if move is None:
        print("Goodbye!")
        return False
    board.push(move)
    print_board(board)
    return True


## @brief Run AI generation, fall back to manual input on failure.
## @utility
## @return False if player quit, True otherwise.
## @version 2
def _play_ai_turn(chess_engine: object, board: chess.Board) -> bool:
    """Handle AI's turn. Returns False if AI failed and player quit.

    @brief Run AI generation, fall back to manual input on failure.
    @version 2
    """
    import engine as eng

    print("AI is thinking...")
    ai_move = eng.get_ai_move(chess_engine)
    if ai_move is not None:
        print(f"AI plays: {ai_move}")
        print_board(board)
        return True

    print("\nAI could not find a move. Play for Black or type 'quit'.")
    move = get_human_move(board, prompt="Move for Black (UCI): ")
    if move is None:
        return False
    board.push(move)
    print_board(board)
    return True


## @brief Initialize engine, alternate human/AI turns until game over.
## @utility
## @version 3
def game_loop() -> None:
    """Run the main game loop.

    @brief Initialize engine, alternate human/AI turns until game over.
    @version 3
    """
    import engine as eng

    print("Loading model...")
    chess_engine = eng.create_engine()
    board = chess_engine.board

    print("PyChess — You are White, AI is Black.")
    print("Enter moves in UCI notation (e.g. e2e4). Type 'quit' to exit.")
    print_board(board)

    while not board.is_game_over():
        if board.turn == chess.WHITE:
            if not _play_human_turn(board):
                break
        elif not _play_ai_turn(chess_engine, board):
            break

    if board.is_game_over():
        _print_game_result(board)

    eng.shutdown(chess_engine)


## @brief Run game loop, handle keyboard interrupt.
## @utility
## @version 1
def main() -> None:
    """Entry point.

    @brief Run game loop, handle keyboard interrupt.
    @version 1
    """
    try:
        game_loop()
    except KeyboardInterrupt:
        print("\nInterrupted.")
        sys.exit(0)


if __name__ == "__main__":
    main()
