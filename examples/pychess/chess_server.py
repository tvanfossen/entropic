"""Chess MCP server — wraps python-chess board state."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import chess
from entropi import BaseMCPServer, BaseTool

# Tool JSON definitions live alongside default_config.yaml
_TOOLS_DIR = Path(__file__).parent / "data" / "tools"


class GetBoardTool(BaseTool):
    """Return current board state as JSON."""

    def __init__(self, board: chess.Board) -> None:
        super().__init__("get_board", "chess", _TOOLS_DIR)
        self._board = board

    async def execute(self, arguments: dict[str, Any]) -> str:
        """Return board ASCII, FEN, side to move, and legal moves."""
        legal = [m.uci() for m in self._board.legal_moves]
        return json.dumps(
            {
                "board": str(self._board),
                "fen": self._board.fen(),
                "turn": "white" if self._board.turn == chess.WHITE else "black",
                "legal_moves": legal,
                "move_number": self._board.fullmove_number,
            }
        )


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
        legal = [m.uci() for m in self._board.legal_moves]
        return json.dumps(
            {
                "board": str(self._board),
                "fen": self._board.fen(),
                "turn": "white" if self._board.turn == chess.WHITE else "black",
                "legal_moves": legal,
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
        self.register_tool(GetBoardTool(self.board))
        self.register_tool(MakeMoveTool(self.board, ai_color))
