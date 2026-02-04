"""
Agentic loop engine.

Implements the core plan→act→observe→repeat cycle with
proper state management and termination conditions.
"""

from __future__ import annotations

import asyncio
import json
import threading
import time
from collections.abc import AsyncIterator, Callable
from dataclasses import dataclass, field
from enum import Enum, auto
from typing import TYPE_CHECKING, Any

from entropi.config.loader import save_permission
from entropi.config.schema import EntropyConfig
from entropi.core.base import Message, ToolCall
from entropi.core.compaction import CompactionManager, CompactionResult, TokenCounter
from entropi.core.context import ContextBuilder
from entropi.core.logging import get_logger
from entropi.core.queue import MessageSource
from entropi.core.todos import TODO_SYSTEM_PROMPT, TODO_TOOL_DEFINITION, TodoList
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
    PAUSED = auto()  # Generation paused, awaiting user input


class InterruptMode(Enum):
    """How to handle generation interrupt."""

    CANCEL = "cancel"  # Discard partial response, stop
    PAUSE = "pause"  # Keep partial response, await input
    INJECT = "inject"  # Keep partial, inject context, continue


class ToolApproval(Enum):
    """Tool approval responses from user."""

    DENY = "deny"  # Deny this once
    ALLOW = "allow"  # Allow this once
    ALWAYS_DENY = "always_deny"  # Deny and save to config
    ALWAYS_ALLOW = "always_allow"  # Allow and save to config


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
    # Track recent tool calls to prevent duplicates (key: "name:args_hash")
    recent_tool_calls: dict[str, str] = field(default_factory=dict)
    # Track consecutive duplicate attempts to detect stuck model
    consecutive_duplicate_attempts: int = 0
    # Flag indicating we have tool results that should be presented
    has_pending_tool_results: bool = False
    # Lock model tier for the entire loop to prevent mid-task switching
    locked_tier: Any = None  # ModelTier or None
    # External task tracking (for MCP integration)
    task_id: str | None = None  # Associated task ID if from external source
    source: str = MessageSource.HUMAN  # Message source (human, claude-code)


