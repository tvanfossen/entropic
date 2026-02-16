"""Tests for dedicated model output logger."""

import logging
from pathlib import Path
from unittest.mock import MagicMock

from entropi.core.base import Message, ToolCall
from entropi.core.engine import LoopContext, LoopMetrics
from entropi.core.logging import get_model_logger, setup_model_logger
from entropi.core.todos import TodoItem, TodoList, TodoStatus


class TestSetupModelLogger:
    """Tests for setup_model_logger."""

    def test_creates_logger_with_correct_name(self, tmp_path: Path) -> None:
        """Model logger uses entropi.model_output namespace."""
        logger = setup_model_logger(project_dir=tmp_path)
        assert logger.name == "entropi.model_output"

    def test_writes_to_session_model_log(self, tmp_path: Path) -> None:
        """Model logger writes to .entropi/session_model.log."""
        logger = setup_model_logger(project_dir=tmp_path)
        logger.info("test message")

        log_path = tmp_path / ".entropi" / "session_model.log"
        assert log_path.exists()
        content = log_path.read_text()
        assert "test message" in content

    def test_does_not_propagate_to_parent(self, tmp_path: Path) -> None:
        """Model logger does not propagate to entropi parent logger."""
        logger = setup_model_logger(project_dir=tmp_path)
        assert logger.propagate is False

    def test_overwrites_on_new_session(self, tmp_path: Path) -> None:
        """Model log uses mode='w' to start fresh each session."""
        logger = setup_model_logger(project_dir=tmp_path)
        logger.info("first session")

        # Simulate new session — setup again
        logger = setup_model_logger(project_dir=tmp_path)
        logger.info("second session")

        log_path = tmp_path / ".entropi" / "session_model.log"
        content = log_path.read_text()
        assert "first session" not in content
        assert "second session" in content

    def test_minimal_format_no_log_level(self, tmp_path: Path) -> None:
        """Model log format is timestamp + message, no level prefix."""
        logger = setup_model_logger(project_dir=tmp_path)
        logger.info("raw output here")

        log_path = tmp_path / ".entropi" / "session_model.log"
        content = log_path.read_text()
        # Should NOT contain log level markers
        assert "[INFO]" not in content
        assert "raw output here" in content

    def test_get_model_logger_returns_same_logger(self, tmp_path: Path) -> None:
        """get_model_logger returns the same logger instance."""
        setup_model_logger(project_dir=tmp_path)
        logger = get_model_logger()
        assert logger.name == "entropi.model_output"

    def test_creates_entropi_directory(self, tmp_path: Path) -> None:
        """Creates .entropi/ directory if it doesn't exist."""
        project = tmp_path / "newproject"
        project.mkdir()
        setup_model_logger(project_dir=project)

        assert (project / ".entropi").is_dir()


