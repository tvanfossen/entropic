"""Tests for agent engine."""

import threading
from unittest.mock import MagicMock, patch

import pytest
from entropi.core.base import Message, ToolCall, ToolResult
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
        from entropi.core.directives import DirectiveProcessor

        with patch("entropi.core.engine.AgentEngine.__init__", return_value=None):
            engine = AgentEngine.__new__(AgentEngine)
        engine.loop_config = LoopConfig()
        engine._on_state_change = None
        engine._context_anchors = {}
        engine._directive_processor = DirectiveProcessor()
        engine._register_directive_handlers()
        engine._inject_context_warning = MagicMock()
        engine._on_presenter_notify = None
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
            msg = Message(role="user", content=f"Result of {tool_call.name}")
            result = ToolResult(call_id=tool_call.id, name=tool_call.name, result=msg.content)
            return msg, result

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


class TestDirectiveStopsToolProcessing(_EngineTestBase):
    """Directive stop_processing must halt remaining tool calls."""

    @pytest.mark.asyncio
    async def test_stop_directive_stops_remaining_tool_calls(self) -> None:
        """Tool calls after a stop_processing directive are dropped."""
        engine = self._make_engine()

        ctx = LoopContext(
            messages=[Message(role="system", content="test")],
        )

        calls = [
            ToolCall(id="1", name="bash.execute", arguments={"command": "ls"}),
            ToolCall(id="2", name="entropi.handoff", arguments={"target_tier": "code"}),
            ToolCall(id="3", name="entropi.todo_write", arguments={"todos": []}),
        ]

        from entropi.core.directives import StopProcessing

        executed = []

        async def mock_execute(ctx, tool_call):
            executed.append(tool_call.name)
            msg = Message(role="user", content=f"Result of {tool_call.name}")
            directives = [StopProcessing()] if tool_call.name == "entropi.handoff" else []
            result = ToolResult(
                call_id=tool_call.id,
                name=tool_call.name,
                result=msg.content,
                directives=directives,
            )
            return msg, result

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

        assert executed == ["bash.execute", "entropi.handoff"]
        assert len(messages) == 2

    @pytest.mark.asyncio
    async def test_no_directive_continues_processing(self) -> None:
        """Tool calls continue when no stop directive is returned."""
        engine = self._make_engine()

        ctx = LoopContext(
            messages=[Message(role="system", content="test")],
        )

        calls = [
            ToolCall(id="1", name="entropi.handoff", arguments={"target_tier": "invalid"}),
            ToolCall(id="2", name="bash.execute", arguments={"command": "ls"}),
        ]

        executed = []

        async def mock_execute(ctx, tool_call):
            executed.append(tool_call.name)
            msg = Message(role="user", content=f"Result of {tool_call.name}")
            result = ToolResult(call_id=tool_call.id, name=tool_call.name, result=msg.content)
            return msg, result

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

        assert executed == ["entropi.handoff", "bash.execute"]
        assert len(messages) == 2


class TestDirectiveTierChange(_EngineTestBase):
    """Directive tier_change validates routing and updates context."""

    def _make_handoff_engine(self):
        """Create engine with mocked dependencies for directive testing."""
        engine = self._make_engine()
        engine.orchestrator = MagicMock()
        engine.orchestrator.can_handoff = MagicMock(return_value=True)
        engine._build_formatted_system_prompt = MagicMock(return_value="system prompt")
        engine._log_assembled_prompt = MagicMock()
        engine._notify_tier_selected = MagicMock()
        engine._reinject_context_anchors = MagicMock()
        return engine

    def test_tier_change_directive_updates_locked_tier(self) -> None:
        """tier_change directive updates ctx.locked_tier."""
        from entropi.core.directives import DirectiveResult, TierChange
        from entropi.inference.orchestrator import ModelTier

        engine = self._make_handoff_engine()

        ctx = LoopContext(
            messages=[Message(role="system", content="test")],
        )
        ctx.locked_tier = ModelTier.THINKING

        result = DirectiveResult()
        engine._directive_tier_change(
            ctx,
            TierChange(tier="code", reason="plan ready"),
            result,
        )

        assert ctx.locked_tier == ModelTier.CODE
        assert result.tier_changed is True
        engine._build_formatted_system_prompt.assert_called_once()

    def test_tier_change_directive_rejects_invalid_tier(self) -> None:
        """tier_change with invalid tier is a no-op."""
        from entropi.core.directives import DirectiveResult, TierChange

        engine = self._make_handoff_engine()

        ctx = LoopContext(
            messages=[Message(role="system", content="test")],
        )

        result = DirectiveResult()
        engine._directive_tier_change(
            ctx,
            TierChange(tier="nonexistent", reason="test"),
            result,
        )

        assert result.tier_changed is False

    def test_tier_change_directive_rejects_disallowed_route(self) -> None:
        """tier_change blocked when orchestrator rejects the route."""
        from entropi.core.directives import DirectiveResult, TierChange
        from entropi.inference.orchestrator import ModelTier

        engine = self._make_handoff_engine()
        engine.orchestrator.can_handoff = MagicMock(return_value=False)

        ctx = LoopContext(
            messages=[Message(role="system", content="test")],
        )
        ctx.locked_tier = ModelTier.CODE

        result = DirectiveResult()
        engine._directive_tier_change(
            ctx,
            TierChange(tier="thinking", reason="test"),
            result,
        )

        assert result.tier_changed is False
        assert ctx.locked_tier == ModelTier.CODE


