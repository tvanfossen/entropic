"""Delegation manager for child inference loops.

Spawns a fresh LoopContext for a target tier, runs the engine loop
to completion, and returns the final assistant message to the parent.
"""

from __future__ import annotations

import uuid
from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Any

from entropic.core.base import Message
from entropic.core.engine_types import AgentState, LoopConfig, LoopContext
from entropic.core.logging import get_logger

if TYPE_CHECKING:
    from entropic.core.engine import AgentEngine

logger = get_logger("core.delegation")


@dataclass
class DelegationResult:
    """Result returned from a child delegation loop."""

    summary: str
    success: bool
    target_tier: str
    task: str
    turns_used: int
    child_messages: list[Message] = field(default_factory=list)


class DelegationManager:
    """Orchestrates child loop creation and execution.

    Builds a fresh LoopContext for the target tier, runs the engine's
    _loop() to completion, and extracts the final assistant message
    as the delegation result.
    """

    def __init__(self, engine: AgentEngine) -> None:
        self._engine = engine

    async def execute_delegation(
        self,
        parent_ctx: LoopContext,
        target_tier: str,
        task: str,
        max_turns: int | None = None,
    ) -> DelegationResult:
        """Run a child inference loop for the target tier.

        1. Resolve tier via orchestrator
        2. Build child LoopContext (fresh messages, locked to target tier)
        3. Build system prompt for target tier
        4. Inject task as user message
        5. Run engine._loop(child_ctx) to completion
        6. Extract final assistant message
        7. Return DelegationResult
        """
        tier = self._engine.orchestrator._find_tier(target_tier)
        if tier is None:
            return DelegationResult(
                summary=f"Error: unknown tier '{target_tier}'",
                success=False,
                target_tier=target_tier,
                task=task,
                turns_used=0,
            )

        child_ctx = self._build_child_context(parent_ctx, tier, task)

        logger.info(
            "[DELEGATION] Starting child loop: tier=%s task=%s max_turns=%s depth=%d",
            target_tier,
            task[:80],
            max_turns,
            child_ctx.delegation_depth,
        )

        # Save engine state that child might mutate
        saved_anchors = dict(self._engine._context_anchors)
        saved_loop_config = self._engine.loop_config
        saved_todo_list = self._save_todo_list()

        # Override max_iterations for child if max_turns specified
        if max_turns is not None:
            self._engine.loop_config = LoopConfig(
                max_iterations=max_turns,
                max_consecutive_errors=saved_loop_config.max_consecutive_errors,
                max_tool_calls_per_turn=saved_loop_config.max_tool_calls_per_turn,
                stream_output=saved_loop_config.stream_output,
                auto_approve_tools=saved_loop_config.auto_approve_tools,
            )

        try:
            async for _msg in self._engine._loop(child_ctx):
                pass  # Consume all messages — child runs to completion
        except Exception as e:
            logger.error("[DELEGATION] Child loop error: %s", e)
            return DelegationResult(
                summary=f"Delegation failed: {e}",
                success=False,
                target_tier=target_tier,
                task=task,
                turns_used=child_ctx.metrics.iterations,
                child_messages=child_ctx.messages,
            )
        finally:
            # Restore engine state
            self._engine._context_anchors = saved_anchors
            self._engine.loop_config = saved_loop_config
            self._restore_todo_list(saved_todo_list)

        summary = self._extract_final_summary(child_ctx)
        success = child_ctx.state == AgentState.COMPLETE

        logger.info(
            "[DELEGATION] Child loop complete: tier=%s turns=%d success=%s summary_len=%d",
            target_tier,
            child_ctx.metrics.iterations,
            success,
            len(summary),
        )

        return DelegationResult(
            summary=summary,
            success=success,
            target_tier=target_tier,
            task=task,
            turns_used=child_ctx.metrics.iterations,
            child_messages=child_ctx.messages,
        )

    def _build_child_context(
        self,
        parent_ctx: LoopContext,
        tier: Any,
        task: str,
    ) -> LoopContext:
        """Build a fresh LoopContext for the child delegation."""
        rg = self._engine._response_generator

        child_ctx = LoopContext()
        child_ctx.delegation_depth = parent_ctx.delegation_depth + 1
        child_ctx.parent_conversation_id = str(uuid.uuid4())
        child_ctx.locked_tier = tier
        child_ctx.all_tools = parent_ctx.all_tools
        child_ctx.base_system = parent_ctx.base_system

        # Build system prompt for target tier
        system_prompt = rg._build_formatted_system_prompt(tier, child_ctx)
        child_ctx.messages = [
            Message(role="system", content=system_prompt),
            Message(role="user", content=task),
        ]

        return child_ctx

    def _extract_final_summary(self, child_ctx: LoopContext) -> str:
        """Extract the final assistant message from the child context."""
        for msg in reversed(child_ctx.messages):
            if msg.role == "assistant" and msg.content.strip():
                return msg.content
        return "(No response from delegate)"

    def _save_todo_list(self) -> Any:
        """Save the current TodoList state from EntropicServer."""
        server = self._get_entropic_server()
        if server is None:
            return None
        return server._todo_list.to_dict()

    def _restore_todo_list(self, saved: Any) -> None:
        """Restore TodoList state to EntropicServer."""
        if saved is None:
            return
        server = self._get_entropic_server()
        if server is None:
            return
        from entropic.core.todos import TodoList

        server._todo_list = TodoList.from_dict(saved)

    def _get_entropic_server(self) -> Any:
        """Get the EntropicServer from ServerManager (if available)."""
        sm = self._engine.server_manager
        if sm is None:
            return None
        # ServerManager wraps servers in InProcessProvider via _clients
        client = sm._clients.get("entropic")
        if client is None:
            return None
        return getattr(client, "_server", None)
