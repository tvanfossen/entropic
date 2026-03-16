"""Tests for dedicated display mirror logger."""

from pathlib import Path

from entropic.core.logging import get_display_logger, setup_display_logger


class TestSetupDisplayLogger:
    """Tests for setup_display_logger."""

    def test_creates_logger_with_correct_name(self, tmp_path: Path) -> None:
        """Display logger uses entropic.display namespace."""
        logger = setup_display_logger(project_dir=tmp_path)
        assert logger.name == "entropic.display"

    def test_creates_log_file(self, tmp_path: Path) -> None:
        """Display logger creates session_display.log in project dir."""
        setup_display_logger(project_dir=tmp_path)
        assert (tmp_path / ".entropic" / "session_display.log").exists()

    def test_writes_startup_message(self, tmp_path: Path) -> None:
        """Display logger writes startup message on creation."""
        setup_display_logger(project_dir=tmp_path)
        content = (tmp_path / ".entropic" / "session_display.log").read_text()
        assert "Display mirror log started" in content

    def test_does_not_propagate(self, tmp_path: Path) -> None:
        """Display logger doesn't propagate to parent (keeps session.log clean)."""
        logger = setup_display_logger(project_dir=tmp_path)
        assert logger.propagate is False

    def test_custom_app_dir_name(self, tmp_path: Path) -> None:
        """Display logger respects custom app directory name."""
        setup_display_logger(project_dir=tmp_path, app_dir_name=".myapp")
        assert (tmp_path / ".myapp" / "session_display.log").exists()

    def test_get_display_logger_returns_same_logger(self, tmp_path: Path) -> None:
        """get_display_logger returns the same logger instance."""
        setup_display_logger(project_dir=tmp_path)
        logger = get_display_logger()
        assert logger.name == "entropic.display"

    def test_writes_display_events(self, tmp_path: Path) -> None:
        """Display logger captures logged events."""
        logger = setup_display_logger(project_dir=tmp_path)
        logger.info("[TIER] ENG")
        logger.info("[GEN START]")
        logger.info("[TOOL START] filesystem.read_file")

        content = (tmp_path / ".entropic" / "session_display.log").read_text()
        assert "[TIER] ENG" in content
        assert "[GEN START]" in content
        assert "[TOOL START]" in content

    def test_overwrites_on_new_session(self, tmp_path: Path) -> None:
        """Each session starts fresh (mode='w')."""
        logger = setup_display_logger(project_dir=tmp_path)
        logger.info("first session")

        # Re-setup (simulates new session)
        setup_display_logger(project_dir=tmp_path)
        content = (tmp_path / ".entropic" / "session_display.log").read_text()
        assert "first session" not in content