class TestEngineModelLogging:
    """Tests for _log_model_output in AgentEngine."""

    def test_log_model_output_writes_to_model_logger(self, tmp_path: Path) -> None:
        """_log_model_output writes raw and parsed output to model logger."""
        from entropi.core.engine import AgentEngine

        setup_model_logger(project_dir=tmp_path)

        # Create engine with minimal mocks
        engine = AgentEngine.__new__(AgentEngine)
        ctx = LoopContext(metrics=LoopMetrics(iterations=3))

        raw = "<think>planning</think>\nHere is my response."
        cleaned = "Here is my response."
        tool_calls: list[ToolCall] = []

        engine._log_model_output(
            ctx,
            raw_content=raw,
            cleaned_content=cleaned,
            tool_calls=tool_calls,
            finish_reason="stop",
        )

        log_path = tmp_path / ".entropi" / "session_model.log"
        content = log_path.read_text()
        assert "[TURN 3]" in content
        assert "finish_reason=stop" in content
        assert "RAW OUTPUT" in content
        assert "<think>planning</think>" in content
        assert "cleaned_content_len=20" in content
        assert "tool_calls=0" in content

    def test_log_model_output_includes_tool_call_names(self, tmp_path: Path) -> None:
        """_log_model_output lists tool call names in parsed section."""
        from entropi.core.engine import AgentEngine

        setup_model_logger(project_dir=tmp_path)

        engine = AgentEngine.__new__(AgentEngine)
        ctx = LoopContext(metrics=LoopMetrics(iterations=1))

        tool_calls = [
            ToolCall(id="1", name="filesystem.read_file", arguments={"path": "a.py"}),
            ToolCall(id="2", name="bash.run", arguments={"command": "ls"}),
        ]

        engine._log_model_output(
            ctx,
            raw_content="raw",
            cleaned_content="cleaned",
            tool_calls=tool_calls,
            finish_reason="stop",
        )

        log_path = tmp_path / ".entropi" / "session_model.log"
        content = log_path.read_text()
        assert "tool_calls=2" in content
        assert "filesystem.read_file" in content
        assert "bash.run" in content

    def test_log_model_output_session_log_summary_only(
        self, tmp_path: Path, caplog: MagicMock
    ) -> None:
        """Session logger gets summary, not full raw output."""
        from entropi.core.engine import AgentEngine

        setup_model_logger(project_dir=tmp_path)

        engine = AgentEngine.__new__(AgentEngine)
        ctx = LoopContext(metrics=LoopMetrics(iterations=2))

        raw = "A very long raw output " * 100

        with caplog.at_level(logging.INFO, logger="entropi.core.engine"):
            engine._log_model_output(
                ctx,
                raw_content=raw,
                cleaned_content="short",
                tool_calls=[],
                finish_reason="stop",
            )

        # Session log should have summary line, not the full raw output
        session_lines = [r.message for r in caplog.records]
        summary = next(m for m in session_lines if "[MODEL OUTPUT]" in m)
        assert "Turn 2" in summary
        assert "finish_reason=stop" in summary
        assert "tool_calls=0" in summary
        # Full raw content should NOT be in session log
        assert raw not in summary


class TestAssembledPromptLogging:
    """Tests for _log_assembled_prompt in AgentEngine."""

    def test_logs_all_messages_untruncated(self, tmp_path: Path) -> None:
        """Full message content appears in model log, no truncation."""
        from entropi.core.engine import AgentEngine

        setup_model_logger(project_dir=tmp_path)

        engine = AgentEngine.__new__(AgentEngine)
        long_system = "You are an AI assistant. " * 200  # ~5000 chars
        ctx = LoopContext(
            metrics=LoopMetrics(iterations=0),
            messages=[
                Message(role="system", content=long_system),
                Message(role="user", content="Hello world"),
            ],
        )
        # Mock locked_tier
        ctx.locked_tier = MagicMock()
        ctx.locked_tier.value = "thinking"

        engine._log_assembled_prompt(ctx, "routed")

        log_path = tmp_path / ".entropi" / "session_model.log"
        content = log_path.read_text()
        assert "[PROMPT] event=routed tier=thinking messages=2" in content
        # Full system prompt must be present — no truncation
        assert long_system in content
        assert "Hello world" in content

    def test_includes_roles_and_lengths(self, tmp_path: Path) -> None:
        """Role and char count header appears per message."""
        from entropi.core.engine import AgentEngine

        setup_model_logger(project_dir=tmp_path)

        engine = AgentEngine.__new__(AgentEngine)
        ctx = LoopContext(
            metrics=LoopMetrics(iterations=0),
            messages=[
                Message(role="system", content="sys prompt"),
                Message(role="user", content="user msg"),
                Message(role="assistant", content="asst reply"),
            ],
        )
        ctx.locked_tier = MagicMock()
        ctx.locked_tier.value = "normal"

        engine._log_assembled_prompt(ctx, "routed")

        log_path = tmp_path / ".entropi" / "session_model.log"
        content = log_path.read_text()
        assert "[0] system (10 chars):" in content
        assert "[1] user (8 chars):" in content
        assert "[2] assistant (10 chars):" in content

    def test_routing_logged_to_model_logger(self, tmp_path: Path) -> None:
        """[ROUTED] event appears in model log."""
        setup_model_logger(project_dir=tmp_path)

        ml = get_model_logger()
        ml.info(f"\n{'#' * 70}\n" f"[ROUTED] tier=simple\n" f"{'#' * 70}")

        log_path = tmp_path / ".entropi" / "session_model.log"
        content = log_path.read_text()
        assert "[ROUTED] tier=simple" in content


