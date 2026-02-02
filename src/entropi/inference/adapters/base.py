"""
Base adapter interface for model-specific formatting.

Different models use different chat formats and tool call conventions.
Adapters handle these differences.

We use content-based tool calling (not llama-cpp-python's native tool calling)
because the chatml-function-calling template drops tool message content.
See llama_cpp.py for details.

Adapters are responsible for:
1. format_system_prompt() - Inject tool definitions into the system prompt
2. parse_tool_calls() - Extract tool calls from model output content
3. format_tool_result() - Format tool results as user messages
"""

import json
import re
import uuid
from abc import ABC, abstractmethod
from pathlib import Path
from typing import Any

from entropi.core.base import Message, ToolCall
from entropi.prompts import get_identity_prompt, get_tool_usage_prompt


class ChatAdapter(ABC):
    """Abstract base class for chat format adapters."""

    def __init__(self, prompts_dir: Path | None = None) -> None:
        """
        Initialize adapter.

        Args:
            prompts_dir: Optional directory for user prompt overrides
        """
        self._prompts_dir = prompts_dir
        self._identity_prompt: str | None = None
        self._tool_usage_prompt: str | None = None
        self._tool_prefixes: frozenset[str] = frozenset()

    def _get_identity_prompt(self) -> str:
        """Get the identity prompt, loading and caching it."""
        if self._identity_prompt is None:
            self._identity_prompt = get_identity_prompt(self._prompts_dir)
        return self._identity_prompt

    def _get_tool_usage_prompt(self) -> str:
        """Get the tool usage prompt, loading and caching it."""
        if self._tool_usage_prompt is None:
            self._tool_usage_prompt = get_tool_usage_prompt(self._prompts_dir)
        return self._tool_usage_prompt

    def _extract_tool_prefixes(self, tools: list[dict[str, Any]]) -> None:
        """
        Extract and cache tool prefixes from tool definitions.

        Called by format_system_prompt to populate prefixes for parse_tool_calls.

        Args:
            tools: List of tool definitions with 'name' keys like 'filesystem.read_file'
        """
        prefixes = set()
        for tool in tools:
            name = tool.get("name", "")
            if "." in name:
                prefix = name.split(".")[0].lower()
                prefixes.add(prefix)
        self._tool_prefixes = frozenset(prefixes)

    def _is_known_tool_prefix(self, name: str) -> bool:
        """
        Check if a dotted name starts with a known tool prefix.

        Args:
            name: Dotted name like 'filesystem.read_file' or 'argparse.ArgumentParser'

        Returns:
            True if the prefix matches a known tool, False otherwise
        """
        if "." not in name:
            return False
        prefix = name.split(".")[0].lower()
        return prefix in self._tool_prefixes

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

    def is_response_complete(self, content: str, tool_calls: list[ToolCall]) -> bool:
        """
        Determine if this response represents task completion.

        Override in subclasses for model-specific logic (e.g., handling think blocks).

        Args:
            content: Model output content
            tool_calls: Parsed tool calls (may be empty)

        Returns:
            True if this is a final response, False if model is still working
        """
        # Default: if there are tool calls, not complete (need to execute them)
        # If no tool calls, this is the final response
        return len(tool_calls) == 0


class GenericAdapter(ChatAdapter):
    """
    Generic/default adapter using common conventions.

    Uses a simple JSON-based tool call format that works with most models.
    Can be used as a fallback or for models without a specific adapter.
    """

    # Generic tool call markers
    TOOL_CALL_START = "<tool_call>"
    TOOL_CALL_END = "</tool_call>"

    @property
    def chat_format(self) -> str:
        """Get llama-cpp chat format name."""
        return "chatml"  # Most common format

    def format_system_prompt(
        self,
        base_prompt: str,
        tools: list[dict[str, Any]] | None = None,
    ) -> str:
        """Format system prompt with identity and tool definitions."""
        # Start with identity (who the assistant is and core behaviors)
        identity = self._get_identity_prompt()
        prompt_parts = [identity]

        # Add user-provided base prompt if any
        if base_prompt:
            prompt_parts.append(base_prompt)

        if not tools:
            return "\n\n".join(prompt_parts)

        # Extract tool prefixes for use in parse_tool_calls
        self._extract_tool_prefixes(tools)

        tools_text = self._format_tools(tools)
        tool_usage = self._get_tool_usage_prompt()

        prompt_parts.append(tool_usage)
        # Tool call format is defined in tool_usage.md - single source of truth
        prompt_parts.append(tools_text)

        return "\n\n".join(prompt_parts)

    def _format_tools(self, tools: list[dict[str, Any]]) -> str:
        """Format tool definitions for the prompt."""
        lines = ["## Available Tools\n"]
        for tool in tools:
            name = tool.get("name", "unknown")
            description = tool.get("description", "No description")
            schema = tool.get("inputSchema", {})

            lines.append(f"### {name}")
            lines.append(f"{description}")

            if schema.get("properties"):
                lines.append("Parameters:")
                for param, details in schema["properties"].items():
                    param_type = details.get("type", "any")
                    param_desc = details.get("description", "")
                    required = param in schema.get("required", [])
                    req_marker = " (required)" if required else ""
                    lines.append(f"  - {param} ({param_type}){req_marker}: {param_desc}")

            lines.append("")

        return "\n".join(lines)

    def parse_tool_calls(self, content: str) -> tuple[str, list[ToolCall]]:
        """Parse tool calls from model output."""
        tool_calls = []

        pattern = re.compile(
            rf"{re.escape(self.TOOL_CALL_START)}\s*(.*?)\s*{re.escape(self.TOOL_CALL_END)}",
            re.DOTALL,
        )

        matches = pattern.findall(content)

        for match in matches:
            try:
                data = json.loads(match.strip())
                tool_call = ToolCall(
                    id=str(uuid.uuid4()),
                    name=data.get("name", ""),
                    arguments=data.get("arguments", {}),
                )
                tool_calls.append(tool_call)
            except json.JSONDecodeError:
                continue

        cleaned = pattern.sub("", content).strip()
        return cleaned, tool_calls

    def format_tool_result(self, tool_call: ToolCall, result: str) -> Message:
        """Format tool result as user message (tool role not rendered properly by llama-cpp)."""
        content = f"""Tool `{tool_call.name}` returned:

{result}

Continue with the task. Call more tools if needed, or respond when complete."""

        return Message(role="user", content=content)


# Adapter registry
_ADAPTERS: dict[str, type[ChatAdapter]] = {
    "generic": GenericAdapter,
}


def register_adapter(name: str, adapter_class: type[ChatAdapter]) -> None:
    """
    Register an adapter class.

    Args:
        name: Adapter name for config reference
        adapter_class: Adapter class to register
    """
    _ADAPTERS[name.lower()] = adapter_class


def get_adapter(name: str, prompts_dir: Path | None = None) -> ChatAdapter:
    """
    Get an adapter instance by name.

    Falls back to generic adapter if not found.

    Args:
        name: Adapter name
        prompts_dir: Optional directory for user prompt overrides

    Returns:
        Adapter instance
    """
    from entropi.core.logging import get_logger

    logger = get_logger("adapters.base")
    name_lower = name.lower()

    if name_lower not in _ADAPTERS:
        logger.warning(f"Unknown adapter '{name}', falling back to generic")
        return _ADAPTERS["generic"](prompts_dir=prompts_dir)

    return _ADAPTERS[name_lower](prompts_dir=prompts_dir)


def list_adapters() -> list[str]:
    """List available adapter names."""
    return list(_ADAPTERS.keys())
