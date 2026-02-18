"""Engine wiring — connects entropi orchestrator, server manager, and agent engine.

Demonstrates the full library setup flow:
    1. Load config (EntropyConfig from YAML)
    2. Create orchestrator with custom tiers and backend factory
    3. Create and register a custom MCP server (ChessServer)
    4. Wire the agent engine with LoopConfig
"""

from __future__ import annotations

import logging
from dataclasses import dataclass

import chess
from chess_server import ChessServer
from config import EXAMPLE_ROOT, load_config
from entropi import (
    AgentEngine,
    EngineCallbacks,
    LoopConfig,
    ModelOrchestrator,
    ServerManager,
    setup_logging,
    setup_model_logger,
)
from tier import ALL_TIERS

logger = logging.getLogger(__name__)


@dataclass
class ChessEngine:
    """Wired entropi components for chess play."""

    orchestrator: ModelOrchestrator
    server_manager: ServerManager
    agent_engine: AgentEngine
    chess_server: ChessServer


async def create_engine() -> ChessEngine:
    """Wire up entropi from config + tiers + server.

    Loads config from config.yaml, initializes the orchestrator with
    three chess tiers (suggest/validate/execute), registers the chess
    MCP server, and returns a ready-to-play ChessEngine.

    Returns:
        Fully initialized ChessEngine.
    """
    config = load_config()

    # Logging: session.log and session_model.log in .pychess/
    setup_logging(config, project_dir=EXAMPLE_ROOT, app_dir_name=".pychess")
    setup_model_logger(project_dir=EXAMPLE_ROOT, app_dir_name=".pychess")

    # Orchestrator: manages model loading, routing, tier swapping.
    # ALL_TIERS provides the suggest/validate/execute tier definitions.
    # backend_factory=None uses the default LlamaCppBackend.
    orchestrator = ModelOrchestrator(config, tiers=ALL_TIERS)
    await orchestrator.initialize()

    # MCP server: ChessServer exposes get_board and make_move tools.
    # register_server() must be called BEFORE server_manager.initialize().
    chess_server = ChessServer()
    tier_names = [t.name for t in ALL_TIERS]
    server_manager = ServerManager(config, tier_names=tier_names)
    server_manager.register_server(chess_server)
    await server_manager.initialize()

    # Agent engine: agentic loop that routes, generates, executes tools,
    # and handles handoffs between tiers automatically.
    loop_config = LoopConfig(auto_approve_tools=True)
    agent_engine = AgentEngine(orchestrator, server_manager, config, loop_config)

    # Wire callbacks: stream to dedicated file, tier changes to session log
    stream_path = EXAMPLE_ROOT / ".pychess" / "session_stream.log"
    stream_path.parent.mkdir(parents=True, exist_ok=True)
    stream_file = open(stream_path, "w")  # noqa: SIM115

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
    )


def _build_board_context(board: chess.Board) -> str:
    """Format current board state and move history for the system prompt."""
    legal = [m.uci() for m in board.legal_moves]
    moves = [m.uci() for m in board.move_stack]

    lines = [
        "## Current Position",
        "",
        f"```\n{board}\n```",
        "",
        f"**FEN:** `{board.fen()}`",
        f"**Move:** {board.fullmove_number}",
        f"**Side to move:** {'Black (you)' if board.turn == chess.BLACK else 'White (opponent)'}",
        f"**Legal moves:** {', '.join(legal)}",
    ]

    if moves:
        lines.append(f"**Move history:** {' '.join(moves)}")

    return "\n".join(lines)


async def get_ai_move(engine: ChessEngine) -> str | None:
    """Run the agentic loop — LLM sees board, calls tools, plays a move.

    The engine handles the full suggest → validate → execute handoff
    chain internally. From the caller's perspective, this is a single
    call that returns when the AI has made its move.

    Args:
        engine: Initialized ChessEngine.

    Returns:
        UCI string of the move played, or None if the AI failed to move.
    """
    ply_before = len(engine.chess_server.board.move_stack)
    board_context = _build_board_context(engine.chess_server.board)

    prompt = "It's your turn (Black). Analyze the position and play your move."

    async for msg in engine.agent_engine.run(prompt, system_prompt=board_context):
        logger.debug("Engine message: role=%s content=%s", msg.role, msg.content[:80])

    # Check if a new move was actually made
    if len(engine.chess_server.board.move_stack) > ply_before:
        return engine.chess_server.board.move_stack[-1].uci()

    logger.warning("AI did not make a move (ply unchanged: %d)", ply_before)
    return None


async def shutdown(engine: ChessEngine) -> None:
    """Clean shutdown of entropi components."""
    await engine.server_manager.shutdown()
    await engine.orchestrator.shutdown()
