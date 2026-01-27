"""Terminal UI module for Entropi."""

from entropi.ui.components import (
    ConversationDisplay,
    HelpDisplay,
    StatusBar,
    StreamingText,
    TodoPanel,
    ToolCallDisplay,
)
from entropi.ui.themes import DARK_THEME, LIGHT_THEME, Theme, get_theme
from entropi.ui.tui import EntropiApp, PauseScreen, ToolApprovalScreen
from entropi.ui.widgets import (
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
