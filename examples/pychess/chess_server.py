"""Chess MCP server — wraps python-chess board state."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import chess
from entropic import BaseMCPServer, BaseTool

# Tool JSON definitions live alongside default_config.yaml
_TOOLS_DIR = Path(__file__).parent / "data" / "tools"


def board_to_pieces(
    board: chess.Board,
    ai_color: chess.Color = chess.BLACK,
) -> dict[str, list[dict[str, Any]]]:
    """Convert board to per-piece representation grouped by ownership.

    Returns::

        {
            "your_pieces": [{"square": "g8", "piece": "knight", "moves": [...]}, ...],
            "opponent_pieces": [{"square": "e4", "piece": "pawn"}, ...]
        }

    ``moves`` is only present on the side-to-move's pieces that have legal moves.
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
    """Labeled ASCII board for human display (terminal / logs).

    Returns a string like::

          a b c d e f g h
        8 r n b q k b n r
        7 p p p p p p p p
        ...
        1 R N B Q K B N R
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


class MakeMoveTool(BaseTool):
    """Validate and push a UCI move onto the board."""

    def __init__(self, board: chess.Board, ai_color: chess.Color) -> None:
        super().__init__("make_move", "chess", _TOOLS_DIR)
        self._board = board
        self._ai_color = ai_color

    async def execute(self, arguments: dict[str, Any]) -> str:
        """Parse UCI string, validate legality, push move."""
        error = self._validate_move(arguments)
        if error:
            return error

        move = chess.Move.from_uci(arguments.get("move", ""))
        self._board.push(move)
        return json.dumps(
            {
                "status": "ok",
                "move": move.uci(),
                "move_number": self._board.fullmove_number,
            }
        )

    def _validate_move(self, arguments: dict[str, Any]) -> str | None:
        """Check turn, UCI syntax, and legality. Returns error JSON or None."""
        if self._board.turn != self._ai_color:
            side = "White" if self._ai_color == chess.WHITE else "Black"
            return json.dumps(
                {"error": f"Not your turn. You are {side}, but it is the opponent's turn."}
            )

        uci = arguments.get("move", "")
        error = self._check_move_valid(uci)
        if error:
            return json.dumps(error)
        return None

    def _check_move_valid(self, uci: str) -> dict[str, Any] | None:
        """Parse and validate a UCI move string. Returns error dict or None."""
        try:
            move = chess.Move.from_uci(uci)
        except (chess.InvalidMoveError, ValueError):
            return {"error": f"Invalid UCI string: '{uci}'"}

        if move not in self._board.legal_moves:
            legal = [m.uci() for m in self._board.legal_moves]
            return {"error": f"Illegal move: '{uci}'", "legal_moves": legal}
        return None


class ChessServer(BaseMCPServer):
    """MCP server exposing chess board tools.

    Maintains a shared ``chess.Board`` instance. Both the human
    player (via ``main.py``) and the AI (via tool calls) mutate
    the same board.
    """

    def __init__(self, ai_color: chess.Color = chess.BLACK) -> None:
        """Initialize chess server with a fresh board.

        Args:
            ai_color: The color the AI plays. ``make_move`` rejects
                calls when it's not this color's turn.
        """
        super().__init__("chess")
        self.board = chess.Board()
        self.register_tool(MakeMoveTool(self.board, ai_color))
