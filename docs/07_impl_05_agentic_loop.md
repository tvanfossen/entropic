# Implementation 05: Agentic Loop

> Core agent execution cycle with plan-act-observe pattern

**Prerequisites:** Implementation 04 complete
**Estimated Time:** 3-4 hours with Claude Code
**Checkpoint:** Can execute multi-turn tool chains autonomously

---

## Objectives

1. Implement state machine for agent loop
2. Create context builder with token budgeting
3. Build tool call parser with retry logic
4. Implement loop termination conditions
5. Add interrupt handling

---

## 1. Agent State Machine

### File: `src/entropi/core/engine.py`

```python
"""
Agentic loop engine.

Implements the core plan→act→observe→repeat cycle with
proper state management and termination conditions.
"""
import asyncio
from dataclasses import dataclass, field
from enum import Enum, auto
from typing import Any, AsyncIterator, Callable

from entropi.config.schema import EntropyConfig
from entropi.core.base import GenerationResult, Message, ToolCall, ToolResult
from entropi.core.context import ContextBuilder
from entropi.core.logging import get_logger
from entropi.inference.orchestrator import ModelOrchestrator, ModelTier
from entropi.mcp.manager import ServerManager

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
    tool_results: list[ToolResult] = field(default_factory=list)
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
        import time

        # Initialize context
        ctx = LoopContext()
        ctx.metrics.start_time = time.time()

        # Build initial messages
        tools = await self.server_manager.list_tools()
        system = self._context_builder.build_system_prompt(
            base_prompt=system_prompt,
            tools=tools,
        )

        ctx.messages = [Message(role="system", content=system)]

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

            # Check for interrupt
            if self._interrupt_event.is_set():
                self._set_state(ctx, AgentState.INTERRUPTED)
                break

            try:
                # Generate response
                self._set_state(ctx, AgentState.EXECUTING)

                response_content = ""
                tool_calls: list[ToolCall] = []

                if self.loop_config.stream_output:
                    # Streaming generation
                    async for chunk in self.orchestrator.generate_stream(ctx.messages):
                        response_content += chunk
                        if self._on_stream_chunk:
                            self._on_stream_chunk(chunk)

                    # Parse tool calls after complete
                    response_content, tool_calls = self.orchestrator.adapter.parse_tool_calls(
                        response_content
                    )
                else:
                    # Non-streaming generation
                    result = await self.orchestrator.generate(ctx.messages)
                    response_content, tool_calls = self.orchestrator.adapter.parse_tool_calls(
                        result.content
                    )
                    ctx.metrics.tokens_used += result.token_count

                # Create assistant message
                assistant_msg = Message(
                    role="assistant",
                    content=response_content,
                    tool_calls=[
                        {"id": tc.id, "name": tc.name, "arguments": tc.arguments}
                        for tc in tool_calls
                    ],
                )
                ctx.messages.append(assistant_msg)
                yield assistant_msg

                # Handle tool calls
                if tool_calls:
                    self._set_state(ctx, AgentState.WAITING_TOOL)

                    for tool_call in tool_calls[:self.loop_config.max_tool_calls_per_turn]:
                        # Check approval if needed
                        if not self.loop_config.auto_approve_tools and self._on_tool_call:
                            if not self._on_tool_call(tool_call):
                                continue

                        # Execute tool
                        result = await self.server_manager.execute(tool_call)
                        ctx.metrics.tool_calls += 1

                        # Format and inject result
                        tool_msg = self.orchestrator.adapter.format_tool_result(
                            tool_call, result.result
                        )
                        ctx.messages.append(tool_msg)
                        yield tool_msg

                    # Reset error count on successful tool execution
                    ctx.consecutive_errors = 0
                else:
                    # No tool calls - we're done
                    self._set_state(ctx, AgentState.COMPLETE)

            except Exception as e:
                logger.error(f"Loop error: {e}")
                ctx.consecutive_errors += 1
                ctx.metrics.errors += 1

                if ctx.consecutive_errors >= self.loop_config.max_consecutive_errors:
                    self._set_state(ctx, AgentState.ERROR)
                    error_msg = Message(
                        role="assistant",
                        content=f"I encountered repeated errors and cannot continue: {e}",
                    )
                    yield error_msg
                    break

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
        if self._on_state_change:
            self._on_state_change(state)

    def interrupt(self) -> None:
        """Interrupt the running loop."""
        self._interrupt_event.set()

    def reset_interrupt(self) -> None:
        """Reset interrupt flag."""
        self._interrupt_event.clear()
```

---

## 2. Context Builder

### File: `src/entropi/core/context.py`

