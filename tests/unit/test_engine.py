"""Tests for agent engine."""

from unittest.mock import MagicMock, patch

import pytest
from entropi.core.base import Message, ToolCall
from entropi.core.context import ContextBuilder, TokenBudget
from entropi.core.engine import AgentEngine, AgentState, LoopConfig, LoopContext, LoopMetrics
from entropi.core.parser import ToolCallParser


class TestLoopConfig:
    """Tests for loop configuration."""

    def test_defaults(self) -> None:
        """Test default values."""
        config = LoopConfig()
        assert config.max_iterations == 15
        assert config.max_consecutive_errors == 3
        assert config.stream_output is True
        assert config.auto_approve_tools is False

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

    def test_default_values(self) -> None:
        """Test default metric values."""
        metrics = LoopMetrics()
        assert metrics.iterations == 0
        assert metrics.tool_calls == 0
        assert metrics.tokens_used == 0
        assert metrics.errors == 0


class TestLoopContext:
    """Tests for loop context."""

    def test_default_state(self) -> None:
        """Test default state is IDLE."""
        ctx = LoopContext()
        assert ctx.state == AgentState.IDLE

    def test_empty_messages(self) -> None:
        """Test empty message list by default."""
        ctx = LoopContext()
        assert ctx.messages == []


class TestToolCallParser:
    """Tests for tool call parser."""

    def setup_method(self) -> None:
        """Set up test fixtures."""
        self.parser = ToolCallParser()

    def test_parse_valid_json(self) -> None:
        """Test parsing valid JSON."""
        content = """Here's my plan.

<tool_call>
{"name": "read_file", "arguments": {"path": "test.py"}}
</tool_call>"""

        cleaned, calls = self.parser.parse(content)

        assert "Here's my plan." in cleaned
        assert len(calls) == 1
        assert calls[0].name == "read_file"
        assert calls[0].arguments == {"path": "test.py"}

    def test_parse_malformed_json_trailing_comma(self) -> None:
        """Test parsing malformed JSON with trailing comma."""
        content = """<tool_call>
{"name": "read_file", "arguments": {"path": "test.py",}}
</tool_call>"""

        _, calls = self.parser.parse(content)
        assert len(calls) == 1
        assert calls[0].name == "read_file"

    def test_parse_malformed_json_single_quotes(self) -> None:
        """Test parsing malformed JSON with single quotes."""
        content = """<tool_call>
{'name': 'read_file', 'arguments': {'path': 'test.py'}}
</tool_call>"""

        _, calls = self.parser.parse(content)
        assert len(calls) == 1
        assert calls[0].name == "read_file"

    def test_parse_multiple(self) -> None:
        """Test parsing multiple tool calls."""
        content = """<tool_call>
{"name": "read_file", "arguments": {"path": "a.py"}}
</tool_call>
<tool_call>
{"name": "read_file", "arguments": {"path": "b.py"}}
</tool_call>"""

        _, calls = self.parser.parse(content)
        assert len(calls) == 2

    def test_regex_fallback(self) -> None:
        """Test regex extraction as fallback."""
        content = """<tool_call>
completely broken {"name": "test_tool", something "arguments": {"key": "value"}}
</tool_call>"""

        _, calls = self.parser.parse(content)
        # Should extract at least the name
        assert len(calls) == 1
        assert calls[0].name == "test_tool"

    def test_no_tool_calls(self) -> None:
        """Test content with no tool calls."""
        content = "Just a regular response."
        cleaned, calls = self.parser.parse(content)
        assert cleaned == content
        assert len(calls) == 0

    def test_custom_markers(self) -> None:
        """Test custom start/end markers."""
        content = """[[TOOL]]
{"name": "test", "arguments": {}}
[[/TOOL]]"""

        _, calls = self.parser.parse(content, "[[TOOL]]", "[[/TOOL]]")
        assert len(calls) == 1
        assert calls[0].name == "test"


class TestTokenBudget:
    """Tests for token budget."""

    def test_from_context_length(self) -> None:
        """Test creating budget from context length."""
        budget = TokenBudget.from_context_length(16384)

        assert budget.total == 16384
        assert budget.system == 2000
        assert budget.history == 10384  # 16384 - 6000
        assert budget.tools == 1000
        assert budget.response == 4000

    def test_custom_budget(self) -> None:
        """Test creating custom budget."""
        budget = TokenBudget(
            total=8192,
            system=1000,
            history=5000,
            tools=500,
            response=2000,
        )
        assert budget.total == 8192


