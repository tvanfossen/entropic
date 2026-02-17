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
from entropi.prompts import get_identity_prompt

# Shared continuation text for tool results — used by all adapters
TOOL_RESULT_SUFFIX = "Continue. Batch multiple tool calls in one response when possible."


class ChatAdapter(ABC):
    """Abstract base class for chat format adapters."""

    def __init__(
        self,
        tier: str,
        prompts_dir: Path | None = None,
        use_bundled_prompts: bool = True,
    ) -> None:
        """
        Initialize adapter.

        Args:
            tier: Model tier (thinking, normal, code, simple)
            prompts_dir: Optional directory for user prompt overrides
            use_bundled_prompts: If False, skip bundled prompt fallback
        """
        self._tier = tier
        self._prompts_dir = prompts_dir
        self._use_bundled_prompts = use_bundled_prompts
        self._identity_prompt: str | None = None
        self._tool_prefixes: frozenset[str] = frozenset()

    def _get_identity_prompt(self) -> str:
        """Get the identity prompt (constitution + tier identity), loading and caching it."""
        if self._identity_prompt is None:
            self._identity_prompt = get_identity_prompt(
                self._tier, self._prompts_dir, use_bundled=self._use_bundled_prompts
            )
        return self._identity_prompt

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

    def format_system_prompt(
        self,
        base_prompt: str,
        tools: list[dict[str, Any]] | None = None,
    ) -> str:
        """Format system prompt with identity and tool definitions.

        Assembles the prompt in order:
        1. Identity (constitution + tier identity)
        2. Base prompt (todo state, project context)
        3. Tool definitions (description + JSON inputSchema, filtered per tier)

        Tool guidance is embedded in each tool's JSON description field —
        no separate per-tool guidance step needed.

        Subclasses should NOT override this — tool isolation depends on
        this assembly order. Override _format_tools() if tool formatting
        needs to differ.
        """
        identity = self._get_identity_prompt()
        prompt_parts = [identity]

        if base_prompt:
            prompt_parts.append(base_prompt)

        if not tools:
            return "\n\n".join(prompt_parts)

        self._extract_tool_prefixes(tools)
        prompt_parts.append(self._format_tools(tools))

        return "\n\n".join(prompt_parts)

    def _format_tools(self, tools: list[dict[str, Any]]) -> str:
        """Format tool definitions for the prompt.

        Renders each tool as: heading, full description, raw JSON inputSchema.
        The model sees both behavioral guidance (description) and exact
        parameter schema (JSON) — no information loss.

        Override in subclasses if the model needs a different tool format.
        """
        lines = [
            "## Tools\n",
            'Call tools with: `<tool_call>{"name": "tool.name", "arguments": {...}}</tool_call>`',
            "Batch independent calls in one response with multiple `<tool_call>` blocks.\n",
        ]
        for tool in tools:
            name = tool.get("name", "unknown")
            description = tool.get("description", "No description")
            schema = tool.get("inputSchema", {})

            lines.append(f"### {name}")
            lines.append(description)
            lines.append("\nSchema:")
            lines.append("```json")
            lines.append(json.dumps(schema, indent=2))
            lines.append("```")
            lines.append("")

        return "\n".join(lines)

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

    def format_tool_result(self, tool_call: ToolCall, result: str) -> Message:
        """Format tool result as user message.

        Default implementation used by all adapters. Override only if
        the model needs a fundamentally different format.
        """
        content = f"Tool `{tool_call.name}` returned:\n\n{result}\n\n{TOOL_RESULT_SUFFIX}"
        return Message(role="user", content=content)

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


def get_adapter(
    name: str,
    tier: str,
    prompts_dir: Path | None = None,
    use_bundled_prompts: bool = True,
) -> ChatAdapter:
    """
    Get an adapter instance by name.

    Falls back to generic adapter if not found.

    Args:
        name: Adapter name
        tier: Model tier (thinking, normal, code, simple)
        prompts_dir: Optional directory for user prompt overrides
        use_bundled_prompts: If False, skip bundled prompt fallback

    Returns:
        Adapter instance
    """
    from entropi.core.logging import get_logger

    logger = get_logger("adapters.base")
    name_lower = name.lower()
    kwargs = {"tier": tier, "prompts_dir": prompts_dir, "use_bundled_prompts": use_bundled_prompts}

    if name_lower not in _ADAPTERS:
        logger.warning(f"Unknown adapter '{name}', falling back to generic")
        return _ADAPTERS["generic"](**kwargs)

    return _ADAPTERS[name_lower](**kwargs)


def list_adapters() -> list[str]:
    """List available adapter names."""
    return list(_ADAPTERS.keys())
