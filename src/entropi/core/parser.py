"""
Tool call parsing with retry and recovery logic.

Handles malformed JSON and provides fallback parsing strategies.
"""

import json
import re
import uuid
from collections.abc import Callable
from typing import Any

from entropi.core.base import ToolCall
from entropi.core.logging import get_logger

logger = get_logger("core.parser")


class ToolCallParser:
    """
    Parser for tool calls with retry logic.

    Attempts multiple parsing strategies to recover from malformed output.
    """

    def __init__(self, max_retries: int = 3) -> None:
        """
        Initialize parser.

        Args:
            max_retries: Maximum retry attempts for malformed JSON
        """
        self.max_retries = max_retries

    def parse(
        self,
        content: str,
        start_marker: str = "<tool_call>",
        end_marker: str = "</tool_call>",
    ) -> tuple[str, list[ToolCall]]:
        """
        Parse tool calls from content.

        Args:
            content: Raw model output
            start_marker: Tool call start marker
            end_marker: Tool call end marker

        Returns:
            Tuple of (cleaned content, tool calls)
        """
        tool_calls = []

        # Find all tool call blocks
        pattern = re.compile(
            rf"{re.escape(start_marker)}\s*(.*?)\s*{re.escape(end_marker)}",
            re.DOTALL,
        )

        matches = pattern.findall(content)

        for match in matches:
            tool_call = self._parse_single(match.strip())
            if tool_call:
                tool_calls.append(tool_call)

        # Clean content
        cleaned = pattern.sub("", content).strip()

        return cleaned, tool_calls

    def _parse_single(self, json_str: str) -> ToolCall | None:
        """
        Parse a single tool call JSON.

        Tries multiple strategies to recover from malformed JSON.

        Args:
            json_str: JSON string to parse

        Returns:
            ToolCall if successful, None otherwise
        """
        strategies: list[Callable[[str], ToolCall | None]] = [
            lambda s: self._try_parse(s),
            lambda s: self._try_parse(self._fix_common_issues(s)),
            lambda s: self._extract_with_regex(s),
        ]

        for strategy in strategies:
            result: ToolCall | None = strategy(json_str)
            if result:
                return result

        logger.warning(f"Failed to parse tool call: {json_str[:100]}...")
        return None

    def _try_parse(self, json_str: str) -> ToolCall | None:
        """Attempt to parse JSON."""
        try:
            data = json.loads(json_str)
            if "name" in data:
                return ToolCall(
                    id=str(uuid.uuid4()),
                    name=data["name"],
                    arguments=data.get("arguments", {}),
                )
        except json.JSONDecodeError:
            pass
        return None

    def _fix_common_issues(self, json_str: str) -> str:
        """Fix common JSON formatting issues."""
        fixed = json_str

        # Remove trailing commas before closing braces/brackets
        fixed = re.sub(r",\s*}", "}", fixed)
        fixed = re.sub(r",\s*]", "]", fixed)

        # Fix unquoted keys (simple cases)
        fixed = re.sub(r"(\{|,)\s*(\w+)\s*:", r'\1"\2":', fixed)

        # Fix single quotes to double quotes
        fixed = fixed.replace("'", '"')

        # Fix escaped newlines that shouldn't be escaped
        fixed = fixed.replace("\\n", "\n")

        return fixed

    def _extract_with_regex(self, json_str: str) -> ToolCall | None:
        """Extract tool call using regex as last resort."""
        # Try to find name
        name_match = re.search(r'"name"\s*:\s*"([^"]+)"', json_str)
        if not name_match:
            return None

        name = name_match.group(1)

        # Try to find arguments
        args_match = re.search(r'"arguments"\s*:\s*(\{[^}]*\})', json_str)
        arguments = {}

        if args_match:
            try:
                # Try to parse the arguments JSON
                args_str = args_match.group(1)
                # Fix common issues in args
                args_str = self._fix_common_issues(args_str)
                arguments = json.loads(args_str)
            except json.JSONDecodeError:
                # Try to extract key-value pairs manually
                arguments = self._extract_simple_args(args_match.group(1))

        return ToolCall(
            id=str(uuid.uuid4()),
            name=name,
            arguments=arguments,
        )

    def _extract_simple_args(self, args_str: str) -> dict[str, Any]:
        """Extract simple key-value arguments from malformed JSON."""
        args: dict[str, Any] = {}
        # Match "key": "value" or "key": value patterns
        pairs = re.findall(r'"(\w+)"\s*:\s*"?([^",}]+)"?', args_str)
        for key, value in pairs:
            # Try to parse value as JSON type
            try:
                args[key] = json.loads(value)
            except json.JSONDecodeError:
                args[key] = value.strip('"')
        return args