class TestAutoPruneToolResults:
    """Auto-prune replaces old tool results with stubs."""

    def _make_engine_with_compaction(self):
        """Create engine with real compaction config for pruning tests."""
        from entropi.config.schema import CompactionConfig
        from entropi.core.compaction import CompactionManager, TokenCounter

        with patch("entropi.core.engine.AgentEngine.__init__", return_value=None):
            engine = AgentEngine.__new__(AgentEngine)

        config = CompactionConfig(tool_result_ttl=2)
        counter = TokenCounter(max_tokens=16384)
        engine._compaction_manager = CompactionManager(config, counter)
        return engine

    def test_prune_old_tool_results(self) -> None:
        """Tool results older than TTL are replaced with stubs."""
        engine = self._make_engine_with_compaction()

        ctx = LoopContext(
            messages=[
                Message(role="system", content="system"),
                Message(
                    role="user",
                    content="file contents here...",
                    tool_results=[{"name": "read_file", "result": "..."}],
                    metadata={"added_at_iteration": 0, "tool_name": "filesystem.read_file"},
                ),
                Message(role="assistant", content="I see the file."),
                Message(
                    role="user",
                    content="another file",
                    tool_results=[{"name": "read_file", "result": "..."}],
                    metadata={"added_at_iteration": 2, "tool_name": "filesystem.read_file"},
                ),
            ],
        )
        ctx.metrics.iterations = (
            3  # TTL=2: iteration 0 is stale (age 3), iteration 2 is fresh (age 1)
        )

        engine._prune_old_tool_results(ctx)

        # First tool result (iteration 0, age=3) should be pruned
        assert ctx.messages[1].content.startswith("[Previous:")
        assert "filesystem.read_file" in ctx.messages[1].content
        # Second tool result (iteration 2, age=1) should still be intact
        assert ctx.messages[3].content == "another file"

    def test_prune_preserves_recent(self) -> None:
        """Tool results within TTL are preserved."""
        engine = self._make_engine_with_compaction()

        ctx = LoopContext(
            messages=[
                Message(role="system", content="system"),
                Message(
                    role="user",
                    content="recent file",
                    tool_results=[{"name": "read_file", "result": "..."}],
                    metadata={"added_at_iteration": 2, "tool_name": "filesystem.read_file"},
                ),
            ],
        )
        ctx.metrics.iterations = 3  # TTL=2, iteration 2 is within TTL

        engine._prune_old_tool_results(ctx)

        assert ctx.messages[1].content == "recent file"


class TestPruneDirective:
    """prune_messages directive prunes old tool results."""

    def _make_engine_with_compaction(self):
        """Create engine with compaction manager for prune tests."""
        from entropi.config.schema import CompactionConfig
        from entropi.core.compaction import CompactionManager, TokenCounter

        with patch("entropi.core.engine.AgentEngine.__init__", return_value=None):
            engine = AgentEngine.__new__(AgentEngine)

        config = CompactionConfig()
        counter = TokenCounter(max_tokens=16384)
        engine._compaction_manager = CompactionManager(config, counter)
        return engine

    def test_prune_directive_replaces_old_results(self) -> None:
        """prune_messages directive replaces old tool results with stubs."""
        from entropi.core.directives import DirectiveResult

        engine = self._make_engine_with_compaction()

        ctx = LoopContext(
            messages=[
                Message(role="system", content="system"),
                Message(
                    role="user",
                    content="x" * 1000,
                    tool_results=[{"name": "read_file", "result": "..."}],
                    metadata={"tool_name": "filesystem.read_file"},
                ),
                Message(role="assistant", content="noted"),
                Message(
                    role="user",
                    content="y" * 500,
                    tool_results=[{"name": "bash", "result": "..."}],
                    metadata={"tool_name": "bash.execute"},
                ),
                Message(
                    role="user",
                    content="z" * 200,
                    tool_results=[{"name": "read_file", "result": "..."}],
                    metadata={"tool_name": "filesystem.read_file"},
                ),
            ],
        )

        from entropi.core.directives import PruneMessages

        result = DirectiveResult()
        engine._directive_prune_messages(ctx, PruneMessages(keep_recent=1), result)

        # Oldest 2 tool results pruned, newest 1 kept
        assert ctx.messages[1].content.startswith("[Previous:")
        assert ctx.messages[3].content.startswith("[Previous:")
        assert ctx.messages[4].content == "z" * 200  # Kept


