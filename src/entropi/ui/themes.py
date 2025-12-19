"""
UI themes for terminal interface.

Provides consistent color schemes across the application.
"""

from dataclasses import dataclass
from typing import Literal


@dataclass(frozen=True)
class Theme:
    """Color theme definition."""

    name: str

    # Base colors
    text_color: str
    background_color: str
    border_color: str

    # Accent colors
    accent_color: str
    prompt_color: str

    # Role colors
    user_color: str
    assistant_color: str
    tool_color: str

    # Status colors
    success_color: str
    warning_color: str
    error_color: str
    info_color: str


# Built-in themes
DARK_THEME = Theme(
    name="dark",
    text_color="white",
    background_color="black",
    border_color="bright_black",
    accent_color="cyan",
    prompt_color="green",
    user_color="blue",
    assistant_color="green",
    tool_color="yellow",
    success_color="green",
    warning_color="yellow",
    error_color="red",
    info_color="cyan",
)

LIGHT_THEME = Theme(
    name="light",
    text_color="black",
    background_color="white",
    border_color="bright_black",
    accent_color="blue",
    prompt_color="blue",
    user_color="blue",
    assistant_color="green",
    tool_color="magenta",
    success_color="green",
    warning_color="yellow",
    error_color="red",
    info_color="blue",
)

THEMES = {
    "dark": DARK_THEME,
    "light": LIGHT_THEME,
}


def get_theme(name: Literal["dark", "light", "auto"]) -> Theme:
    """
    Get theme by name.

    Args:
        name: Theme name

    Returns:
        Theme instance
    """
    if name == "auto":
        # Could detect terminal theme here
        return DARK_THEME

    return THEMES.get(name, DARK_THEME)
