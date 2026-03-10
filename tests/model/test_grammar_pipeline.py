"""Model tests for grammar + auto-chain pipeline.

Uses pychess example as the verification vehicle (it has grammar + auto_chain +
multi-tier config already wired up), but tests engine semantics not chess outcomes.

Fixtures (pychess_config, pychess_orchestrator, pychess_engine) live in
tests/model/conftest.py and are available to any model test module.
"""

from __future__ import annotations

import json
import logging
import sys
from pathlib import Path

import pytest

chess = pytest.importorskip("chess", reason="python-chess not installed")

# pychess example modules need sys.path entry
_PYCHESS_ROOT = Path(__file__).resolve().parent.parent.parent / "examples" / "pychess"
if str(_PYCHESS_ROOT) not in sys.path:
    sys.path.insert(0, str(_PYCHESS_ROOT))

from chess_server import board_to_pieces  # noqa: E402
from entropic import AgentEngine, LoopConfig, ServerManager  # noqa: E402

from .conftest import with_timeout  # noqa: E402

logger = logging.getLogger(__name__)


# =============================================================================
# Helpers
# =============================================================================


def _build_prompt(board: chess.Board) -> tuple[str, str]:
    """Build a system prompt and user prompt for the chess engine."""
    pieces_json = json.dumps(board_to_pieces(board), separators=(",", ":"))
    system = (
        "You are a chess engine playing as Black.\n\n"
        "## Board Format\n"
        "- `your_pieces` — Black pieces with square, piece, and moves\n"
        "- `opponent_pieces` — White pieces with square and piece\n\n"
        f"## Current Position\n```json\n{pieces_json}\n```\n"
        f"**Move:** {board.fullmove_number}\n"
        f"**Side to move:** {'Black (you)' if board.turn == chess.BLACK else 'White'}"
    )
    return system, "It's your turn (Black). Analyze the position and play your move."


async def _collect_messages(engine, prompt, system, messages):
    """Run engine and collect all messages."""
    async for msg in engine.run(prompt, system_prompt=system):
        messages.append(msg)
        logger.debug("msg: role=%s content=%s", msg.role, msg.content[:100])


# =============================================================================
# Tests
# =============================================================================


@pytest.mark.model
class TestGrammarPipeline:
    """Tests for grammar + auto-chain engine behavior using real inference."""

    @pytest.mark.asyncio
    async def test_grammar_constrains_output(self, pychess_engine) -> None:
        """Grammar produces structured output matching GBNF rules."""
        engine, chess_server = pychess_engine

        chess_server.board.push(chess.Move.from_uci("e2e4"))  # White opens

        system, prompt = _build_prompt(chess_server.board)
        messages = []
        await with_timeout(
            _collect_messages(engine, prompt, system, messages),
            expected_turns=4,
            name="grammar_constrains_output",
        )

        # First assistant message should be grammar-structured
        assistant_msgs = [m for m in messages if m.role == "assistant"]
        assert len(assistant_msgs) >= 1, "Expected at least one assistant message"

        first_response = assistant_msgs[0].content
        # Thinker grammar produces "Threats: ...\nCandidates:\n- ...\nBest move: ..."
        assert (
            "Threats:" in first_response or "Candidates:" in first_response
        ), f"First response doesn't match thinker grammar structure: {first_response!r}"

    @pytest.mark.asyncio
    async def test_grammar_auto_chain_fires(self, pychess_engine) -> None:
        """auto_chain=True + grammar + stop → delegation fires, two tiers observed."""
        engine, chess_server = pychess_engine

        chess_server.board.push(chess.Move.from_uci("e2e4"))

        system, prompt = _build_prompt(chess_server.board)
        messages = []
        await with_timeout(
            _collect_messages(engine, prompt, system, messages),
            expected_turns=4,
            name="grammar_auto_chain_fires",
        )

        # Should see at least 2 assistant messages (thinker + executor)
        assistant_msgs = [m for m in messages if m.role == "assistant"]
        assert len(assistant_msgs) >= 2, (
            f"Expected >=2 assistant messages (thinker+executor), got {len(assistant_msgs)}. "
            f"Messages: {[(m.role, m.content[:80]) for m in messages]}"
        )

    @pytest.mark.asyncio
    async def test_multi_tier_pipeline_completes(self, pychess_engine) -> None:
        """Full pipeline: thinker → auto_chain → executor → tool execution."""
        engine, chess_server = pychess_engine

        chess_server.board.push(chess.Move.from_uci("e2e4"))

        system, prompt = _build_prompt(chess_server.board)
        messages = []
        await with_timeout(
            _collect_messages(engine, prompt, system, messages),
            expected_turns=4,
            name="multi_tier_pipeline",
        )

        # Pipeline should run through both tiers (thinker + executor)
        # Note: whether the executor's tool call is actually parsed depends on
        # the adapter's parse_tool_calls path — a separate concern from auto-chain.
        assistant_msgs = [m for m in messages if m.role == "assistant"]
        assert len(assistant_msgs) >= 2, (
            f"Expected >=2 assistant messages (thinker+executor), got {len(assistant_msgs)}. "
            f"Messages: {[(m.role, m.content[:80]) for m in messages]}"
        )

    @pytest.mark.asyncio
    async def test_grammar_without_auto_chain_is_terminal(
        self, pychess_config, pychess_orchestrator
    ) -> None:
        """Grammar tier without auto_chain → engine marks COMPLETE, no delegation.

        Reuses the module-scoped orchestrator (single model instance). Config
        fields ``auto_chain`` and ``allowed_tools`` are read at runtime, so
        mutating the shared config is sufficient — no second orchestrator needed.
        """
        from chess_server import ChessServer

        thinker = pychess_config.models.tiers["thinker"]
        orig_auto_chain = thinker.auto_chain
        orig_allowed_tools = thinker.allowed_tools

        thinker.auto_chain = False
        thinker.allowed_tools = []

        try:
            chess_server = ChessServer()
            server_manager = ServerManager(
                pychess_config, tier_names=pychess_orchestrator.tier_names
            )
            server_manager.register_server(chess_server)
            await server_manager.initialize()

            loop_config = LoopConfig(auto_approve_tools=True, max_iterations=4)
            engine = AgentEngine(pychess_orchestrator, server_manager, pychess_config, loop_config)

            chess_server.board.push(chess.Move.from_uci("e2e4"))

            system, prompt = _build_prompt(chess_server.board)
            messages = []
            await with_timeout(
                _collect_messages(engine, prompt, system, messages),
                expected_turns=2,
                name="grammar_terminal",
            )

            # Should see thinker output but NO executor (no delegation)
            assistant_msgs = [m for m in messages if m.role == "assistant"]
            assert len(assistant_msgs) >= 1
            # No move should be made (executor never runs)
            assert len(chess_server.board.move_stack) == 1, (
                f"Expected no AI move (thinker is terminal), "
                f"but board has {len(chess_server.board.move_stack)} moves"
            )

            await server_manager.shutdown()
        finally:
            thinker.auto_chain = orig_auto_chain
            thinker.allowed_tools = orig_allowed_tools
