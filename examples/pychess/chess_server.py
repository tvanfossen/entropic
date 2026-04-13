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

import json
import sys
from typing import Any

import chess


## @brief Build structured piece list from board state.
## @utility
## @return Dict with your_pieces and opponent_pieces lists.
## @version 1
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


## @brief Render board as ASCII art with file/rank labels.
## @utility
## @return Multi-line ASCII board string.
## @version 1
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


# ── MCP Stdio Server ────────────────────────────────

_MCP_TOOLS: list[dict[str, str]] = [
    {
        "name": "make_move",
        "description": "Play a move on the board using UCI notation (e.g. 'e7e5').",
    },
]


## @brief Parse newline-delimited JSON-RPC request.
## @utility
## @return Parsed dict or None on EOF.
## @version 1
def _read_jsonrpc() -> dict[str, Any] | None:
    """Read one JSON-RPC 2.0 message from stdin.

    @brief Parse newline-delimited JSON-RPC request.
    @version 1
    """
    line = sys.stdin.readline()
    if not line:
        return None
    return json.loads(line.strip())


## @brief Send newline-delimited JSON-RPC response.
## @utility
## @version 1
def _write_jsonrpc(response: dict[str, Any]) -> None:
    """Write one JSON-RPC 2.0 message to stdout.

    @brief Send newline-delimited JSON-RPC response.
    @version 1
    """
    sys.stdout.write(json.dumps(response, separators=(",", ":")) + "\n")
    sys.stdout.flush()


## @brief Write result envelope.
## @utility
## @version 1
def _jsonrpc_ok(rid: int | None, result: dict[str, Any]) -> None:
    """Send a JSON-RPC success response.

    @brief Write result envelope.
    @version 1
    """
    _write_jsonrpc({"jsonrpc": "2.0", "id": rid, "result": result})


## @brief Write error envelope.
## @utility
## @version 1
def _jsonrpc_err(rid: int | None, code: int, message: str) -> None:
    """Send a JSON-RPC error response.

    @brief Write error envelope.
    @version 1
    """
    _write_jsonrpc({"jsonrpc": "2.0", "id": rid, "error": {"code": code, "message": message}})


## @brief Dispatch tool call. Stateless — acknowledges moves without board tracking.
## @utility
## @version 1
def _handle_tools_call(rid: int | None, params: dict[str, Any]) -> None:
    """Handle a tools/call request.

    @brief Dispatch tool call. Stateless — acknowledges moves without board tracking.
    @version 1
    """
    name = params.get("name", "")
    args = params.get("arguments", {})
    if name == "make_move":
        move = args.get("move", "")
        _jsonrpc_ok(rid, {"content": [{"type": "text", "text": f"Move applied: {move}"}]})
    else:
        _jsonrpc_err(rid, -32601, f"Unknown tool: {name}")


## @brief Route method to handler, send response.
## @utility
## @version 1
def _dispatch(request: dict[str, Any]) -> None:
    """Dispatch a JSON-RPC request to the appropriate handler.

    @brief Route method to handler, send response.
    @version 1
    """
    method = request.get("method", "")
    params = request.get("params", {})
    rid = request.get("id")
    handlers: dict[str, Any] = {
        "initialize": lambda: _jsonrpc_ok(
            rid,
            {
                "protocolVersion": "2024-11-05",
                "serverInfo": {"name": "chess"},
                "capabilities": {},
            },
        ),
        "tools/list": lambda: _jsonrpc_ok(rid, {"tools": _MCP_TOOLS}),
        "tools/call": lambda: _handle_tools_call(rid, params),
    }
    handler = handlers.get(method)
    if handler:
        handler()
    else:
        _jsonrpc_err(rid, -32601, f"Unknown method: {method}")


## @brief Main MCP server loop.
## @utility
## @version 1
def serve_stdio() -> None:
    """Run the MCP server loop over stdio transport.

    Reads newline-delimited JSON-RPC 2.0 requests from stdin,
    dispatches to handlers, writes responses to stdout.

    @brief Main MCP server loop.
    @version 1
    """
    while True:
        request = _read_jsonrpc()
        if request is None:
            break
        _dispatch(request)


if __name__ == "__main__":
    serve_stdio()