@dataclass
class InterruptContext:
    """Context for interrupted/paused generation."""

    partial_content: str = ""
    partial_tool_calls: list[ToolCall] = field(default_factory=list)
    injection: str | None = None
    mode: InterruptMode = InterruptMode.PAUSE


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
        # Use threading.Event for thread-safe cross-loop signaling
        # (Textual runs on a different event loop than the generation worker)
        self._interrupt_event = threading.Event()

        # Pause/inject support for interrupting generation
        self._pause_event = threading.Event()
        self._interrupt_context: InterruptContext | None = None

        # Callback for pause/inject prompting
        self._on_pause_prompt: Callable[[str], Any] | None = None

        # Todo list for agentic task tracking
        self._todo_list = TodoList()

        # Compaction manager for context management
        max_tokens = self._get_max_context_tokens()
        self._token_counter = TokenCounter(max_tokens)
        self._compaction_manager = CompactionManager(
            config=config.compaction,
            token_counter=self._token_counter,
            orchestrator=orchestrator,
        )

        # Callbacks
        self._on_state_change: Callable[[AgentState], None] | None = None
        self._on_tool_approval: Callable[[ToolCall], Any] | None = None  # Can be sync or async
        self._on_stream_chunk: Callable[[str], None] | None = None
        self._on_tool_start: Callable[[ToolCall], None] | None = None
        self._on_tool_complete: Callable[[ToolCall, str, float], None] | None = None
        self._on_todo_update: Callable[[TodoList], None] | None = None
        self._on_compaction: Callable[[CompactionResult], None] | None = None
        # Task tracking callback for external MCP integration
        self._on_tool_record: Callable[[str, ToolCall, str, str | None, float], None] | None = None

    def _get_max_context_tokens(self) -> int:
        """Get maximum context tokens from model config."""
        # Try to get from the default model config
        default_model = self.config.models.default
        model_config = getattr(self.config.models, default_model, None)
        if model_config and hasattr(model_config, "context_length"):
            return model_config.context_length
        # Fallback default
        return 16384

    def _refresh_context_limit(self) -> None:
        """Refresh context limit based on current model.

        When models swap (e.g., normal → thinking), the context limit
        may change. This method updates the token counter to use the
        current model's context length.
        """
        # Get context length from last used tier's model config
        last_tier = self.orchestrator.last_used_tier
        if last_tier is None:
            return

        # Map tier to config attribute name
        tier_to_config = {
            "thinking": self.config.models.thinking,
            "normal": self.config.models.normal,
            "code": self.config.models.code,
            "simple": self.config.models.simple,
        }

        model_config = tier_to_config.get(last_tier.value)
        if model_config and hasattr(model_config, "context_length"):
            new_max = model_config.context_length
            if new_max != self._token_counter.max_tokens:
                logger.debug(
                    f"Updating context limit: {self._token_counter.max_tokens} -> {new_max}"
                )
                self._token_counter.max_tokens = new_max

    def set_callbacks(
        self,
        on_state_change: Callable[[AgentState], None] | None = None,
        on_tool_call: Callable[[ToolCall], Any] | None = None,
        on_stream_chunk: Callable[[str], None] | None = None,
        on_tool_start: Callable[[ToolCall], None] | None = None,
        on_tool_complete: Callable[[ToolCall, str, float], None] | None = None,
        on_todo_update: Callable[[TodoList], None] | None = None,
        on_compaction: Callable[[CompactionResult], None] | None = None,
        on_pause_prompt: Callable[[str], Any] | None = None,
        on_tool_record: Callable[[str, ToolCall, str, str | None, float], None] | None = None,
    ) -> None:
        """
        Set callback functions for loop events.

        Args:
            on_state_change: Called when agent state changes
            on_tool_call: Called to approve tool calls (return True to allow). Can be sync or async.
            on_stream_chunk: Called for each streamed text chunk
            on_tool_start: Called when tool execution begins
            on_tool_complete: Called when tool execution completes (tool, result, duration_ms)
            on_todo_update: Called when todo list is updated
            on_compaction: Called when context is compacted
            on_pause_prompt: Called when generation is paused, returns injection text or None
            on_tool_record: Called to record tool call for task tracking (task_id, tool, status, result, duration)
        """
        self._on_state_change = on_state_change
        self._on_tool_approval = on_tool_call
        self._on_stream_chunk = on_stream_chunk
        self._on_tool_start = on_tool_start
        self._on_tool_complete = on_tool_complete
        self._on_todo_update = on_todo_update
        self._on_compaction = on_compaction
        self._on_pause_prompt = on_pause_prompt
        self._on_tool_record = on_tool_record

    @property
    def todo_list(self) -> TodoList:
        """Get the current todo list."""
        return self._todo_list

    async def run(
        self,
        user_message: str,
        history: list[Message] | None = None,
        system_prompt: str | None = None,
        task_id: str | None = None,
        source: str = MessageSource.HUMAN,
    ) -> AsyncIterator[Message]:
        """
        Run the agentic loop.

        Args:
            user_message: User's input message
            history: Optional conversation history
            system_prompt: Optional system prompt override
            task_id: Optional task ID for external tracking (MCP integration)
            source: Message source (human, claude-code, system)

        Yields:
            Messages generated during the loop
        """
        # Initialize context
        ctx = LoopContext()
        ctx.metrics.start_time = time.time()
        ctx.task_id = task_id
        ctx.source = source

        # Reset interrupt flag from any previous run
        self.reset_interrupt()
        self._pause_event.clear()

        # Build initial messages and get tools for system prompt
        tools = await self.server_manager.list_tools()
        # Add internal todo tool
        tools.append(TODO_TOOL_DEFINITION)
        logger.info(f"Tools available ({len(tools)}): {[t['name'] for t in tools]}")

        # Build system prompt with todo guidance appended
        base_with_todos = (system_prompt or "") + TODO_SYSTEM_PROMPT
        system = self._context_builder.build_system_prompt(base_with_todos)

        # Format system prompt with tools through adapter
        formatted_system = self.orchestrator.get_adapter().format_system_prompt(system, tools)

        ctx.messages = [Message(role="system", content=formatted_system)]
        logger.info(f"System prompt size: ~{len(formatted_system) // 4} tokens")

        if history:
            ctx.messages.extend(history)

        ctx.messages.append(Message(role="user", content=user_message))

        # Log user prompt for debugging
        logger.info(f"\n{'='*60}\n[USER PROMPT]\n{'='*60}\n{user_message}\n{'='*60}")

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

        # If we exited due to max iterations, warn the user
        if ctx.metrics.iterations >= self.loop_config.max_iterations:
            logger.warning("Loop ended due to max iterations")
            yield Message(
                role="assistant",
                content="[Note: Maximum iterations reached. The task may be incomplete.]",
            )

    async def _execute_iteration(self, ctx: LoopContext) -> AsyncIterator[Message]:
        """Execute a single loop iteration."""
        logger.info(
            f"[LOOP START] Iteration {ctx.metrics.iterations}/{self.loop_config.max_iterations} | "
            f"state={ctx.state.name} | messages={len(ctx.messages)} | errors={ctx.consecutive_errors}"
        )
        try:
            # Refresh context limit in case model changed
            self._refresh_context_limit()

            # Check for compaction before generating
            await self._check_compaction(ctx)

            self._set_state(ctx, AgentState.EXECUTING)
            content, tool_calls = await self._generate_response(ctx)

            logger.debug(
                f"Iteration {ctx.metrics.iterations}: "
                f"content_len={len(content)}, tool_calls={len(tool_calls)}, "
                f"content_preview={content[:100]!r}"
            )

            assistant_msg = self._create_assistant_message(content, tool_calls)
            ctx.messages.append(assistant_msg)

            yield assistant_msg

            if tool_calls:
                async for msg in self._process_tool_calls(ctx, tool_calls):
                    yield msg
            else:
                # Check if generation was cut off due to token limit
                finish_reason = self.orchestrator.last_finish_reason
                if finish_reason == "length":
                    logger.info("[LOOP DECISION] finish_reason=length, continuing loop")
                    # Don't mark complete, let loop continue
                else:
                    # No tool calls - check if response is complete using adapter
                    adapter = self.orchestrator.get_adapter()
                    is_complete = adapter.is_response_complete(content, tool_calls)
                    logger.info(f"[LOOP DECISION] tool_calls=0, is_response_complete={is_complete}")
                    if is_complete:
                        self._set_state(ctx, AgentState.COMPLETE)
                    # else: continue loop (model may still be thinking)

        except Exception as e:
            logger.exception(f"Loop iteration error: {e}")
            async for msg in self._handle_error(ctx, e):
                yield msg

    async def _generate_response(self, ctx: LoopContext) -> tuple[str, list[ToolCall]]:
        """Generate model response, streaming or not."""
        logger.debug(f"Generating response (stream={self.loop_config.stream_output})")

        if self.loop_config.stream_output:
            # Streaming mode - tools will be parsed from content after streaming
            content = ""

            # Use locked tier if set, otherwise let orchestrator auto-route
            async for chunk in self.orchestrator.generate_stream(
                ctx.messages, tier=ctx.locked_tier
            ):
                # Check for pause request during streaming
                if self._pause_event.is_set():
                    logger.info("Generation paused by user")
                    content = await self._handle_pause(ctx, content)
                    # After handling pause, check if we should continue or abort
                    if self._interrupt_event.is_set():
                        break

                content += chunk

                # Pass ALL content to callback (including think blocks)
                # UI layer handles styling of think blocks
                if self._on_stream_chunk:
                    self._on_stream_chunk(chunk)

            logger.debug("\n=== Stream complete ===")
            logger.debug(f"Total content length: {len(content)}")

            # Log COMPLETE raw model output including thinking blocks
            logger.info(
                f"\n{'='*70}\n"
                f"[MODEL OUTPUT - Turn {ctx.metrics.iterations}]\n"
                f"{'='*70}\n"
                f"{content}\n"
                f"{'='*70}"
            )

            # Lock tier after first generation to prevent mid-task model switching
            if ctx.locked_tier is None:
                ctx.locked_tier = self.orchestrator.last_used_tier
                logger.debug(f"Locked tier for loop: {ctx.locked_tier}")

            cleaned_content, tool_calls = self.orchestrator.get_adapter().parse_tool_calls(content)
            logger.debug(f"After parsing: {len(tool_calls)} tool calls found")
            return cleaned_content, tool_calls

        # Non-streaming mode - use locked tier if set
        result = await self.orchestrator.generate(ctx.messages, tier=ctx.locked_tier)

        # Log COMPLETE raw model output including thinking blocks
        logger.info(
            f"\n{'='*70}\n"
            f"[MODEL OUTPUT - Turn {ctx.metrics.iterations}]\n"
            f"{'='*70}\n"
            f"{result.content}\n"
            f"{'='*70}"
        )

        # Lock tier after first generation to prevent mid-task model switching
        if ctx.locked_tier is None:
            ctx.locked_tier = self.orchestrator.last_used_tier
            logger.debug(f"Locked tier for loop: {ctx.locked_tier}")

        ctx.metrics.tokens_used += result.token_count
        logger.debug(
            f"Non-stream response: {len(result.content)} chars, {len(result.tool_calls)} tool calls"
        )
        return result.content, result.tool_calls

    async def _handle_pause(self, ctx: LoopContext, partial_content: str) -> str:
        """
        Handle pause during generation.

        Prompts user for injection and either continues with context or resumes.
        """
        # Save partial state
        self._interrupt_context = InterruptContext(
            partial_content=partial_content,
            mode=InterruptMode.PAUSE,
        )
        self._set_state(ctx, AgentState.PAUSED)

        # Call the pause prompt callback to get user input
        injection = None
        if self._on_pause_prompt:
            result = self._on_pause_prompt(partial_content)
            if asyncio.iscoroutine(result):
                injection = await result
            else:
                injection = result

        # Clear pause event
        self._pause_event.clear()

        if injection is None:
            # User cancelled - set interrupt
            logger.info("Pause cancelled by user")
            self._interrupt_event.set()
            return partial_content

        if injection.strip():
            # User provided injection - continue with new context
            logger.info(f"Continuing with injection: {injection[:50]}...")
            return await self._continue_with_injection(ctx, partial_content, injection)

        # Empty injection - just resume
        logger.info("Resuming without injection")
        self._set_state(ctx, AgentState.EXECUTING)
        return partial_content

    async def _continue_with_injection(
        self,
        ctx: LoopContext,
        partial_content: str,
        injection: str,
    ) -> str:
        """Continue generation with injected user context."""
        # Add partial response to context (if any meaningful content)
        if partial_content.strip():
            ctx.messages.append(
                Message(
                    role="assistant",
                    content=partial_content + "\n\n[Generation paused by user]",
                )
            )

        # Add user injection
        ctx.messages.append(
            Message(
                role="user",
                content=f"[User interjection]: {injection}\n\nPlease continue with this in mind.",
            )
        )

        # Clear pause state and resume
        self._set_state(ctx, AgentState.EXECUTING)

        # Generate new response with injected context
        content = ""
        async for chunk in self.orchestrator.generate_stream(ctx.messages, tier=ctx.locked_tier):
            # Check for another pause during continuation
            if self._pause_event.is_set():
                logger.info("Generation paused again during continuation")
                content = await self._handle_pause(ctx, content)
                if self._interrupt_event.is_set():
                    break

            content += chunk
            if self._on_stream_chunk:
                self._on_stream_chunk(chunk)

        return content

    def _create_assistant_message(self, content: str, tool_calls: list[ToolCall]) -> Message:
        """Create assistant message with tool calls.

        Note: Empty assistant messages can cause llama_decode errors with some models
        (especially Falcon H1R). When content is empty but tool calls exist, we preserve
        a representation of the tool calls to ensure valid ChatML formatting.
        """
        # Prevent empty assistant messages which can cause KV cache issues
        if not content.strip() and tool_calls:
            # Use proper <tool_call> format so model learns correct pattern
            # (prevents feedback loop where model mimics [Calling: ...] format)
            content = "\n".join([
                f'<tool_call>{{"name": "{tc.name}", "arguments": {json.dumps(tc.arguments)}}}</tool_call>'
                for tc in tool_calls
            ])

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
        logger.info(f"Processing {len(tool_calls)} tool calls")
        self._set_state(ctx, AgentState.WAITING_TOOL)
        limited_calls = tool_calls[: self.loop_config.max_tool_calls_per_turn]

        for i, tool_call in enumerate(limited_calls):
            # Check for duplicate tool calls
            duplicate_result = self._check_duplicate_tool_call(ctx, tool_call)
            if duplicate_result:
                ctx.consecutive_duplicate_attempts += 1
                logger.warning(
                    f"Duplicate tool call #{ctx.consecutive_duplicate_attempts}: {tool_call.name}"
                )

                # Circuit breaker: if model is stuck in loop, force break
                if ctx.consecutive_duplicate_attempts >= 3:
                    logger.error(
                        f"Model stuck in loop - {ctx.consecutive_duplicate_attempts} "
                        f"consecutive duplicate tool calls"
                    )
                    break_msg = Message(
                        role="user",
                        content="STOP: You have called the same tool 3 times with identical arguments. "
                        "This indicates you are stuck. Please try a completely different approach "
                        "or respond to the user explaining what's blocking you.",
                    )
                    ctx.messages.append(break_msg)
                    logger.info(f"[FEEDBACK] Circuit breaker triggered: {break_msg.content}")
                    yield break_msg
                    return  # Exit tool processing entirely

                msg = self._create_duplicate_message(tool_call, duplicate_result)
                ctx.messages.append(msg)
                logger.info(f"[FEEDBACK] Duplicate message sent: {msg.content[:100]}...")
                yield msg
                continue

            # Successful non-duplicate tool call - reset counter
            ctx.consecutive_duplicate_attempts = 0

            logger.debug(f"Executing tool {i+1}/{len(limited_calls)}: {tool_call.name}")
            msg = await self._execute_tool(ctx, tool_call)
            ctx.messages.append(msg)
            yield msg

            # Track this tool call for duplicate detection
            self._record_tool_call(ctx, tool_call, msg)

        ctx.consecutive_errors = 0

    def _get_tool_call_key(self, tool_call: ToolCall) -> str:
        """Generate a unique key for a tool call based on name and arguments."""
        args_str = json.dumps(tool_call.arguments, sort_keys=True, default=str)
        return f"{tool_call.name}:{args_str}"

    def _check_duplicate_tool_call(self, ctx: LoopContext, tool_call: ToolCall) -> str | None:
        """Check if this tool call is a duplicate. Returns previous result if duplicate."""
        # Never skip read_file - it has side effects (updates FileAccessTracker)
        # Without this exemption, re-reads after TTL expiry get blocked as "duplicate"
        # but the tracker doesn't update, so subsequent edits still fail with read_required
        if tool_call.name == "filesystem.read_file":
            return None

        key = self._get_tool_call_key(tool_call)
        return ctx.recent_tool_calls.get(key)

    def _record_tool_call(self, ctx: LoopContext, tool_call: ToolCall, result_msg: Message) -> None:
        """Record a tool call for duplicate detection."""
        key = self._get_tool_call_key(tool_call)
        # Store the result content for reuse
        ctx.recent_tool_calls[key] = result_msg.content

    def _create_duplicate_message(self, tool_call: ToolCall, previous_result: str) -> Message:
        """Create a message indicating a duplicate tool call was skipped.

        Uses role="user" because llama-cpp doesn't render role="tool" properly,
        which would cause the model to never see the feedback and loop forever.
        """
        content = f"""Tool `{tool_call.name}` was already called with the same arguments.

Previous result:
{previous_result}

Do NOT call this tool again. Use the previous result above."""

        return Message(role="user", content=content)

    async def _execute_tool(self, ctx: LoopContext, tool_call: ToolCall) -> Message:
        """Execute a single tool call."""
        logger.info(f"[TOOL CALL] {tool_call.name}")
        logger.info(f"[TOOL ARGS] {json.dumps(tool_call.arguments, indent=2, default=str)}")

        # Handle internal tools first (no approval needed)
        if tool_call.name == "entropi.todo_write":
            return self._handle_todo_tool(tool_call)

        # Handle system.handoff - auto-approve and process specially
        if tool_call.name == "system.handoff":
            return await self._handle_handoff(ctx, tool_call)

        approval_result = await self._is_tool_approved(tool_call)
        if approval_result is not True:
            # Denied - check if user provided feedback
            if isinstance(approval_result, str):
                logger.warning(f"Tool call denied with feedback: {tool_call.name}")
                reason = f"Permission denied by user. Feedback: {approval_result}"
            else:
                logger.warning(f"Tool call denied by user: {tool_call.name}")
                reason = "Permission denied by user"
            denied_msg = self._create_denied_message(tool_call, reason)
            logger.info(f"[FEEDBACK] Denied message sent: {denied_msg.content[:100]}...")
            return denied_msg

        # Notify tool start
        if self._on_tool_start:
            self._on_tool_start(tool_call)

        start_time = time.time()
        try:
            logger.debug(f"Executing tool via server manager: {tool_call.name}")
            result = await self.server_manager.execute(tool_call)
            ctx.metrics.tool_calls += 1
            duration_ms = (time.time() - start_time) * 1000

            result_str = str(result.result)
            logger.debug(f"Tool result ({duration_ms:.0f}ms):\n{result_str}")

            # Log full tool result for debugging
            logger.info(
                f"\n{'='*60}\n[TOOL RESULT: {tool_call.name}]\n{'='*60}\n"
                f"{result_str}\n"
                f"{'='*60}"
            )
            logger.info(f"[TOOL COMPLETE] {tool_call.name} -> {len(result_str)} chars result ({duration_ms:.0f}ms)")

            # Notify tool complete
            if self._on_tool_complete:
                self._on_tool_complete(tool_call, result.result, duration_ms)

            # Record tool call for task tracking (MCP integration)
            if ctx.task_id and self._on_tool_record:
                self._on_tool_record(ctx.task_id, tool_call, "success", result.result, duration_ms)

            return self.orchestrator.get_adapter().format_tool_result(tool_call, result.result)
        except PermissionDeniedError as e:
            duration_ms = (time.time() - start_time) * 1000
            logger.warning(f"Tool permission denied: {tool_call.name} - {e}")
            if self._on_tool_complete:
                self._on_tool_complete(tool_call, f"Permission denied: {e}", duration_ms)
            # Record error for task tracking
            if ctx.task_id and self._on_tool_record:
                self._on_tool_record(ctx.task_id, tool_call, "error", str(e), duration_ms)
            denied_msg = self._create_denied_message(tool_call, str(e))
            logger.info(f"[FEEDBACK] Permission denied message sent: {denied_msg.content[:100]}...")
            return denied_msg
        except Exception as e:
            duration_ms = (time.time() - start_time) * 1000
            logger.error(f"Tool execution error: {tool_call.name} - {e}")
            if self._on_tool_complete:
                self._on_tool_complete(tool_call, f"Error: {e}", duration_ms)
            # Record error for task tracking
            if ctx.task_id and self._on_tool_record:
                self._on_tool_record(ctx.task_id, tool_call, "error", str(e), duration_ms)
            # Return error message instead of raising to allow the model to recover
            error_msg = self._create_error_message(tool_call, str(e))
            logger.info(f"[FEEDBACK] Error message sent: {error_msg.content[:100]}...")
            return error_msg

    async def _is_tool_approved(self, tool_call: ToolCall) -> bool | str:
        """Check if tool call is approved.

        Approval flow:
        1. If auto_approve is set, always approve
        2. If tool is explicitly in allow list, skip prompting
        3. Otherwise, prompt user via callback
        4. If user selects "Always allow/deny", save to config

        Returns:
            True if approved, False if denied, or str with feedback if denied with feedback.
        """
        if self.loop_config.auto_approve_tools:
            return True

        # Check if explicitly allowed in config - no prompt needed
        if self.server_manager.is_explicitly_allowed(tool_call):
            return True

        # No callback means auto-approve (e.g., headless mode)
        if self._on_tool_approval is None:
            return True

        # Prompt user via callback
        # Callback can return: bool, ToolApproval enum, str (feedback), or awaitable
        result = self._on_tool_approval(tool_call)
        if asyncio.iscoroutine(result):
            result = await result

        # Handle feedback string (denial with user message)
        if isinstance(result, str):
            return result  # Return feedback string as denial reason

        # Handle ToolApproval enum responses
        if isinstance(result, ToolApproval):
            return self._handle_approval_result(tool_call, result)

        # Legacy bool support
        return bool(result)

    def _handle_approval_result(self, tool_call: ToolCall, approval: ToolApproval) -> bool:
        """Handle ToolApproval result and save to config if needed."""
        # Generate permission pattern for this tool call
        pattern = self._get_permission_pattern(tool_call)

        if approval == ToolApproval.ALWAYS_ALLOW:
            logger.info(f"Saving 'always allow' permission: {pattern}")
            save_permission(pattern, allow=True)
            # Also update the in-memory permissions
            self.server_manager.add_permission(pattern, allow=True)
            return True

        elif approval == ToolApproval.ALWAYS_DENY:
            logger.info(f"Saving 'always deny' permission: {pattern}")
            save_permission(pattern, allow=False)
            self.server_manager.add_permission(pattern, allow=False)
            return False

        elif approval == ToolApproval.ALLOW:
            return True

        else:  # DENY
            return False

    def _get_permission_pattern(self, tool_call: ToolCall) -> str:
        """Generate a permission pattern for a tool call.

        For bash commands, uses the command as the pattern.
        For file operations, uses the path.
        """
        tool_name = tool_call.name
        args = tool_call.arguments

        # For bash.execute, use the command
        if tool_name == "bash.execute" and "command" in args:
            return f"{tool_name}:{args['command']}"

        # For filesystem operations, use the path
        if tool_name.startswith("filesystem.") and "path" in args:
            return f"{tool_name}:{args['path']}"

        # Default: just the tool name with wildcard
        return f"{tool_name}:*"

    def _create_denied_message(self, tool_call: ToolCall, reason: str) -> Message:
        """Create a permission denied message.

        Uses role="user" because llama-cpp doesn't render role="tool" properly,
        which would cause the model to never see the feedback and loop forever.
        """
        content = f"""Tool `{tool_call.name}` was denied: {reason}

Try a different approach or ask the user for clarification."""

        return Message(role="user", content=content)

    def _create_error_message(self, tool_call: ToolCall, error: str) -> Message:
        """Create a structured error message for the model to recover from.

        Uses role="user" because llama-cpp doesn't render role="tool" properly,
        which would cause the model to never see the feedback and loop forever.
        """
        content = f"""Tool `{tool_call.name}` failed with error: {error}

RECOVERY:
- Check arguments are correct
- Try a different approach
- Do NOT retry with the same arguments"""

        return Message(role="user", content=content)

    def _handle_todo_tool(self, tool_call: ToolCall) -> Message:
        """Handle the internal entropi.todo_write tool call."""
        logger.debug(f"Handling internal todo tool: {tool_call.arguments}")

        todos = tool_call.arguments.get("todos", [])
        result = self._todo_list.update_from_tool_call(todos)

        # Notify UI of todo update
        if self._on_todo_update:
            self._on_todo_update(self._todo_list)

        return Message(
            role="tool",
            content=result,
            tool_results=[
                {
                    "call_id": tool_call.id,
                    "name": tool_call.name,
                    "result": result,
                }
            ],
        )

    async def _handle_handoff(self, ctx: LoopContext, tool_call: ToolCall) -> Message:
        """Handle the system.handoff tool call for tier switching."""
        from entropi.inference.orchestrator import ModelTier

        target_tier_str = tool_call.arguments.get("target_tier", "")
        reason = tool_call.arguments.get("reason", "")

        logger.info(f"[HANDOFF] Request: {ctx.locked_tier} -> {target_tier_str} ({reason})")

        # Validate target tier
        try:
            target_tier = ModelTier(target_tier_str)
        except ValueError:
            error_msg = f"Invalid target tier: {target_tier_str}"
            logger.warning(f"[HANDOFF] {error_msg}")
            return self._create_error_message(tool_call, error_msg)

        # Get current tier (may be None if not locked yet)
        current_tier = ctx.locked_tier

        # Validate routing rules
        if not self.orchestrator.can_handoff(current_tier, target_tier):
            current_name = current_tier.value if current_tier else "none"
            error_msg = f"Handoff not permitted: {current_name} cannot hand off to {target_tier_str}"
            logger.warning(f"[HANDOFF] {error_msg}")
            return self._create_error_message(tool_call, error_msg)

        # Update the locked tier - next generation will use the new tier
        ctx.locked_tier = target_tier
        logger.info(f"[HANDOFF] Success: Now using {target_tier_str} tier")

        # Uses role="user" because llama-cpp doesn't render role="tool" properly
        content = f"Handoff successful. Now operating as {target_tier_str} tier. Reason: {reason}"

        return Message(role="user", content=content)

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

        # Circuit breaker: stop if model is stuck in duplicate tool calls
        if ctx.consecutive_duplicate_attempts >= 3:
            logger.warning("Stopping loop: model stuck in duplicate tool calls")
            return True

        return False

    def _set_state(self, ctx: LoopContext, state: AgentState) -> None:
        """Set agent state and notify callback."""
        ctx.state = state
        logger.debug(f"State: {state.name}")
        if self._on_state_change:
            self._on_state_change(state)

    async def _check_compaction(self, ctx: LoopContext) -> None:
        """Check if compaction is needed and perform if so."""
        ctx.messages, result = await self._compaction_manager.check_and_compact(
            conversation_id=None,  # TODO: pass actual conversation ID
            messages=ctx.messages,
        )

        if result.compacted:
            logger.info(
                f"Compacted context: {result.old_token_count} -> {result.new_token_count} tokens"
            )
            # Notify via callback
            if self._on_compaction:
                self._on_compaction(result)

    def interrupt(self) -> None:
        """Interrupt the running loop (hard cancel)."""
        self._interrupt_event.set()
        # Also clear any pending pause
        self._pause_event.clear()

    def reset_interrupt(self) -> None:
        """Reset interrupt flag."""
        self._interrupt_event.clear()

    def pause(self) -> None:
        """Pause generation (Escape key) to allow injection."""
        logger.debug("Pause requested")
        self._pause_event.set()

    def inject(self, content: str) -> None:
        """Inject user content and resume generation.

        Note: Currently injection happens via _on_pause_prompt callback.
        This method is reserved for future direct injection support.
        """
        logger.debug(f"Injecting content: {content[:50]}...")
        # Injection now handled via _on_pause_prompt callback in _handle_pause()

    def resume(self) -> None:
        """Resume generation without injection.

        Note: Currently resume happens via _on_pause_prompt returning empty string.
        This method is reserved for future direct resume support.
        """
        logger.debug("Resume without injection")
        # Resume now handled via _on_pause_prompt callback in _handle_pause()

    def cancel_pause(self) -> None:
        """Cancel pause and interrupt completely."""
        logger.debug("Cancel pause requested")
        self._pause_event.clear()
        self._interrupt_event.set()

    @property
    def is_paused(self) -> bool:
        """Check if generation is paused."""
        return self._pause_event.is_set()