class TestContextBuilder:
    """Tests for context builder."""

    def test_build_system_prompt_returns_base(self) -> None:
        """Test build_system_prompt returns the base prompt."""
        from entropi.config.schema import EntropyConfig

        config = EntropyConfig()
        builder = ContextBuilder(config)

        prompt = builder.build_system_prompt("Test base prompt")
        assert "Test base prompt" in prompt

    def test_estimate_tokens(self) -> None:
        """Test token estimation."""
        from entropi.config.schema import EntropyConfig

        config = EntropyConfig()
        builder = ContextBuilder(config)

        # ~4 chars per token
        assert builder.estimate_tokens("hello world") == 2  # 11 // 4 = 2
        assert builder.estimate_tokens("a" * 100) == 25  # 100 // 4 = 25

    def test_build_system_prompt_with_project_context(self) -> None:
        """Test building system prompt appends project context."""
        from entropi.config.schema import EntropyConfig

        config = EntropyConfig()
        builder = ContextBuilder(config)
        builder._project_context = "Project-specific info here"

        prompt = builder.build_system_prompt("Base prompt")
        assert "Base prompt" in prompt
        assert "Project Context" in prompt
        assert "Project-specific info here" in prompt


class _EngineTestBase:
    """Shared helper for engine tests that need a mocked engine."""

    def _make_engine(self):
        """Create an Engine with mocked dependencies."""
        with patch("entropi.core.engine.AgentEngine.__init__", return_value=None):
            engine = AgentEngine.__new__(AgentEngine)
        engine.loop_config = LoopConfig()
        engine._on_state_change = None
        engine._todo_list = MagicMock()
        engine._todo_list.is_empty = True
        return engine


class TestCompactionAfterToolResults(_EngineTestBase):
    """Compaction must be checked after each tool result."""

    @pytest.mark.asyncio
    async def test_compaction_called_after_tool_result(self) -> None:
        """_check_compaction is called after each tool result."""
        engine = self._make_engine()

        ctx = LoopContext(
            messages=[Message(role="system", content="test")],
        )

        calls = [
            ToolCall(id="1", name="bash.execute", arguments={"command": "ls"}),
            ToolCall(id="2", name="bash.execute", arguments={"command": "pwd"}),
        ]

        compaction_calls = []

        async def mock_execute(ctx, tool_call):
            return Message(role="user", content=f"Result of {tool_call.name}")

        async def mock_check_compaction(ctx, *, force=False):
            compaction_calls.append(force)

        engine._execute_tool = mock_execute
        engine._set_state = MagicMock()
        engine._check_duplicate_tool_call = MagicMock(return_value=None)
        engine._record_tool_call = MagicMock()
        engine._check_compaction = mock_check_compaction

        messages = []
        async for msg in engine._process_tool_calls(ctx, calls):
            messages.append(msg)

        assert len(messages) == 2
        # _check_compaction called once per tool result
        assert len(compaction_calls) == 2
        assert all(f is False for f in compaction_calls)


class TestOverflowRecovery(_EngineTestBase):
    """Context overflow triggers forced compaction, not error state."""

    @pytest.mark.asyncio
    async def test_overflow_triggers_forced_compaction(self) -> None:
        """ValueError with 'exceed context window' triggers forced compaction."""
        engine = self._make_engine()

        ctx = LoopContext(
            messages=[Message(role="system", content="test")],
        )

        error = ValueError("Requested tokens (16875) exceed context window of 16384")

        compaction_calls = []

        async def mock_check_compaction(ctx, *, force=False):
            compaction_calls.append(force)

        engine._check_compaction = mock_check_compaction
        engine._set_state = MagicMock()

        messages = []
        async for msg in engine._handle_error(ctx, error):
            messages.append(msg)

        # Should trigger forced compaction
        assert len(compaction_calls) == 1
        assert compaction_calls[0] is True
        # Should NOT increment error counter
        assert ctx.consecutive_errors == 0
        assert ctx.metrics.errors == 0
        # Should NOT yield any error messages
        assert len(messages) == 0

    @pytest.mark.asyncio
    async def test_non_overflow_error_increments_counter(self) -> None:
        """Non-overflow errors still increment the error counter."""
        engine = self._make_engine()

        ctx = LoopContext(
            messages=[Message(role="system", content="test")],
        )

        error = RuntimeError("Some other error")
        engine._set_state = MagicMock()

        messages = []
        async for msg in engine._handle_error(ctx, error):
            messages.append(msg)

        assert ctx.consecutive_errors == 1
        assert ctx.metrics.errors == 1