class TestContextWarningInjection:
    """Context warning injected when usage exceeds threshold."""

    def _make_engine_with_compaction(self, max_tokens=1000, threshold=0.6):
        """Create engine with compaction for warning tests."""
        from entropi.config.schema import CompactionConfig
        from entropi.core.compaction import CompactionManager, TokenCounter

        with patch("entropi.core.engine.AgentEngine.__init__", return_value=None):
            engine = AgentEngine.__new__(AgentEngine)

        config = CompactionConfig(warning_threshold_percent=threshold)
        counter = TokenCounter(max_tokens=max_tokens)
        engine._compaction_manager = CompactionManager(config, counter)
        return engine

    def test_warning_injected_above_threshold(self) -> None:
        """Warning message injected when context usage exceeds threshold."""
        engine = self._make_engine_with_compaction(max_tokens=100, threshold=0.5)

        # Create context that uses ~70% (well above 50% threshold)
        ctx = LoopContext(
            messages=[
                Message(role="system", content="x" * 100),
                Message(role="user", content="y" * 100),
                Message(role="assistant", content="z" * 100),
            ],
        )
        ctx.metrics.iterations = 1
        initial_count = len(ctx.messages)

        engine._inject_context_warning(ctx)

        assert len(ctx.messages) == initial_count + 1
        assert "[CONTEXT WARNING]" in ctx.messages[-1].content
        assert ctx.metadata["last_warning_iteration"] == 1

    def test_warning_not_repeated_same_iteration(self) -> None:
        """Warning not injected twice in same iteration."""
        engine = self._make_engine_with_compaction(max_tokens=100, threshold=0.5)

        ctx = LoopContext(
            messages=[
                Message(role="system", content="x" * 100),
                Message(role="user", content="y" * 100),
            ],
        )
        ctx.metrics.iterations = 1

        engine._inject_context_warning(ctx)
        count_after_first = len(ctx.messages)

        engine._inject_context_warning(ctx)
        assert len(ctx.messages) == count_after_first  # No new message

    def test_no_warning_below_threshold(self) -> None:
        """No warning when context usage is below threshold."""
        engine = self._make_engine_with_compaction(max_tokens=10000, threshold=0.6)

        ctx = LoopContext(
            messages=[
                Message(role="system", content="short"),
                Message(role="user", content="hello"),
            ],
        )
        ctx.metrics.iterations = 1
        initial_count = len(ctx.messages)

        engine._inject_context_warning(ctx)

        assert len(ctx.messages) == initial_count


class TestStreamingInterrupt(_EngineTestBase):
    """Interrupt event must stop streaming mid-generation."""

    def _make_streaming_engine(self):
        """Create engine wired for streaming interrupt tests."""
        engine = self._make_engine()
        engine._interrupt_event = threading.Event()
        engine._pause_event = threading.Event()
        engine._on_stream_chunk = None
        engine._log_model_output = MagicMock()
        engine.orchestrator = MagicMock()
        engine.orchestrator.last_finish_reason = "stop"
        return engine

    @pytest.mark.asyncio
    async def test_interrupt_stops_streaming(self) -> None:
        """Setting _interrupt_event breaks out of stream loop."""
        engine = self._make_streaming_engine()

        chunks_yielded = 0

        async def fake_stream(*args, **kwargs):
            nonlocal chunks_yielded
            for word in ["Hello ", "world ", "this ", "should ", "stop "]:
                chunks_yielded += 1
                if chunks_yielded == 2:
                    engine._interrupt_event.set()
                yield word

        engine.orchestrator.generate_stream = fake_stream
        engine.orchestrator.get_adapter = MagicMock()
        engine.orchestrator.get_adapter().parse_tool_calls.return_value = ("Hello ", [])

        ctx = LoopContext(messages=[Message(role="system", content="test")])
        content, tool_calls = await engine._generate_streaming(ctx)

        # Should have stopped after chunk 2 set the interrupt
        assert chunks_yielded == 2
        assert "interrupted" in str(engine._log_model_output.call_args)

    @pytest.mark.asyncio
    async def test_no_interrupt_streams_fully(self) -> None:
        """Without interrupt, all chunks are consumed."""
        engine = self._make_streaming_engine()

        async def fake_stream(*args, **kwargs):
            for word in ["Hello ", "world "]:
                yield word

        engine.orchestrator.generate_stream = fake_stream
        engine.orchestrator.get_adapter = MagicMock()
        engine.orchestrator.get_adapter().parse_tool_calls.return_value = (
            "Hello world ",
            [],
        )

        ctx = LoopContext(messages=[Message(role="system", content="test")])
        content, _ = await engine._generate_streaming(ctx)

        assert content == "Hello world "