class TestTodoCompactionPersistence:
    """Tests for todo state injection after compaction."""

    def test_format_for_context_renders_all_statuses(self) -> None:
        """Pending, in_progress, and completed all render with correct icons."""
        todo_list = TodoList()
        todo_list.items = [
            TodoItem(content="Read file", active_form="Reading file", status=TodoStatus.COMPLETED),
            TodoItem(
                content="Write tests", active_form="Writing tests", status=TodoStatus.IN_PROGRESS
            ),
            TodoItem(content="Run lint", active_form="Running lint", status=TodoStatus.PENDING),
        ]

        result = todo_list.format_for_context()
        assert "[CURRENT TODO STATE]" in result
        assert "[x] Read file" in result
        assert "[>] Write tests" in result
        assert "[ ] Run lint" in result
        assert "Progress: 1/3 completed" in result
        assert "[END TODO STATE]" in result

    def test_format_for_context_empty_returns_empty_string(self) -> None:
        """Empty todo list returns empty string (no injection)."""
        todo_list = TodoList()
        assert todo_list.format_for_context() == ""

    def test_context_anchor_created(self) -> None:
        """Context anchor is appended with correct metadata."""
        from entropi.core.directives import ContextAnchor, DirectiveResult
        from entropi.core.engine import AgentEngine

        engine = AgentEngine.__new__(AgentEngine)
        engine._context_anchors = {}

        # Build state from a TodoList (as the server would)
        todo_list = TodoList()
        todo_list.items = [
            TodoItem(content="Fix bug", active_form="Fixing bug", status=TodoStatus.IN_PROGRESS),
            TodoItem(content="Add test", active_form="Adding test", status=TodoStatus.PENDING),
        ]
        state = todo_list.format_for_context()

        ctx = LoopContext(
            messages=[
                Message(role="system", content="sys"),
                Message(role="user", content="[CONVERSATION SUMMARY] ..."),
            ],
        )

        engine._directive_context_anchor(
            ctx, ContextAnchor(key="todo_state", content=state), DirectiveResult()
        )

        assert len(ctx.messages) == 3
        anchor_msg = ctx.messages[2]
        assert anchor_msg.role == "user"
        assert anchor_msg.metadata.get("is_context_anchor") is True
        assert anchor_msg.metadata.get("anchor_key") == "todo_state"
        assert "[CURRENT TODO STATE]" in anchor_msg.content
        assert "[>] Fix bug" in anchor_msg.content
        assert "[ ] Add test" in anchor_msg.content

    def test_context_anchor_replaces_existing(self) -> None:
        """Updating anchor removes old one and appends new at end."""
        from entropi.core.directives import ContextAnchor, DirectiveResult
        from entropi.core.engine import AgentEngine

        engine = AgentEngine.__new__(AgentEngine)
        engine._context_anchors = {}

        ctx = LoopContext(messages=[Message(role="system", content="sys")])
        engine._directive_context_anchor(
            ctx, ContextAnchor(key="todo_state", content="state v1"), DirectiveResult()
        )
        assert len(ctx.messages) == 2  # system + anchor

        # Add an assistant message after
        ctx.messages.append(Message(role="assistant", content="reply"))

        # Update anchor
        engine._directive_context_anchor(
            ctx, ContextAnchor(key="todo_state", content="state v2"), DirectiveResult()
        )

        # Still only 1 anchor, now at end
        anchors = [m for m in ctx.messages if m.metadata.get("is_context_anchor")]
        assert len(anchors) == 1
        assert ctx.messages[-1].content == "state v2"

    def test_empty_anchor_removes_existing(self) -> None:
        """Empty content removes the anchor from context."""
        from entropi.core.directives import ContextAnchor, DirectiveResult
        from entropi.core.engine import AgentEngine

        engine = AgentEngine.__new__(AgentEngine)
        engine._context_anchors = {}

        ctx = LoopContext(
            messages=[
                Message(role="system", content="sys"),
                Message(role="user", content="summary"),
            ],
        )

        # Create anchor
        engine._directive_context_anchor(
            ctx, ContextAnchor(key="todo_state", content="state"), DirectiveResult()
        )
        assert len(ctx.messages) == 3

        # Remove with empty content
        engine._directive_context_anchor(
            ctx, ContextAnchor(key="todo_state", content=""), DirectiveResult()
        )
        assert len(ctx.messages) == 2  # Anchor removed