class TestHandoffStopsToolProcessing(_EngineTestBase):
    """Handoff must stop processing remaining tool calls."""

    @pytest.mark.asyncio
    async def test_handoff_stops_remaining_tool_calls(self) -> None:
        """Tool calls after a successful handoff are dropped."""
        engine = self._make_engine()

        ctx = LoopContext(
            messages=[Message(role="system", content="test")],
        )

        calls = [
            ToolCall(id="1", name="bash.execute", arguments={"command": "ls"}),
            ToolCall(id="2", name="system.handoff", arguments={"target_tier": "code"}),
            ToolCall(id="3", name="entropi.todo_write", arguments={"todos": []}),
        ]

        executed = []

        async def mock_execute(ctx, tool_call):
            executed.append(tool_call.name)
            if tool_call.name == "system.handoff":
                return Message(
                    role="user",
                    content="Handoff successful. Now operating as code tier. Reason: plan ready",
                )
            return Message(role="user", content=f"Result of {tool_call.name}")

        async def mock_check_compaction(ctx, *, force=False):
            pass

        engine._execute_tool = mock_execute
        engine._set_state = MagicMock()
        engine._check_duplicate_tool_call = MagicMock(return_value=None)
        engine._record_tool_call = MagicMock()
        engine._check_compaction = mock_check_compaction

        messages = []
        async for msg in engine._process_tool_calls(ctx, calls):
            messages.append(msg)

        assert executed == ["bash.execute", "system.handoff"]
        assert len(messages) == 2
        assert "Handoff successful" in messages[1].content

    @pytest.mark.asyncio
    async def test_failed_handoff_continues_processing(self) -> None:
        """Tool calls continue after a failed handoff."""
        engine = self._make_engine()

        ctx = LoopContext(
            messages=[Message(role="system", content="test")],
        )

        calls = [
            ToolCall(id="1", name="system.handoff", arguments={"target_tier": "invalid"}),
            ToolCall(id="2", name="bash.execute", arguments={"command": "ls"}),
        ]

        executed = []

        async def mock_execute(ctx, tool_call):
            executed.append(tool_call.name)
            if tool_call.name == "system.handoff":
                return Message(role="user", content="Error: Invalid target tier: invalid")
            return Message(role="user", content=f"Result of {tool_call.name}")

        async def mock_check_compaction(ctx, *, force=False):
            pass

        engine._execute_tool = mock_execute
        engine._set_state = MagicMock()
        engine._check_duplicate_tool_call = MagicMock(return_value=None)
        engine._record_tool_call = MagicMock()
        engine._check_compaction = mock_check_compaction

        messages = []
        async for msg in engine._process_tool_calls(ctx, calls):
            messages.append(msg)

        assert executed == ["system.handoff", "bash.execute"]
        assert len(messages) == 2


class TestHandoffGate(_EngineTestBase):
    """Handoff gate rejects when todos exist but none target the handoff tier."""

    def _make_handoff_engine(self):
        """Create engine with mocked dependencies for handoff testing."""
        engine = self._make_engine()
        engine.orchestrator = MagicMock()
        engine.orchestrator.can_handoff = MagicMock(return_value=True)
        engine._build_formatted_system_prompt = MagicMock(return_value="system prompt")
        engine._log_assembled_prompt = MagicMock()
        engine._notify_tier_selected = MagicMock()
        engine._inject_todo_state = MagicMock()
        engine._create_error_message = lambda tc, msg: Message(role="user", content=f"Error: {msg}")
        return engine

    @pytest.mark.asyncio
    async def test_handoff_rejected_when_todos_exist_but_none_target_tier(self) -> None:
        """Handoff rejected when todos exist but none target the requested tier."""
        from entropi.core.todos import TodoList

        engine = self._make_handoff_engine()
        engine._todo_list = TodoList()
        engine._todo_list.update_from_tool_call(
            [
                {
                    "content": "Read files",
                    "active_form": "Reading",
                    "status": "completed",
                },
                {
                    "content": "Investigate patterns",
                    "active_form": "Investigating",
                    "status": "pending",
                },
            ]
        )

        ctx = LoopContext(
            messages=[Message(role="system", content="test")],
        )

        tool_call = ToolCall(
            id="1",
            name="system.handoff",
            arguments={"target_tier": "code", "reason": "plan ready"},
        )

        msg = await engine._handle_handoff(ctx, tool_call)
        assert "Error" in msg.content
        assert "No todos targeting code tier" in msg.content

    @pytest.mark.asyncio
    async def test_handoff_allowed_when_todos_target_tier(self) -> None:
        """Handoff allowed when todos target the requested tier."""
        from entropi.core.todos import TodoList

        engine = self._make_handoff_engine()
        engine._todo_list = TodoList()
        engine._todo_list.update_from_tool_call(
            [
                {
                    "content": "Fix engine.py",
                    "active_form": "Fixing",
                    "status": "pending",
                    "target_tier": "code",
                },
            ]
        )

        ctx = LoopContext(
            messages=[Message(role="system", content="test")],
        )

        tool_call = ToolCall(
            id="1",
            name="system.handoff",
            arguments={"target_tier": "code", "reason": "plan ready"},
        )

        msg = await engine._handle_handoff(ctx, tool_call)
        assert "Handoff successful" in msg.content

    @pytest.mark.asyncio
    async def test_handoff_allowed_when_todo_list_empty(self) -> None:
        """Handoff allowed when no todos at all (not all interactions need plans)."""
        from entropi.core.todos import TodoList

        engine = self._make_handoff_engine()
        engine._todo_list = TodoList()

        ctx = LoopContext(
            messages=[Message(role="system", content="test")],
        )

        tool_call = ToolCall(
            id="1",
            name="system.handoff",
            arguments={"target_tier": "code", "reason": "simple task"},
        )

        msg = await engine._handle_handoff(ctx, tool_call)
        assert "Handoff successful" in msg.content