```python
"""
Context builder for managing conversation context.

Handles token budgeting, message assembly, and context window management.
"""
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from entropi.config.schema import EntropyConfig
from entropi.core.base import Message
from entropi.core.logging import get_logger

logger = get_logger("core.context")


@dataclass
class TokenBudget:
    """Token budget allocation."""

    total: int
    system: int
    history: int
    tools: int
    response: int

    @classmethod
    def from_context_length(cls, context_length: int) -> "TokenBudget":
        """Create budget from context length."""
        return cls(
            total=context_length,
            system=2000,
            history=context_length - 6000,  # Leave room for response
            tools=1000,
            response=4000,
        )


class ContextBuilder:
    """
    Builds and manages conversation context.

    Responsibilities:
    - System prompt assembly
    - Token budgeting
    - History truncation
    - ENTROPI.md loading
    """

    def __init__(self, config: EntropyConfig) -> None:
        """
        Initialize context builder.

        Args:
            config: Application configuration
        """
        self.config = config
        self._default_system_prompt = self._load_default_prompt()
        self._project_context: str | None = None

    def _load_default_prompt(self) -> str:
        """Load default system prompt."""
        default_path = self.config.prompts_dir / "system" / "default.md"

        if default_path.exists():
            return default_path.read_text()

        return """You are Entropi, a local AI coding assistant.

You help developers write, review, and improve code. You have access to tools for:
- Reading and writing files
- Executing shell commands
- Git operations

When you need to perform actions, use the available tools. Think step by step and explain your reasoning.

Be concise but thorough. Write clean, well-documented code following best practices."""

    def load_project_context(self, project_dir: Path) -> None:
        """
        Load ENTROPI.md from project directory.

        Args:
            project_dir: Project directory path
        """
        entropi_md = project_dir / "ENTROPI.md"

        if entropi_md.exists():
            self._project_context = entropi_md.read_text()
            logger.info(f"Loaded project context from {entropi_md}")
        else:
            self._project_context = None

    def build_system_prompt(
        self,
        base_prompt: str | None = None,
        tools: list[dict[str, Any]] | None = None,
    ) -> str:
        """
        Build complete system prompt.

        Args:
            base_prompt: Optional base prompt override
            tools: Available tools

        Returns:
            Complete system prompt
        """
        parts = []

        # Base prompt
        parts.append(base_prompt or self._default_system_prompt)

        # Project context
        if self._project_context:
            parts.append("\n# Project Context\n")
            parts.append(self._project_context)

        # Tool formatting is handled by the adapter
        # Just return the assembled prompt
        return "\n".join(parts)

    def truncate_history(
        self,
        messages: list[Message],
        budget: TokenBudget,
        count_tokens: callable,
    ) -> list[Message]:
        """
        Truncate history to fit within token budget.

        Args:
            messages: All messages
            budget: Token budget
            count_tokens: Token counting function

        Returns:
            Truncated messages
        """
        if not messages:
            return messages

        # Always keep system message and latest user message
        system_msgs = [m for m in messages if m.role == "system"]
        other_msgs = [m for m in messages if m.role != "system"]

        if not other_msgs:
            return messages

        # Count tokens in system messages
        system_tokens = sum(count_tokens(m.content) for m in system_msgs)
        available = budget.history - system_tokens

        # Build result from most recent messages
        result = []
        current_tokens = 0

        for msg in reversed(other_msgs):
            msg_tokens = count_tokens(msg.content)
            if current_tokens + msg_tokens > available:
                break
            result.insert(0, msg)
            current_tokens += msg_tokens

        return system_msgs + result

    def estimate_tokens(self, text: str) -> int:
        """Rough token estimation (4 chars per token)."""
        return len(text) // 4
```

---

## 3. Tool Call Parser with Retry

### File: `src/entropi/core/parser.py`

