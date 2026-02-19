"""Terminal UI module for Entropi."""

from entropic.ui.components import (
    ConversationDisplay,
    HelpDisplay,
    StatusBar,
    StreamingText,
    TodoPanel,
    ToolCallDisplay,
)
from entropic.ui.themes import DARK_THEME, LIGHT_THEME, Theme, get_theme
from entropic.ui.tui import EntropiApp, PauseScreen, ToolApprovalScreen
from entropic.ui.widgets import (
    AssistantMessage,
    ContextBar,
    ProcessingIndicator,
    SpinnerFrames,
    StatusFooter,
    ThinkingBlock,
    TodoWidget,
    ToolCallWidget,
    UserMessage,
)

__all__ = [
    "AssistantMessage",
    "ContextBar",
    "ConversationDisplay",
    "DARK_THEME",
    "EntropiApp",
    "HelpDisplay",
    "LIGHT_THEME",
    "PauseScreen",
    "ProcessingIndicator",
    "SpinnerFrames",
    "StatusBar",
    "StatusFooter",
    "StreamingText",
    "Theme",
    "ThinkingBlock",
    "TodoPanel",
    "TodoWidget",
    "ToolApprovalScreen",
    "ToolCallDisplay",
    "ToolCallWidget",
    "UserMessage",
    "get_theme",
]
