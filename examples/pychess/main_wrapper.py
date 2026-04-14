"""PyChess — play chess against a local LLM via the entropic Python wrapper.

Two-tier chess engine: thinker analyzes, auto-chain hands off to executor.
The chess MCP server (chess_server.py) runs as an external process registered
with the C engine over stdio transport.

Usage:
    1. Set ENTROPIC_LIB_PATH to point to librentropic.so
    2. Edit data/default_config.yaml with your model paths
    3. python main_wrapper.py

@brief Interactive chess game using C engine via Python wrapper.
@version 1
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

import chess
from chess_server import format_board_text
from entropic import EntropicEngine

EXAMPLE_ROOT = Path(__file__).resolve().parent


## @brief Display board state and move history to stdout.
## @param board Current chess board.
## @utility
## @version 2.0.2
def print_board(board: chess.Board) -> None:
    """Print the board with coordinates and move history.

    @brief Display board state and move list to stdout.
    @version 1
    """
    print()
    print(format_board_text(board))
    if board.move_stack:
        moves: list[str] = []
        for i, move in enumerate(board.move_stack):
            ply = i + 1
            if ply % 2 == 1:
                moves.append(f"{(ply + 1) // 2}. {move.uci()}")
            else:
                moves[-1] += f" {move.uci()}"
        print(f"\nMoves: {' '.join(moves)}")
    print()


## @brief Read UCI move from stdin, validate, return or re-prompt.
## @param board Current chess board for move validation.
## @return Valid chess move, or None to quit.
## @utility
## @version 2.0.2
def get_human_move(board: chess.Board) -> chess.Move | None:
    """Prompt the human for a move. Returns None to quit.

    @brief Read UCI move from stdin, validate, return or re-prompt.
    @version 1
    """
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
            continue
        return move


## @brief Extract UCI move string from executor tool-call output.
## @param output Raw streamed text from executor.
## @return UCI move string, or None if not found.
## @utility
## @version 2.0.2
def _parse_move_from_output(output: str) -> str | None:
    """Extract the UCI move from the executor's tool call output.

    @brief Regex search for the chess.make_move arguments in streamed text.
    @version 1
    """
    match = re.search(r'"move"\s*:\s*"([a-h][1-8][a-h][1-8][qrbn]?)"', output)
    return match.group(1) if match else None


## @brief Parse and apply the AI's move from streaming output.
## @param board Current chess board.
## @param output Raw streamed text containing the move.
## @return Applied UCI move string, or None on failure.
## @utility
## @version 2.0.2
def _apply_parsed_move(board: chess.Board, output: str) -> str | None:
    """Parse and apply the AI's move from streaming output.

    @brief Extract UCI move, validate against board, push if legal.
    @version 1
    """
    move_uci = _parse_move_from_output(output)
    if not move_uci:
        return None
    try:
        move = chess.Move.from_uci(move_uci)
        if move in board.legal_moves:
            board.push(move)
            return move_uci
    except (chess.InvalidMoveError, ValueError):
        pass
    return None


## @brief Build piece list and move history for engine context.
## @param board Current chess board.
## @return Formatted context string for the AI prompt.
## @utility
## @version 2.0.2
def _build_board_context(board: chess.Board) -> str:
    """Format current board state for the AI prompt.

    @brief Build piece list and move history for engine context.
    @version 1
    """
    pieces = _board_to_piece_list(board)
    lines = [
        "## Current Position",
        "",
        f"White pieces: {pieces['white']}",
        f"Black pieces: {pieces['black']}",
        "",
        f"**Move:** {board.fullmove_number}",
        f"**Side to move:** {'Black (you)' if board.turn == chess.BLACK else 'White'}",
    ]
    if board.move_stack:
        lines.append(f"\n**Move history:**\n{_annotate_moves(board)}")
    return "\n".join(lines)


## @brief Scan board for pieces, format as comma-separated strings by color.
## @param board Current chess board.
## @return Dict with 'white' and 'black' piece list strings.
## @utility
## @version 2.0.2
def _board_to_piece_list(board: chess.Board) -> dict[str, str]:
    """Build comma-separated piece lists grouped by color.

    @brief Scan board for pieces, format as 'Pc2, Nb1' strings.
    @version 1
    """
    white_pieces: list[str] = []
    black_pieces: list[str] = []
    for sq in chess.SQUARES:
        piece = board.piece_at(sq)
        if piece is None:
            continue
        entry = f"{piece.symbol()}{chess.square_name(sq)}"
        target = white_pieces if piece.color == chess.WHITE else black_pieces
        target.append(entry)
    return {"white": ", ".join(white_pieces), "black": ", ".join(black_pieces)}


## @brief Build annotated move history with piece names for context.
## @param board Current chess board with move stack.
## @return Formatted move history string.
## @utility
## @version 2.0.2
def _annotate_moves(board: chess.Board) -> str:
    """Build annotated move history from the board's move stack.

    @brief Replay moves with piece names for context.
    @version 1
    """
    replay = chess.Board()
    entries: list[str] = []
    for move in board.move_stack:
        piece = replay.piece_at(move.from_square)
        color = "White" if replay.turn == chess.WHITE else "Black"
        name = chess.piece_name(piece.piece_type) if piece else "?"
        entries.append(f"{color} {name} {move.uci()}")
        replay.push(move)
    lines: list[str] = []
    for i in range(0, len(entries), 2):
        move_num = (i // 2) + 1
        pair = entries[i : i + 2]
        lines.append(f"{move_num}. {', '.join(pair)}")
    return "\n".join(lines)


## @brief Stream thinker+executor output, parse and apply AI move.
## @param engine Entropic engine instance.
## @param board Current chess board.
## @return Applied UCI move string, or None on failure.
## @utility
## @version 2.0.2
def _get_ai_move(engine: EntropicEngine, board: chess.Board) -> str | None:
    """Run the AI pipeline and extract its move.

    @brief Stream thinker+executor output, parse and apply AI move.
    @version 1
    """
    context = _build_board_context(board)
    prompt = f"{context}\n\nIt's your turn (Black). Analyze the position and play your move."
    collected: list[str] = []

    def on_token(tok: str) -> None:
        """Forward tokens to stderr and accumulate.

        @brief Streaming callback for AI output capture.
        @version 1
        """
        collected.append(tok)
        sys.stderr.write(tok)
        sys.stderr.flush()

    engine.run_streaming(prompt, on_token=on_token)
    return _apply_parsed_move(board, "".join(collected))


## @brief Alternate human/AI turns until game over or quit.
## @param engine Entropic engine instance.
## @utility
## @version 2.0.2
def _play_game(engine: EntropicEngine) -> None:
    """Run the main game loop.

    @brief Alternate human/AI turns until game over or quit.
    @version 1
    """
    board = chess.Board()
    print("PyChess — You are White, AI is Black.")
    print("Enter moves in UCI notation (e.g. e2e4). Type 'quit' to exit.")
    print_board(board)

    while not board.is_game_over():
        if board.turn == chess.WHITE:
            move = get_human_move(board)
            if move is None:
                print("Goodbye!")
                break
            board.push(move)
            print_board(board)
        else:
            print("AI is thinking...")
            ai_move = _get_ai_move(engine, board)
            if ai_move:
                print(f"\nAI plays: {ai_move}")
                print_board(board)
            else:
                print("\nAI could not find a valid move.")
                break

    if board.is_game_over():
        result = board.result()
        print(f"Game over: {result}")


## @brief Initialize engine with MCP server, run game, clean up.
## @utility
## @version 2.0.2
def game_loop() -> None:
    """Create engine and run the chess game.

    @brief Initialize engine with MCP server, run game, clean up.
    @version 1
    """
    project_dir = str(EXAMPLE_ROOT / ".pychess")
    engine = EntropicEngine()
    engine.configure_dir(project_dir)
    server_json = json.dumps({"command": "python3", "args": ["chess_server.py"]})
    engine.register_mcp_server("chess", server_json)
    _play_game(engine)
    engine.destroy()


## @brief Run game loop, handle keyboard interrupt.
## @utility
## @version 2.0.2
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