```python
"""
Tool call parsing with retry and recovery logic.

Handles malformed JSON and provides fallback parsing strategies.
"""
import json
import re
from typing import Any

from entropi.core.base import ToolCall
from entropi.core.logging import get_logger

logger = get_logger("core.parser")


class ToolCallParser:
    """
    Parser for tool calls with retry logic.

    Attempts multiple parsing strategies to recover from malformed output.
    """

    def __init__(self, max_retries: int = 3) -> None:
        """
        Initialize parser.

        Args:
            max_retries: Maximum retry attempts for malformed JSON
        """
        self.max_retries = max_retries

    def parse(
        self,
        content: str,
        start_marker: str = "<tool_call>",
        end_marker: str = "</tool_call>",
    ) -> tuple[str, list[ToolCall]]:
        """
        Parse tool calls from content.

        Args:
            content: Raw model output
            start_marker: Tool call start marker
            end_marker: Tool call end marker

        Returns:
            Tuple of (cleaned content, tool calls)
        """
        tool_calls = []

        # Find all tool call blocks
        pattern = re.compile(
            rf"{re.escape(start_marker)}\s*(.*?)\s*{re.escape(end_marker)}",
            re.DOTALL,
        )

        matches = pattern.findall(content)

        for match in matches:
            tool_call = self._parse_single(match.strip())
            if tool_call:
                tool_calls.append(tool_call)

        # Clean content
        cleaned = pattern.sub("", content).strip()

        return cleaned, tool_calls

    def _parse_single(self, json_str: str) -> ToolCall | None:
        """
        Parse a single tool call JSON.

        Tries multiple strategies to recover from malformed JSON.
        """
        # Strategy 1: Direct parse
        result = self._try_parse(json_str)
        if result:
            return result

        # Strategy 2: Fix common issues
        fixed = self._fix_common_issues(json_str)
        result = self._try_parse(fixed)
        if result:
            return result

        # Strategy 3: Extract with regex
        result = self._extract_with_regex(json_str)
        if result:
            return result

        logger.warning(f"Failed to parse tool call: {json_str[:100]}...")
        return None

    def _try_parse(self, json_str: str) -> ToolCall | None:
        """Attempt to parse JSON."""
        try:
            data = json.loads(json_str)
            if "name" in data:
                import uuid
                return ToolCall(
                    id=str(uuid.uuid4()),
                    name=data["name"],
                    arguments=data.get("arguments", {}),
                )
        except json.JSONDecodeError:
            pass
        return None

    def _fix_common_issues(self, json_str: str) -> str:
        """Fix common JSON formatting issues."""
        fixed = json_str

        # Remove trailing commas
        fixed = re.sub(r",\s*}", "}", fixed)
        fixed = re.sub(r",\s*]", "]", fixed)

        # Fix unquoted keys
        fixed = re.sub(r"(\{|,)\s*(\w+)\s*:", r'\1"\2":', fixed)

        # Fix single quotes
        fixed = fixed.replace("'", '"')

        return fixed

    def _extract_with_regex(self, json_str: str) -> ToolCall | None:
        """Extract tool call using regex as last resort."""
        # Try to find name
        name_match = re.search(r'"name"\s*:\s*"([^"]+)"', json_str)
        if not name_match:
            return None

        name = name_match.group(1)

        # Try to find arguments
        args_match = re.search(r'"arguments"\s*:\s*(\{[^}]+\})', json_str)
        arguments = {}

        if args_match:
            try:
                arguments = json.loads(args_match.group(1))
            except json.JSONDecodeError:
                pass

        import uuid
        return ToolCall(
            id=str(uuid.uuid4()),
            name=name,
            arguments=arguments,
        )
```

---

## 4. Tests

### File: `tests/unit/test_engine.py`

```python
"""Tests for agent engine."""
import pytest

from entropi.core.engine import AgentState, LoopConfig, LoopContext, LoopMetrics
from entropi.core.parser import ToolCallParser


class TestLoopConfig:
    """Tests for loop configuration."""

    def test_defaults(self) -> None:
        """Test default values."""
        config = LoopConfig()
        assert config.max_iterations == 15
        assert config.max_consecutive_errors == 3
        assert config.stream_output is True

    def test_custom_values(self) -> None:
        """Test custom configuration."""
        config = LoopConfig(max_iterations=5, stream_output=False)
        assert config.max_iterations == 5
        assert config.stream_output is False


class TestLoopMetrics:
    """Tests for loop metrics."""

    def test_duration_calculation(self) -> None:
        """Test duration calculation."""
        metrics = LoopMetrics()
        metrics.start_time = 1000.0
        metrics.end_time = 1002.5
        assert metrics.duration_ms == 2500


class TestToolCallParser:
    """Tests for tool call parser."""

    def setup_method(self) -> None:
        """Set up test fixtures."""
        self.parser = ToolCallParser()

    def test_parse_valid_json(self) -> None:
        """Test parsing valid JSON."""
        content = '''Here's my plan.

<tool_call>
{"name": "read_file", "arguments": {"path": "test.py"}}
</tool_call>'''

        cleaned, calls = self.parser.parse(content)

        assert "Here's my plan." in cleaned
        assert len(calls) == 1
        assert calls[0].name == "read_file"
        assert calls[0].arguments == {"path": "test.py"}

    def test_parse_malformed_json(self) -> None:
        """Test parsing malformed JSON with recovery."""
        content = '''<tool_call>
{'name': 'read_file', 'arguments': {'path': 'test.py',}}
</tool_call>'''

        _, calls = self.parser.parse(content)
        assert len(calls) == 1
        assert calls[0].name == "read_file"

    def test_parse_multiple(self) -> None:
        """Test parsing multiple tool calls."""
        content = '''<tool_call>
{"name": "read_file", "arguments": {"path": "a.py"}}
</tool_call>
<tool_call>
{"name": "read_file", "arguments": {"path": "b.py"}}
</tool_call>'''

        _, calls = self.parser.parse(content)
        assert len(calls) == 2

    def test_regex_fallback(self) -> None:
        """Test regex extraction as fallback."""
        content = '''<tool_call>
completely broken {"name": "test_tool", something "arguments": {"key": "value"}}
</tool_call>'''

        _, calls = self.parser.parse(content)
        # Should extract at least the name
        assert len(calls) == 1
        assert calls[0].name == "test_tool"
```

---

## Checkpoint: Verification

```bash
# Run tests
pytest tests/unit/test_engine.py -v

# Integration test with actual model
cd ~/projects/entropi
entropi ask "List the files in the current directory"
# Should show tool calls being made
```

**Success Criteria:**
- [ ] Parser tests pass
- [ ] Loop config works
- [ ] Engine initializes without errors
- [ ] Tool calls parsed correctly

---

## Next Phase

Proceed to **Implementation 06: Terminal UI** to build the rich terminal interface.
