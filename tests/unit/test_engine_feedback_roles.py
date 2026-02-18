"""Test feedback message roles and circuit breaker functionality.

These tests verify fixes for the infinite loop bug where models didn't receive
feedback because role="tool" messages aren't rendered by llama-cpp.
"""

from unittest.mock import AsyncMock, MagicMock, patch

import pytest
from entropi.core.base import Message, ToolCall
from entropi.core.engine import AgentEngine, LoopContext


class TestFeedbackMessageRoles:
    """Test that feedback messages use role='user' so llama-cpp renders them."""

    @pytest.fixture
    def engine(self):
        """Create engine with minimal mocking."""
        orchestrator = MagicMock()
        server_manager = MagicMock()
        config = MagicMock()
        config.compaction = MagicMock()
        config.models = MagicMock()
        config.models.default = "normal"
        config.models.normal = MagicMock(context_length=16384)

        with patch.object(AgentEngine, "_get_max_context_tokens", return_value=16384):
            return AgentEngine(orchestrator, server_manager, config)

    @pytest.fixture
    def tool_call(self):
        """Create a sample tool call."""
        return ToolCall(
            id="test-123", name="filesystem.read_file", arguments={"path": "/test/file.py"}
        )

    def test_duplicate_message_uses_user_role(self, engine, tool_call):
        """Verify _create_duplicate_message returns role='user'."""
        previous_result = "file contents here..."
        msg = engine._create_duplicate_message(tool_call, previous_result)

        assert msg.role == "user", f"Expected role='user', got role='{msg.role}'"
        assert "already called" in msg.content.lower()
        assert "filesystem.read_file" in msg.content
        assert previous_result in msg.content

    def test_denied_message_uses_user_role(self, engine, tool_call):
        """Verify _create_denied_message returns role='user'."""
        reason = "Permission denied by user"
        msg = engine._create_denied_message(tool_call, reason)

        assert msg.role == "user", f"Expected role='user', got role='{msg.role}'"
        assert "denied" in msg.content.lower()
        assert reason in msg.content

    def test_error_message_uses_user_role(self, engine, tool_call):
        """Verify _create_error_message returns role='user'."""
        error = "Connection timeout"
        msg = engine._create_error_message(tool_call, error)

        assert msg.role == "user", f"Expected role='user', got role='{msg.role}'"
        assert "failed" in msg.content.lower() or "error" in msg.content.lower()
        assert error in msg.content


