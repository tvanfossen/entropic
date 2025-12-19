"""
Base adapter interface for model-specific formatting.

Different models use different chat formats and tool call conventions.
Adapters handle these differences.
"""

from abc import ABC, abstractmethod
from typing import Any

from entropi.core.base import Message, ToolCall


class ChatAdapter(ABC):
    """Abstract base class for chat format adapters."""

    @property
    @abstractmethod
    def chat_format(self) -> str:
        """Get llama-cpp chat format name."""
        pass

    @abstractmethod
    def format_system_prompt(
        self,
        base_prompt: str,
        tools: list[dict[str, Any]] | None = None,
    ) -> str:
        """
        Format system prompt with optional tool definitions.

        Args:
            base_prompt: Base system prompt
            tools: Tool definitions

        Returns:
            Formatted system prompt
        """
        pass

    @abstractmethod
    def parse_tool_calls(self, content: str) -> tuple[str, list[ToolCall]]:
        """
        Parse tool calls from model output.

        Args:
            content: Model output

        Returns:
            Tuple of (cleaned content, tool calls)
        """
        pass

    @abstractmethod
    def format_tool_result(self, tool_call: ToolCall, result: str) -> Message:
        """
        Format tool result for injection into conversation.

        Args:
            tool_call: Original tool call
            result: Tool execution result

        Returns:
            Message to inject
        """
        pass
