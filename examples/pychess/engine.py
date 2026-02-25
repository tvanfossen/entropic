"""Engine wiring — connects entropic orchestrator, server manager, and agent engine.

Two-tier chess engine: thinker analyzes → auto-chain → executor plays.
Same .gguf, zero VRAM swap.
"""

from __future__ import annotations

import json
import logging
from dataclasses import dataclass
from typing import IO

import chess
from chess_server import ChessServer, board_to_pieces
from config import EXAMPLE_ROOT, load_config
from entropic import (
    AgentEngine,
    EngineCallbacks,
    LoopConfig,
    ModelOrchestrator,
    ServerManager,
    setup_logging,
    setup_model_logger,
)

logger = logging.getLogger(__name__)


@dataclass
class ChessEngine:
    """Wired entropic components for chess play."""

    orchestrator: ModelOrchestrator
    server_manager: ServerManager
    agent_engine: AgentEngine
    chess_server: ChessServer
    _stream_file: IO[str] | None = None


async def create_engine() -> ChessEngine:
    """Wire up entropic from config + single player tier + chess server.

    Returns:
        Fully initialized ChessEngine.
    """
    config = load_config()

    # Logging: session.log and session_model.log in .pychess/
    setup_logging(config, project_dir=EXAMPLE_ROOT, app_dir_name=".pychess")
    setup_model_logger(project_dir=EXAMPLE_ROOT, app_dir_name=".pychess")

    # Orchestrator: manages model loading for the player tier.
    orchestrator = ModelOrchestrator(config)
    await orchestrator.initialize()

    # MCP server: ChessServer exposes make_move tool.
    # register_server() must be called BEFORE server_manager.initialize().
    chess_server = ChessServer()
    server_manager = ServerManager(config, tier_names=orchestrator.tier_names)
    server_manager.register_server(chess_server)
    await server_manager.initialize()

    # Agent engine: agentic loop that generates and executes tool calls.
    loop_config = LoopConfig(auto_approve_tools=True, max_iterations=5)
    agent_engine = AgentEngine(orchestrator, server_manager, config, loop_config)

    # Wire callbacks: stream to dedicated file
    stream_path = EXAMPLE_ROOT / ".pychess" / "session_stream.log"
    stream_path.parent.mkdir(parents=True, exist_ok=True)
    stream_file = stream_path.open("w")

    col = 0
    wrap_at = 100

    def _write_stream_chunk(chunk: str) -> None:
        nonlocal col
        for ch in chunk:
            if ch == "\n":
                stream_file.write(ch)
                col = 0
            elif col >= wrap_at and ch == " ":
                stream_file.write("\n")
                col = 0
            else:
                stream_file.write(ch)
                col += 1
        stream_file.flush()

    callbacks = EngineCallbacks(
        on_stream_chunk=_write_stream_chunk,
        on_tier_selected=lambda tier: logger.info("[TIER] %s", tier),
    )
    agent_engine.set_callbacks(callbacks)

    return ChessEngine(
        orchestrator=orchestrator,
        server_manager=server_manager,
        agent_engine=agent_engine,
        chess_server=chess_server,
        _stream_file=stream_file,
    )


def _annotate_move_history(board: chess.Board) -> str:
    """Replay moves from the start and annotate each with piece name and color.

    Returns a string like::

        1. White pawn e2e4, Black pawn d7d5
        2. White pawn e4d5, Black queen d8d7
    """
    replay = chess.Board()
    entries: list[str] = []
    for move in board.move_stack:
        piece = replay.piece_at(move.from_square)
        color = "White" if replay.turn == chess.WHITE else "Black"
        name = chess.piece_name(piece.piece_type) if piece else "?"
        entries.append(f"{color} {name} {move.uci()}")
        replay.push(move)

    # Group into full-move pairs: "1. White ... , Black ..."
    lines: list[str] = []
    for i in range(0, len(entries), 2):
        move_num = (i // 2) + 1
        pair = entries[i : i + 2]
        lines.append(f"{move_num}. {', '.join(pair)}")
    return "\n".join(lines)


def _build_board_context(board: chess.Board) -> str:
    """Format current board state for the system prompt.

    Includes board format legend so identity prompts stay role-focused.
    Uses per-piece representation grouped by ownership so the model
    never has to infer piece type or color from notation.
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


async def get_ai_move(engine: ChessEngine) -> str | None:
    """Run the agentic loop — LLM analyzes the position and plays a move.

    Single generation: the player tier calls get_board, picks a move,
    and calls make_move.

    Returns:
        UCI string of the move played, or None if the AI failed to move.
    """
    ply_before = len(engine.chess_server.board.move_stack)
    board_context = _build_board_context(engine.chess_server.board)

    prompt = "It's your turn (Black). Analyze the position and play your move."

    async for msg in engine.agent_engine.run(prompt, system_prompt=board_context):
        logger.debug("Engine message: role=%s content=%s", msg.role, msg.content[:80])
        if len(engine.chess_server.board.move_stack) > ply_before:
            break

    # Check if a new move was actually made
    if len(engine.chess_server.board.move_stack) > ply_before:
        return engine.chess_server.board.move_stack[-1].uci()

    logger.warning("AI did not make a move (ply unchanged: %d)", ply_before)
    return None


async def shutdown(engine: ChessEngine) -> None:
    """Clean shutdown of entropic components."""
    if engine._stream_file:
        engine._stream_file.close()
    await engine.server_manager.shutdown()
    await engine.orchestrator.shutdown()
