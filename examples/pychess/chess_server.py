"""Chess MCP server — standalone external server over stdio.

Exposes chess board tools (get_board, make_move) via the MCP protocol.
Registered in config as an external MCP server; the C engine communicates
with it over stdio transport.

Can also be imported directly for board utility functions (board_to_pieces,
format_board_text).

@brief Standalone chess MCP server (stdio transport).
@version 2
"""

from __future__ import annotations

from typing import Any

import chess


def board_to_pieces(
    board: chess.Board,
    ai_color: chess.Color = chess.BLACK,
) -> dict[str, list[dict[str, Any]]]:
    """Convert board to per-piece representation grouped by ownership.

    @brief Build structured piece list from board state.
    @version 1
    """
    your: list[dict[str, Any]] = []
    opponent: list[dict[str, Any]] = []

    for sq in chess.SQUARES:
        piece = board.piece_at(sq)
        if piece is None:
            continue
        entry: dict[str, Any] = {
            "square": chess.square_name(sq),
            "piece": chess.piece_name(piece.piece_type),
        }
        if piece.color == ai_color:
            if board.turn == ai_color:
                moves = [m.uci() for m in board.legal_moves if m.from_square == sq]
                if moves:
                    entry["moves"] = moves
            your.append(entry)
        else:
            opponent.append(entry)

    return {"your_pieces": your, "opponent_pieces": opponent}


def format_board_text(board: chess.Board) -> str:
    """Labeled ASCII board for human display.

    @brief Render board as ASCII art with file/rank labels.
    @version 1
    """
    header = "    " + " ".join("abcdefgh")
    rows: list[str] = []
    for rank_idx in range(7, -1, -1):
        cells: list[str] = []
        for file_idx in range(8):
            sq = chess.square(file_idx, rank_idx)
            piece = board.piece_at(sq)
            cells.append(piece.symbol() if piece else ".")
        rows.append(f"  {rank_idx + 1} {' '.join(cells)}")
    return "\n".join([header, *rows])
