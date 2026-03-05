"""
Router adapter for classification-only models.

Minimal adapter for the router model. No thinking, no identity prompt,
no tool scaffolding. Just passes the classification prompt through
and returns the raw output.
"""

from typing import Any

from entropic.core.base import Message, ToolCall
from entropic.inference.adapters.base import ChatAdapter, register_adapter


class RouterAdapter(ChatAdapter):
    """Minimal adapter for router/classification models.

    Uses the GGUF's embedded jinja template (chat_format=None triggers
    auto-detection) with thinking disabled. The router model receives
    only the classification prompt and outputs a digit.
    """

    enable_thinking: bool = False

    @property
    def chat_format(self) -> str | None:
        """Use GGUF's embedded template (auto-detected by llama-cpp-python)."""
        return None

    def format_system_prompt(
        self,
        base_prompt: str,
        tools: list[dict[str, Any]] | None = None,
        *,
        enable_thinking: bool = True,
    ) -> str:
        """Pass through as-is. Router has no identity or tools."""
        return base_prompt

    def parse_tool_calls(self, content: str) -> tuple[str, list[ToolCall]]:
        """Router never produces tool calls."""
        return content, []

    def format_tool_result(self, tool_call: ToolCall, result: str) -> Message:
        """Router never uses tools."""
        return Message(role="user", content=result)


register_adapter("router", RouterAdapter)
