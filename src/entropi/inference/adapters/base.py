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
from entropi.core.logging import get_logger
from entropi.prompts import get_identity_prompt

# Shared continuation text for tool results — used by all adapters
TOOL_RESULT_SUFFIX = "Continue. Batch multiple tool calls in one response when possible."


class ChatAdapter(ABC):
    """Abstract base class for chat format adapters.

    Provides concrete implementations for shared tool-call parsing,
    think-block handling, JSON recovery, and logging. Subclasses
    override ``parse_tool_calls`` and any methods that need
    model-specific behavior.
    """

    # Common markers — override per-adapter if the model uses different tags
    TOOL_CALL_START = "<tool_call>"
    TOOL_CALL_END = "</tool_call>"
    THINK_START = "<think>"
    THINK_END = "</think>"

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
        self._logger = get_logger(f"adapter.{type(self).__name__}")

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

    # --- Shared tool-call parsing primitives ---

    def convert_tools_to_openai_format(
        self, mcp_tools: list[dict[str, Any]]
    ) -> list[dict[str, Any]]:
        """Convert MCP tool definitions to OpenAI function calling format."""
        return [
            {
                "type": "function",
                "function": {
                    "name": tool.get("name", "unknown"),
                    "description": tool.get("description", ""),
                    "parameters": tool.get("inputSchema", {"type": "object", "properties": {}}),
                },
            }
            for tool in mcp_tools
        ]

    def _parse_tagged_tool_calls(self, content: str) -> list[ToolCall]:
        """Parse tool calls from <tool_call> tags."""
        tool_calls = []
        pattern = re.compile(
            rf"{re.escape(self.TOOL_CALL_START)}\s*(.*?)\s*{re.escape(self.TOOL_CALL_END)}",
            re.DOTALL,
        )
        for match in pattern.findall(content):
            tool_call = self._parse_single_tool_call(match.strip())
            if tool_call:
                tool_calls.append(tool_call)
                self._logger.info(f"Parsed {type(self).__name__} tool call: {tool_call.name}")
        return tool_calls

    def _parse_single_tool_call(self, json_str: str) -> ToolCall | None:
        """Parse a single tool call JSON string."""
        try:
            data = json.loads(json_str)
            if "name" in data:
                arguments = data.get("arguments", data.get("parameters", {}))
                return ToolCall(id=str(uuid.uuid4()), name=data["name"], arguments=arguments)
        except json.JSONDecodeError:
            recovered = self._try_recover_json(json_str)
            if not recovered:
                self._logger.warning(f"Failed to parse tool call: {json_str}")
            return recovered
        return None

    def _parse_bare_json_tool_calls(self, content: str) -> list[ToolCall]:
        """Parse bare JSON tool calls from lines."""
        tool_calls = []
        for line in content.split("\n"):
            stripped = line.strip()
            if not (stripped.startswith("{") and '"name"' in stripped):
                continue
            tool_call = self._try_parse_bare_json_line(stripped)
            if tool_call:
                tool_calls.append(tool_call)
                self._logger.info(
                    f"Parsed {type(self).__name__} bare JSON tool call: {tool_call.name}"
                )
        return tool_calls

    def _try_parse_bare_json_line(self, line: str) -> ToolCall | None:
        """Try to parse a bare JSON line as a tool call."""
        try:
            data = json.loads(line)
            if "name" in data:
                arguments = data.get("arguments", data.get("parameters", {}))
                return ToolCall(id=str(uuid.uuid4()), name=data["name"], arguments=arguments)
        except json.JSONDecodeError:
            pass
        return None

    def _try_recover_json(self, json_str: str) -> ToolCall | None:
        """Attempt to recover malformed JSON tool call."""
        fixed = json_str
        fixed = re.sub(r",\s*}", "}", fixed)
        fixed = re.sub(r",\s*]", "]", fixed)
        fixed = fixed.replace("'", '"')

        try:
            data = json.loads(fixed)
            if "name" in data:
                arguments = data.get("arguments", data.get("parameters", {}))
                return ToolCall(id=str(uuid.uuid4()), name=data["name"], arguments=arguments)
        except json.JSONDecodeError:
            pass

        # Last resort: regex extraction
        name_match = re.search(r'"name"\s*:\s*"([^"]+)"', json_str)
        if name_match:
            name = name_match.group(1)
            arguments: dict[str, Any] = {}
            args_match = re.search(r'"arguments"\s*:\s*(\{[^}]+\})', json_str)
            if args_match:
                try:
                    arguments = json.loads(args_match.group(1))
                except json.JSONDecodeError:
                    pass
            return ToolCall(id=str(uuid.uuid4()), name=name, arguments=arguments)

        return None

    # --- Content cleaning ---

    def _remove_bare_json_lines(self, content: str) -> str:
        """Remove lines that are bare JSON tool calls."""
        cleaned_lines = []
        for line in content.split("\n"):
            stripped = line.strip()
            if stripped.startswith("{") and '"name"' in stripped:
                if self._try_parse_bare_json_line(stripped):
                    continue
            cleaned_lines.append(line)
        return "\n".join(cleaned_lines).strip()

    # --- Think block handling ---

    def _extract_thinking(self, content: str) -> str | None:
        """Extract thinking content from <think> blocks."""
        pattern = re.compile(
            rf"{re.escape(self.THINK_START)}(.*?){re.escape(self.THINK_END)}",
            re.DOTALL,
        )
        matches = pattern.findall(content)
        if matches:
            return "\n".join(m.strip() for m in matches)
        return None

    def _strip_think_blocks(self, content: str) -> str:
        """Remove complete think blocks from content."""
        think_pattern = re.compile(
            rf"{re.escape(self.THINK_START)}.*?{re.escape(self.THINK_END)}",
            re.DOTALL,
        )
        return think_pattern.sub("", content).strip()

    # --- Logging helpers ---

    def _log_parse_start(self, content: str) -> None:
        """Log debug info at start of parsing."""
        self._logger.debug(f"\n=== Parsing tool calls ({type(self).__name__}) ===")
        self._logger.debug(f"Content length: {len(content)}")
        self._logger.debug(f"Content:\n{content}")

    def _log_thinking_content(self, content: str) -> None:
        """Extract and log thinking content."""
        thinking_content = self._extract_thinking(content)
        if thinking_content:
            self._logger.info(f"[THINKING] ({len(thinking_content)} chars):\n{thinking_content}")

    def _log_parse_result(self, content: str, tool_calls: list[ToolCall]) -> None:
        """Log parsing results."""
        if tool_calls:
            self._logger.info(
                f"Parsed {len(tool_calls)} tool calls: {[tc.name for tc in tool_calls]}"
            )
        else:
            self._logger.debug("No tool calls parsed")
            self._logger.debug(f"  - Contains '{{': {'{' in content}")
            has_name = '"name"' in content
            self._logger.debug(f"  - Contains '\"name\"': {has_name}")
            self._logger.debug(f"  - Contains '<tool_call>': {'<tool_call>' in content}")

    # --- Key-value argument parsing ---

    def _parse_key_value_args(self, args_str: str) -> dict[str, Any] | None:
        """Parse key=value arguments (shell-style or Python kwargs).

        Handles: key="value", key='value', key=123, key=True
        """
        arguments: dict[str, Any] = {}
        pattern = re.compile(r'(\w+)\s*=\s*(?:"([^"]*)"|\'([^\']*)\'|(\S+))')
        for match in pattern.finditer(args_str):
            key = match.group(1)
            value = match.group(2) or match.group(3) or match.group(4)
            if value is not None:
                arguments[key] = self._convert_typed_value(value)
        return arguments if arguments else None

    def _convert_typed_value(self, value: str) -> Any:
        """Convert string value to appropriate Python type."""
        lower = value.lower()
        literal_map = {"true": True, "false": False, "none": None, "null": None}
        if lower in literal_map:
            return literal_map[lower]
        return self._try_numeric_conversion(value)

    def _try_numeric_conversion(self, value: str) -> int | float | str:
        """Try to convert value to int or float, fall back to string."""
        try:
            return int(value)
        except ValueError:
            try:
                return float(value)
            except ValueError:
                return value

    # --- Response completion ---

    def is_response_complete(self, content: str, tool_calls: list[ToolCall]) -> bool:
        """Determine if this response represents task completion.

        Think-block-aware: if the response is only a think block with no
        other content or tool calls, the model is still working.

        Override in subclasses for model-specific logic.
        """
        if self._has_incomplete_indicators(content, tool_calls):
            return False

        content_without_think = self._strip_think_blocks(content)

        if self._has_unparsed_tool_calls(content_without_think):
            return False

        return bool(content_without_think)

    def _has_incomplete_indicators(self, content: str, tool_calls: list[ToolCall]) -> bool:
        """Check for indicators that response is incomplete."""
        if tool_calls:
            return True
        has_think_start = self.THINK_START in content
        has_think_end = self.THINK_END in content
        if has_think_start and not has_think_end:
            self._logger.debug("Unclosed think block detected - continuing loop")
            return True
        return False

    def _has_unparsed_tool_calls(self, content: str) -> bool:
        """Check for unparsed tool call JSON in code blocks.

        Override in subclasses to check additional patterns (shell-style, Python-style).
        """
        if "```" not in content:
            return False
        code_block_pattern = re.compile(r"```\w*\s*\n?([\s\S]*?)\n?```", re.MULTILINE)
        for block in code_block_pattern.findall(content):
            block_stripped = block.strip()
            if block_stripped.startswith("{") and '"name"' in block_stripped:
                self._logger.debug("Found unparsed JSON tool call in code block - continuing loop")
                return True
        return False


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