class TestCircuitBreaker:
    """Test circuit breaker for preventing infinite loops."""

    def test_consecutive_duplicate_counter_exists(self):
        """Verify LoopContext has consecutive_duplicate_attempts field."""
        ctx = LoopContext()
        assert hasattr(ctx, "consecutive_duplicate_attempts")
        assert ctx.consecutive_duplicate_attempts == 0

    def test_should_stop_on_duplicate_threshold(self):
        """Verify _should_stop returns True when duplicate threshold reached."""
        orchestrator = MagicMock()
        server_manager = MagicMock()
        config = MagicMock()
        config.compaction = MagicMock()
        config.models = MagicMock()
        config.models.default = "normal"
        config.models.normal = MagicMock(context_length=16384)

        with patch.object(AgentEngine, "_get_max_context_tokens", return_value=16384):
            engine = AgentEngine(orchestrator, server_manager, config)

        ctx = LoopContext()

        # Below threshold - should continue
        ctx.consecutive_duplicate_attempts = 2
        assert engine._should_stop(ctx) is False

        # At threshold - should stop
        ctx.consecutive_duplicate_attempts = 3
        assert engine._should_stop(ctx) is True

    @pytest.mark.asyncio
    async def test_process_tool_calls_increments_duplicate_counter(self):
        """Verify duplicate detection increments counter."""
        orchestrator = MagicMock()
        server_manager = MagicMock()
        config = MagicMock()
        config.compaction = MagicMock()
        config.models = MagicMock()
        config.models.default = "normal"
        config.models.normal = MagicMock(context_length=16384)

        server_manager.skip_duplicate_check = MagicMock(return_value=False)

        with patch.object(AgentEngine, "_get_max_context_tokens", return_value=16384):
            engine = AgentEngine(orchestrator, server_manager, config)

        ctx = LoopContext()
        tool_call = ToolCall(id="1", name="bash.execute", arguments={"command": "ls"})

        # Pre-populate with a previous call result
        key = engine._get_tool_call_key(tool_call)
        ctx.recent_tool_calls[key] = "previous result"

        # Process the duplicate tool call
        messages = []
        async for msg in engine._process_tool_calls(ctx, [tool_call]):
            messages.append(msg)

        assert (
            ctx.consecutive_duplicate_attempts == 1
        ), f"Expected counter=1, got {ctx.consecutive_duplicate_attempts}"
        assert len(messages) == 1
        assert messages[0].role == "user"

    @pytest.mark.asyncio
    async def test_process_tool_calls_resets_counter_on_success(self):
        """Verify successful tool execution resets duplicate counter."""
        orchestrator = MagicMock()
        orchestrator.get_adapter = MagicMock(
            return_value=MagicMock(
                format_tool_result=MagicMock(return_value=Message(role="user", content="result"))
            )
        )

        server_manager = MagicMock()
        server_manager.execute = AsyncMock(return_value=MagicMock(result="success"))

        config = MagicMock()
        config.compaction = MagicMock()
        config.models = MagicMock()
        config.models.default = "normal"
        config.models.normal = MagicMock(context_length=16384)

        with patch.object(AgentEngine, "_get_max_context_tokens", return_value=16384):
            engine = AgentEngine(orchestrator, server_manager, config)

        # Mock approval, compaction, and context warning
        engine._on_tool_approval = None
        engine.loop_config.auto_approve_tools = True
        engine._check_compaction = AsyncMock()
        engine._inject_context_warning = MagicMock()

        ctx = LoopContext()
        ctx.consecutive_duplicate_attempts = 2  # Simulate previous duplicates

        tool_call = ToolCall(id="1", name="bash.execute", arguments={"command": "pwd"})

        # Process non-duplicate tool call
        messages = []
        async for msg in engine._process_tool_calls(ctx, [tool_call]):
            messages.append(msg)

        assert (
            ctx.consecutive_duplicate_attempts == 0
        ), f"Expected counter reset to 0, got {ctx.consecutive_duplicate_attempts}"

    @pytest.mark.asyncio
    async def test_circuit_breaker_triggers_at_threshold(self):
        """Verify circuit breaker sends STOP message at 3 consecutive duplicates."""
        orchestrator = MagicMock()
        server_manager = MagicMock()
        server_manager.skip_duplicate_check = MagicMock(return_value=False)
        config = MagicMock()
        config.compaction = MagicMock()
        config.models = MagicMock()
        config.models.default = "normal"
        config.models.normal = MagicMock(context_length=16384)

        with patch.object(AgentEngine, "_get_max_context_tokens", return_value=16384):
            engine = AgentEngine(orchestrator, server_manager, config)

        ctx = LoopContext()
        ctx.consecutive_duplicate_attempts = 2  # Already had 2 duplicates

        tool_call = ToolCall(id="1", name="bash.execute", arguments={"command": "ls"})
        key = engine._get_tool_call_key(tool_call)
        ctx.recent_tool_calls[key] = "previous result"

        # Process 3rd duplicate - should trigger circuit breaker
        messages = []
        async for msg in engine._process_tool_calls(ctx, [tool_call]):
            messages.append(msg)

        assert ctx.consecutive_duplicate_attempts == 3
        assert len(messages) == 1
        assert "STOP" in messages[0].content
        assert "stuck" in messages[0].content.lower()


class TestErrorSanitizer:
    """Test error_sanitizer callback filters errors before model sees them."""

    @pytest.fixture
    def engine(self):
        """Create engine with minimal mocking."""
        orchestrator = MagicMock()
        server_manager = MagicMock()
        config = MagicMock()
        config.compaction = MagicMock()
        config.models = MagicMock()
        config.models.default = "normal"
        config.models.normal = MagicMock(context_length=16384)

        with patch.object(AgentEngine, "_get_max_context_tokens", return_value=16384):
            return AgentEngine(orchestrator, server_manager, config)

    @pytest.fixture
    def tool_call(self):
        return ToolCall(id="test-456", name="bash.execute", arguments={"command": "cat .env"})

    def test_sanitizer_filters_error_in_model_message(self, engine, tool_call):
        """Sanitized error reaches model, raw error stays in logs."""
        from entropi.core.engine import EngineCallbacks

        def redact_secrets(error: str) -> str:
            return error.replace("password=hunter2", "password=***")

        engine.set_callbacks(EngineCallbacks(error_sanitizer=redact_secrets))

        ctx = LoopContext()
        error = Exception("Connection failed: password=hunter2")
        msg = engine._handle_tool_error(ctx, tool_call, error, 10.0, is_permission=False)

        # Model message should have sanitized error
        assert "password=***" in msg.content
        assert "password=hunter2" not in msg.content

    def test_no_sanitizer_passes_raw_error(self, engine, tool_call):
        """Without sanitizer, raw error reaches model unchanged."""
        ctx = LoopContext()
        error = Exception("password=hunter2")
        msg = engine._handle_tool_error(ctx, tool_call, error, 10.0, is_permission=False)

        assert "password=hunter2" in msg.content

    def test_sanitizer_applies_to_permission_errors(self, engine, tool_call):
        """Sanitizer also applies to permission denied messages."""
        from entropi.core.engine import EngineCallbacks

        engine.set_callbacks(
            EngineCallbacks(error_sanitizer=lambda e: e.replace("/secret/path", "***"))
        )

        ctx = LoopContext()
        error = Exception("Denied: /secret/path")
        msg = engine._handle_tool_error(ctx, tool_call, error, 10.0, is_permission=True)

        assert "***" in msg.content
        assert "/secret/path" not in msg.content
