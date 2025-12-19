"""
Agentic loop engine.

Implements the core plan→act→observe→repeat cycle with
proper state management and termination conditions.
"""

from __future__ import annotations

import asyncio
import time
from collections.abc import AsyncIterator, Callable
from dataclasses import dataclass, field
from enum import Enum, auto
from typing import TYPE_CHECKING, Any

from entropi.config.schema import EntropyConfig
from entropi.core.base import Message, ToolCall
from entropi.core.context import ContextBuilder
from entropi.core.logging import get_logger
from entropi.inference.orchestrator import ModelOrchestrator
from entropi.mcp.manager import PermissionDeniedError, ServerManager

if TYPE_CHECKING:
    pass

logger = get_logger("core.engine")


class AgentState(Enum):
    """Agent execution states."""

    IDLE = auto()
    PLANNING = auto()
    EXECUTING = auto()
    WAITING_TOOL = auto()
    VERIFYING = auto()
    COMPLETE = auto()
    ERROR = auto()
    INTERRUPTED = auto()


@dataclass
class LoopConfig:
    """Configuration for the agentic loop."""

    max_iterations: int = 15
    max_consecutive_errors: int = 3
    max_tool_calls_per_turn: int = 10
    idle_timeout_seconds: int = 300
    require_plan_for_complex: bool = True
    stream_output: bool = True
    auto_approve_tools: bool = False


@dataclass
class LoopMetrics:
    """Metrics collected during loop execution."""

    iterations: int = 0
    tool_calls: int = 0
    tokens_used: int = 0
    errors: int = 0
    start_time: float = 0.0
    end_time: float = 0.0

    @property
    def duration_ms(self) -> int:
        """Get duration in milliseconds."""
        return int((self.end_time - self.start_time) * 1000)


@dataclass
class LoopContext:
    """Context maintained during loop execution."""

    messages: list[Message] = field(default_factory=list)
    pending_tool_calls: list[ToolCall] = field(default_factory=list)
    state: AgentState = AgentState.IDLE
    metrics: LoopMetrics = field(default_factory=LoopMetrics)
    consecutive_errors: int = 0


