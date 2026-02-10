"""Tests for dedicated model output logger."""

import logging
from pathlib import Path
from unittest.mock import MagicMock

from entropi.core.base import ToolCall
from entropi.core.engine import LoopContext, LoopMetrics
from entropi.core.logging import get_model_logger, setup_model_logger


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
