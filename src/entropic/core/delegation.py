"""Delegation manager for child inference loops.

Spawns a fresh LoopContext for a target tier, runs the engine loop
to completion, and returns the final assistant message to the parent.
"""

from __future__ import annotations

import uuid
from dataclasses import dataclass, field
from pathlib import Path
from typing import TYPE_CHECKING, Any

from entropic.core.base import Message, StorageBackend
from entropic.core.engine_types import AgentState, LoopConfig, LoopContext
from entropic.core.logging import get_logger
from entropic.core.worktree import WorktreeInfo, WorktreeManager, scoped_worktree

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

    def __init__(
        self,
        engine: AgentEngine,
        storage: StorageBackend | None = None,
        repo_dir: Path | None = None,
    ) -> None:
        self._engine = engine
        self._storage = storage
        self._repo_dir = repo_dir
        self._worktree_mgr: WorktreeManager | None = None
        self._shared_worktree: WorktreeInfo | None = None
        if repo_dir is not None:
            self._worktree_mgr = WorktreeManager(repo_dir)

    async def execute_delegation(
        self,
        parent_ctx: LoopContext,
        target_tier: str,
        task: str,
        max_turns: int | None = None,
        parent_conversation_id: str | None = None,
    ) -> DelegationResult:
        """Run a child inference loop for the target tier.

        1. Resolve tier via orchestrator
        2. Persist delegation record (if storage available)
        3. Build child LoopContext (fresh messages, locked to target tier)
        4. Run engine._loop(child_ctx) to completion
        5. Persist child messages + complete delegation record
        6. Return DelegationResult
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

        delegating_tier = str(parent_ctx.locked_tier) if parent_ctx.locked_tier else "unknown"
        delegation_id, child_conv_id = await self._create_storage_records(
            parent_conversation_id, delegating_tier, target_tier, task, max_turns
        )

        child_ctx = self._build_child_context(parent_ctx, tier, task)

        logger.info(
            "[DELEGATION] Starting child loop: tier=%s task=%s max_turns=%s depth=%d",
            target_tier,
            task[:80],
            max_turns,
            child_ctx.delegation_depth,
        )

        result = await self._run_child_loop(child_ctx, target_tier, task, max_turns)

        # Persist child messages and mark delegation complete
        await self._complete_storage_records(delegation_id, child_conv_id, result)

        return result

    async def execute_pipeline(
        self,
        parent_ctx: LoopContext,
        stages: list[str],
        task: str,
        parent_conversation_id: str | None = None,
    ) -> DelegationResult:
        """Run a multi-stage delegation pipeline sequentially.

        Each stage gets the original task (unchanged) plus a pipeline context
        line showing its position.  Artifacts pass between stages via the
        shared worktree — not via text accumulation in the task prompt.

        A single worktree is created for the entire pipeline. All stages
        share it so files written by stage N are visible to stage N+1.
        Merged on pipeline success, discarded on failure.
        """
        pipeline_id = str(uuid.uuid4())
        wt_info = await self._create_worktree(pipeline_id, f"pipeline-{stages[0]}")
        self._shared_worktree = wt_info
        pipeline_result: DelegationResult | None = None
        stage_labels = " → ".join(stages)

        try:
            for i, stage in enumerate(stages):
                context_line = (
                    f"[PIPELINE CONTEXT] Stage {i + 1} of {len(stages)}: {stage_labels}\n"
                    f"You are: {stage}. Stay within your role's scope — do NOT do work "
                    f"that belongs to a later stage in the pipeline. Your output is the "
                    f"input for the next stage.\n\n"
                )
                stage_task = f"{context_line}{task}"

                result = await self.execute_delegation(
                    parent_ctx,
                    stage,
                    stage_task,
                    parent_conversation_id=parent_conversation_id,
                )

                if not result.success:
                    logger.warning("[PIPELINE] Stage '%s' failed: %s", stage, result.summary[:100])
                    pipeline_result = result
                    return result

                pipeline_result = result
        finally:
            self._shared_worktree = None
            # Always merge partial work — completed stages produce valuable
            # artifacts (specs, configs) that shouldn't be discarded because
            # a later stage failed or was interrupted.
            if wt_info is not None and self._worktree_mgr is not None:
                merged = await self._worktree_mgr.merge_worktree(wt_info)
                if not merged:
                    logger.error(
                        "[PIPELINE] Worktree merge FAILED — partial work may be lost. "
                        "Branch %s preserved for manual recovery.",
                        wt_info.branch,
                    )

        # Should always have at least one result (stages has minItems=2)
        assert pipeline_result is not None
        return pipeline_result

    async def _run_child_loop(
        self,
        child_ctx: LoopContext,
        target_tier: str,
        task: str,
        max_turns: int | None,
    ) -> DelegationResult:
        """Run the child loop with engine state save/restore.

        Uses ``_shared_worktree`` (set by pipeline) when available.
        Otherwise creates a per-delegation worktree.
        """
        saved_anchors = dict(self._engine._context_anchors)
        saved_loop_config = self._engine.loop_config
        saved_todo_list = self._save_todo_list()
        self._install_fresh_todo_list()

        if max_turns is not None:
            self._engine.loop_config = LoopConfig(
                max_iterations=max_turns,
                max_consecutive_errors=saved_loop_config.max_consecutive_errors,
                max_tool_calls_per_turn=saved_loop_config.max_tool_calls_per_turn,
                stream_output=saved_loop_config.stream_output,
                auto_approve_tools=saved_loop_config.auto_approve_tools,
            )

        # Use shared worktree (pipeline) or create per-delegation worktree
        owns_worktree = self._shared_worktree is None
        if owns_worktree:
            delegation_id = child_ctx.parent_conversation_id or str(uuid.uuid4())
            wt_info = await self._create_worktree(delegation_id, target_tier)
        else:
            wt_info = self._shared_worktree

        result: DelegationResult | None = None

        try:
            result = await self._execute_child_in_context(child_ctx, target_tier, task, wt_info)
        finally:
            self._engine._context_anchors = saved_anchors
            self._engine.loop_config = saved_loop_config
            self._restore_todo_list(saved_todo_list)
            # Only manage worktree lifecycle if we own it (not shared)
            if owns_worktree:
                if result is not None:
                    await self._finalize_worktree(wt_info, result)
                elif wt_info is not None and self._worktree_mgr is not None:
                    await self._worktree_mgr.discard_worktree(wt_info)

        assert result is not None  # _execute_child_in_context always returns
        return result

    async def _execute_child_in_context(
        self,
        child_ctx: LoopContext,
        target_tier: str,
        task: str,
        wt_info: WorktreeInfo | None,
    ) -> DelegationResult:
        """Run the child loop, optionally inside a worktree scope."""
        if wt_info is not None:
            async with scoped_worktree(self._engine, wt_info.path):
                return await self._run_engine_loop(child_ctx, target_tier, task)
        return await self._run_engine_loop(child_ctx, target_tier, task)

    async def _run_engine_loop(
        self,
        child_ctx: LoopContext,
        target_tier: str,
        task: str,
    ) -> DelegationResult:
        """Execute engine._loop and build DelegationResult."""
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

    async def _create_worktree(self, delegation_id: str, tier: str) -> WorktreeInfo | None:
        """Create a worktree if manager is available."""
        if self._worktree_mgr is None:
            return None
        return await self._worktree_mgr.create_worktree(delegation_id, tier)

    async def _finalize_worktree(
        self,
        wt_info: WorktreeInfo | None,
        result: DelegationResult,
    ) -> None:
        """Merge or discard worktree based on delegation result."""
        if self._worktree_mgr is None or wt_info is None:
            return
        if result.success:
            merged = await self._worktree_mgr.merge_worktree(wt_info)
            if not merged:
                logger.error(
                    "[DELEGATION] Worktree merge FAILED — child work may be lost. "
                    "Branch %s preserved for manual recovery.",
                    wt_info.branch,
                )
        else:
            await self._worktree_mgr.discard_worktree(wt_info)

    async def _create_storage_records(
        self,
        parent_conversation_id: str | None,
        delegating_tier: str,
        target_tier: str,
        task: str,
        max_turns: int | None,
    ) -> tuple[str | None, str | None]:
        """Create delegation + child conversation in storage. Returns (delegation_id, child_conv_id)."""
        if not self._storage or not parent_conversation_id:
            return None, None
        try:
            return await self._storage.create_delegation(
                parent_conversation_id, delegating_tier, target_tier, task, max_turns
            )
        except Exception as e:
            logger.warning("[DELEGATION] Storage create failed (non-fatal): %s", e)
            return None, None

    async def _complete_storage_records(
        self,
        delegation_id: str | None,
        child_conv_id: str | None,
        result: DelegationResult,
    ) -> None:
        """Persist child messages and mark delegation complete."""
        if not self._storage:
            return
        try:
            if child_conv_id:
                await self._storage.save_conversation(child_conv_id, result.child_messages)
            if delegation_id:
                status = "completed" if result.success else "failed"
                await self._storage.complete_delegation(delegation_id, status, result.summary[:500])
        except Exception as e:
            logger.warning("[DELEGATION] Storage complete failed (non-fatal): %s", e)

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
        system_prompt += self._completion_instructions(tier)
        child_ctx.messages = [
            Message(role="system", content=system_prompt),
            Message(role="user", content=task),
        ]

        return child_ctx

    def _completion_instructions(self, tier: Any) -> str:
        """Build completion instructions for child delegation contexts.

        Checks the tier's identity frontmatter for explicit_completion flag.
        When enabled, instructs the child to use entropic.complete when done.
        When disabled, relies on auto-chain (finish_reason detection).
        """
        fm = self._engine.orchestrator._prompt_manager.get_identity_frontmatter(
            tier.name if hasattr(tier, "name") else str(tier)
        )
        use_explicit = getattr(fm, "explicit_completion", False) if fm else False

        if use_explicit:
            return (
                "\n\n## Delegation Completion\n"
                "You are operating as a delegated child context. When you have "
                "finished the assigned task, call `entropic.complete` with a "
                "summary of what was accomplished. This signals completion to "
                "the parent context.\n"
                "Do NOT simply stop responding — use the tool to signal you are done."
            )
        return ""

    def _extract_final_summary(self, child_ctx: LoopContext) -> str:
        """Extract the delegation summary from the child context.

        Prefers explicit completion summary (from entropic.complete tool)
        over the last assistant message.
        """
        explicit = child_ctx.metadata.get("explicit_completion_summary")
        if explicit:
            return explicit
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

    def _install_fresh_todo_list(self) -> None:
        """Install a fresh empty TodoList for the child delegation.

        Updates both the server's reference and all tool references
        that hold a pointer to the TodoList.
        """
        server = self._get_entropic_server()
        if server is None:
            return
        from entropic.core.todos import TodoList

        fresh = TodoList()
        server._todo_list = fresh
        # Update tool references that hold a pointer to the TodoList
        for tool in server._tool_registry._tools.values():
            if hasattr(tool, "_todo_list"):
                tool._todo_list = fresh

    def _restore_todo_list(self, saved: Any) -> None:
        """Restore TodoList state to EntropicServer."""
        if saved is None:
            return
        server = self._get_entropic_server()
        if server is None:
            return
        from entropic.core.todos import TodoList

        restored = TodoList.from_dict(saved)
        server._todo_list = restored
        for tool in server._tool_registry._tools.values():
            if hasattr(tool, "_todo_list"):
                tool._todo_list = restored

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