class AgentEngine:
    """
    Core agent execution engine.

    Manages the agentic loop lifecycle including:
    - State transitions
    - Tool execution
    - Context management
    - Error recovery
    """

    def __init__(
        self,
        orchestrator: ModelOrchestrator,
        server_manager: ServerManager,
        config: EntropyConfig,
        loop_config: LoopConfig | None = None,
    ) -> None:
        """
        Initialize agent engine.

        Args:
            orchestrator: Model orchestrator
            server_manager: MCP server manager
            config: Application configuration
            loop_config: Loop-specific configuration
        """
        self.orchestrator = orchestrator
        self.server_manager = server_manager
        self.config = config
        self.loop_config = loop_config or LoopConfig()

        self._context_builder = ContextBuilder(config)
        self._interrupt_event = asyncio.Event()

        # Callbacks
        self._on_state_change: Callable[[AgentState], None] | None = None
        self._on_tool_call: Callable[[ToolCall], bool] | None = None
        self._on_stream_chunk: Callable[[str], None] | None = None

    def set_callbacks(
        self,
        on_state_change: Callable[[AgentState], None] | None = None,
        on_tool_call: Callable[[ToolCall], bool] | None = None,
        on_stream_chunk: Callable[[str], None] | None = None,
    ) -> None:
        """Set callback functions for loop events."""
        self._on_state_change = on_state_change
        self._on_tool_call = on_tool_call
        self._on_stream_chunk = on_stream_chunk

    async def run(
        self,
        user_message: str,
        history: list[Message] | None = None,
        system_prompt: str | None = None,
    ) -> AsyncIterator[Message]:
        """
        Run the agentic loop.

        Args:
            user_message: User's input message
            history: Optional conversation history
            system_prompt: Optional system prompt override

        Yields:
            Messages generated during the loop
        """
        # Initialize context
        ctx = LoopContext()
        ctx.metrics.start_time = time.time()

        # Build initial messages
        tools = await self.server_manager.list_tools()
        system = self._context_builder.build_system_prompt(
            base_prompt=system_prompt,
            tools=tools,
        )

        # Format system prompt with tools through adapter
        formatted_system = self.orchestrator.adapter.format_system_prompt(system, tools)

        ctx.messages = [Message(role="system", content=formatted_system)]

        if history:
            ctx.messages.extend(history)

        ctx.messages.append(Message(role="user", content=user_message))

        self._set_state(ctx, AgentState.PLANNING)

        try:
            async for message in self._loop(ctx, tools):
                yield message
        finally:
            ctx.metrics.end_time = time.time()
            logger.info(
                f"Loop complete: {ctx.metrics.iterations} iterations, "
                f"{ctx.metrics.tool_calls} tool calls, "
                f"{ctx.metrics.duration_ms}ms"
            )

    async def _loop(
        self,
        ctx: LoopContext,
        tools: list[dict[str, Any]],
    ) -> AsyncIterator[Message]:
        """Main loop implementation."""
        while not self._should_stop(ctx):
            ctx.metrics.iterations += 1

            if self._interrupt_event.is_set():
                self._set_state(ctx, AgentState.INTERRUPTED)
                break

            async for msg in self._execute_iteration(ctx):
                yield msg

            if ctx.state == AgentState.ERROR:
                break

    async def _execute_iteration(self, ctx: LoopContext) -> AsyncIterator[Message]:
        """Execute a single loop iteration."""
        try:
            self._set_state(ctx, AgentState.EXECUTING)
            content, tool_calls = await self._generate_response(ctx)
            assistant_msg = self._create_assistant_message(content, tool_calls)
            ctx.messages.append(assistant_msg)
            yield assistant_msg

            if tool_calls:
                async for msg in self._process_tool_calls(ctx, tool_calls):
                    yield msg
            else:
                self._set_state(ctx, AgentState.COMPLETE)

        except Exception as e:
            async for msg in self._handle_error(ctx, e):
                yield msg

    async def _generate_response(self, ctx: LoopContext) -> tuple[str, list[ToolCall]]:
        """Generate model response, streaming or not."""
        if self.loop_config.stream_output:
            content = ""
            async for chunk in self.orchestrator.generate_stream(ctx.messages):
                content += chunk
                if self._on_stream_chunk:
                    self._on_stream_chunk(chunk)
            return self.orchestrator.adapter.parse_tool_calls(content)

        result = await self.orchestrator.generate(ctx.messages)
        ctx.metrics.tokens_used += result.token_count
        return result.content, result.tool_calls

    def _create_assistant_message(self, content: str, tool_calls: list[ToolCall]) -> Message:
        """Create assistant message with tool calls."""
        return Message(
            role="assistant",
            content=content,
            tool_calls=[
                {"id": tc.id, "name": tc.name, "arguments": tc.arguments} for tc in tool_calls
            ],
        )

    async def _process_tool_calls(
        self, ctx: LoopContext, tool_calls: list[ToolCall]
    ) -> AsyncIterator[Message]:
        """Process tool calls and yield result messages."""
        self._set_state(ctx, AgentState.WAITING_TOOL)
        limited_calls = tool_calls[: self.loop_config.max_tool_calls_per_turn]

        for tool_call in limited_calls:
            msg = await self._execute_tool(ctx, tool_call)
            ctx.messages.append(msg)
            yield msg

        ctx.consecutive_errors = 0

    async def _execute_tool(self, ctx: LoopContext, tool_call: ToolCall) -> Message:
        """Execute a single tool call."""
        if not self._is_tool_approved(tool_call):
            return self._create_denied_message(tool_call, "Permission denied by user")

        try:
            result = await self.server_manager.execute(tool_call)
            ctx.metrics.tool_calls += 1
            return self.orchestrator.adapter.format_tool_result(tool_call, result.result)
        except PermissionDeniedError as e:
            return self._create_denied_message(tool_call, str(e))

    def _is_tool_approved(self, tool_call: ToolCall) -> bool:
        """Check if tool call is approved."""
        if self.loop_config.auto_approve_tools:
            return True
        if self._on_tool_call is None:
            return True
        return self._on_tool_call(tool_call)

    def _create_denied_message(self, tool_call: ToolCall, reason: str) -> Message:
        """Create a permission denied message."""
        return Message(
            role="tool",
            content=f"Permission denied: {reason}",
            tool_results=[
                {
                    "call_id": tool_call.id,
                    "name": tool_call.name,
                    "result": reason,
                }
            ],
        )

    async def _handle_error(self, ctx: LoopContext, error: Exception) -> AsyncIterator[Message]:
        """Handle loop iteration error."""
        logger.error(f"Loop error: {error}")
        ctx.consecutive_errors += 1
        ctx.metrics.errors += 1

        if ctx.consecutive_errors >= self.loop_config.max_consecutive_errors:
            self._set_state(ctx, AgentState.ERROR)
            yield Message(
                role="assistant",
                content=f"I encountered repeated errors and cannot continue: {error}",
            )

    def _should_stop(self, ctx: LoopContext) -> bool:
        """Check if loop should stop."""
        if ctx.state in (AgentState.COMPLETE, AgentState.ERROR, AgentState.INTERRUPTED):
            return True

        if ctx.metrics.iterations >= self.loop_config.max_iterations:
            logger.warning("Max iterations reached")
            return True

        return False

    def _set_state(self, ctx: LoopContext, state: AgentState) -> None:
        """Set agent state and notify callback."""
        ctx.state = state
        logger.debug(f"State: {state.name}")
        if self._on_state_change:
            self._on_state_change(state)

    def interrupt(self) -> None:
        """Interrupt the running loop."""
        self._interrupt_event.set()

    def reset_interrupt(self) -> None:
        """Reset interrupt flag."""
        self._interrupt_event.clear()
