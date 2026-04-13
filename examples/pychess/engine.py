"""Engine wiring — connects entropic wrapper to chess game loop.

Thin wrapper around EntropicEngine. The chess MCP server runs as an
external stdio process registered in the engine's config.

@brief EntropicEngine wrapper for pychess. No async, no Python engine.
@version 2
"""

from __future__ import annotations

import json
import logging
import sys
from dataclasses import dataclass
from pathlib import Path

import chess
from chess_server import board_to_pieces
from entropic import EntropicEngine

logger = logging.getLogger(__name__)

EXAMPLE_ROOT = Path(__file__).resolve().parent


@dataclass
class ChessEngine:
    """Wired entropic components for chess play.

    @brief Holds EntropicEngine handle and shared board state.
    @version 2
    """

    engine: EntropicEngine
    board: chess.Board


## @brief Instantiate EntropicEngine with pychess config.
## @utility
## @return Configured ChessEngine instance.
## @version 2
def create_engine() -> ChessEngine:
    """Create and configure the engine from local config.

    @brief Instantiate EntropicEngine with pychess config.
    @version 2
    """
    config_path = EXAMPLE_ROOT / "data" / "default_config.yaml"
    engine = EntropicEngine(config_path=str(config_path))
    return ChessEngine(engine=engine, board=chess.Board())


## @brief Build annotated move history string.
## @utility
## @return Formatted move history.
## @version 1
def _annotate_move_history(board: chess.Board) -> str:
    """Replay moves from the start and annotate each with piece name and color.

    @brief Build annotated move history string.
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


## @brief Build JSON board representation for engine context.
## @utility
## @return Markdown-formatted board context string.
## @version 1
def _build_board_context(board: chess.Board) -> str:
    """Format current board state for the system prompt.

    @brief Build JSON board representation for engine context.
    @version 1
    """
    pieces_json = json.dumps(board_to_pieces(board), separators=(",", ":"))

    lines = [
        "## Board Format",
        "",
        "- `your_pieces` — your Black pieces with `square`, `piece` type, and",
        "  `moves` (legal UCI moves from that square when present)",
        "- `opponent_pieces` — White's pieces with `square` and `piece` type",
        "",
        "Pieces without a `moves` array cannot move this turn.",
        "",
        "## Current Position",
        "",
        f"```json\n{pieces_json}\n```",
        "",
        f"**Move:** {board.fullmove_number}",
        f"**Side to move:** {'Black (you)' if board.turn == chess.BLACK else 'White (opponent)'}",
    ]

    if board.move_stack:
        lines.append(f"**Move history:**\n{_annotate_move_history(board)}")

    return "\n".join(lines)


## @brief Single synchronous generation: analyze + play via C engine.
## @utility
## @return UCI move string or None if AI failed.
## @version 2
def get_ai_move(chess_engine: ChessEngine) -> str | None:
    """Run the agentic loop — LLM analyzes the position and plays a move.

    @brief Single synchronous generation: analyze + play via C engine.
    @version 2
    """
    ply_before = len(chess_engine.board.move_stack)
    board_context = _build_board_context(chess_engine.board)
    prompt = f"{board_context}\n\nIt's your turn (Black). Analyze the position and play your move."

    def on_token(tok: str) -> None:
        """Stream tokens to log file.

        @brief Forward streaming tokens to stderr for debugging.
        @version 1
        """
        sys.stderr.write(tok)
        sys.stderr.flush()

    chess_engine.engine.run_streaming(prompt, on_token=on_token)

    if len(chess_engine.board.move_stack) > ply_before:
        return chess_engine.board.move_stack[-1].uci()

    logger.warning("AI did not make a move (ply unchanged: %d)", ply_before)
    return None


## @brief Destroy the C engine handle.
## @utility
## @version 2
def shutdown(chess_engine: ChessEngine) -> None:
    """Clean shutdown of entropic engine.

    @brief Destroy the C engine handle.
    @version 2
    """
    chess_engine.engine.destroy()
