"""Tests for UI components."""

from entropic.ui.components import StatusBar, ToolCallDisplay
from entropic.ui.themes import DARK_THEME, LIGHT_THEME, get_theme


class TestThemes:
    """Tests for theme system."""

    def test_get_dark_theme(self) -> None:
        """Test getting dark theme."""
        theme = get_theme("dark")
        assert theme == DARK_THEME
        assert theme.name == "dark"

    def test_get_light_theme(self) -> None:
        """Test getting light theme."""
        theme = get_theme("light")
        assert theme == LIGHT_THEME

    def test_auto_defaults_to_dark(self) -> None:
        """Test auto theme defaults to dark."""
        theme = get_theme("auto")
        assert theme == DARK_THEME

    def test_theme_has_required_colors(self) -> None:
        """Test theme has all required color attributes."""
        theme = get_theme("dark")
        assert theme.text_color
        assert theme.background_color
        assert theme.border_color
        assert theme.accent_color
        assert theme.prompt_color
        assert theme.user_color
        assert theme.assistant_color
        assert theme.tool_color
        assert theme.success_color
        assert theme.warning_color
        assert theme.error_color
        assert theme.info_color


class TestComponents:
    """Tests for UI components."""

    def test_status_bar_renders(self) -> None:
        """Test status bar rendering."""
        theme = get_theme("dark")
        status = StatusBar(
            model="14B",
            vram_used=10.5,
            vram_total=16.0,
            tokens=1234,
            theme=theme,
        )
        rendered = status.render()
        assert rendered is not None

    def test_status_bar_high_vram(self) -> None:
        """Test status bar with high VRAM usage."""
        theme = get_theme("dark")
        status = StatusBar(
            model="14B",
            vram_used=15.5,
            vram_total=16.0,
            tokens=1234,
            theme=theme,
        )
        rendered = status.render()
        assert rendered is not None

    def test_tool_call_display(self) -> None:
        """Test tool call display."""
        theme = get_theme("dark")
        display = ToolCallDisplay(
            name="read_file",
            arguments={"path": "test.py"},
            theme=theme,
        )
        rendered = display.render()
        assert rendered is not None

    def test_tool_call_display_empty_args(self) -> None:
        """Test tool call display with empty arguments."""
        theme = get_theme("dark")
        display = ToolCallDisplay(
            name="status",
            arguments={},
            theme=theme,
        )
        rendered = display.render()
        assert rendered is not None

    def test_tool_call_display_multiple_args(self) -> None:
        """Test tool call display with multiple arguments."""
        theme = get_theme("dark")
        display = ToolCallDisplay(
            name="write_file",
            arguments={"path": "test.py", "content": "hello"},
            theme=theme,
        )
        rendered = display.render()
        assert rendered is not None
