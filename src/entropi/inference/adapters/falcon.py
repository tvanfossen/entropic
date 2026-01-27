"""
Falcon H1R adapter.

Falcon H1R models use:
- ChatML format (<|im_start|>, <|im_end|>)
- <tool_call>{"name": "...", "arguments": {...}}</tool_call> for tool calls
- <think>...</think> blocks for reasoning
"""

import json
import re
import uuid
from typing import Any

from entropi.core.base import Message, ToolCall
from entropi.core.logging import get_logger
from entropi.inference.adapters.base import ChatAdapter, register_adapter

logger = get_logger("adapter.falcon")


class FalconAdapter(ChatAdapter):
    """Adapter for Falcon H1R models."""

    # Falcon H1R markers (same format as Qwen3)
    TOOL_CALL_START = "<tool_call>"
    TOOL_CALL_END = "</tool_call>"
    THINK_START = "<think>"
    THINK_END = "</think>"

    @property
    def chat_format(self) -> str:
        """Get llama-cpp chat format name."""
        return "chatml"

    def format_system_prompt(
        self,
        base_prompt: str,
        tools: list[dict[str, Any]] | None = None,
    ) -> str:
        """Format system prompt with identity and tool definitions for Falcon."""
        # Start with identity
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
        """
        Parse tool calls from Falcon output.

        Handles:
        - <tool_call>{"name": "...", "arguments": {...}}</tool_call>
        - <think>...</think> blocks (extracted for logging)

        Args:
            content: Model output

        Returns:
            Tuple of (cleaned content, tool calls)
        """
        logger.debug("\n=== Parsing tool calls (Falcon) ===")
        logger.debug(f"Content length: {len(content)}")
        logger.debug(f"Content:\n{content}")

        tool_calls = []

        # Extract thinking blocks for logging
        thinking_content = self._extract_thinking(content)
        if thinking_content:
            logger.debug(f"Model thinking:\n{thinking_content}")

        # Pattern 1: <tool_call> tags
        tool_call_pattern = re.compile(
            rf"{re.escape(self.TOOL_CALL_START)}\s*(.*?)\s*{re.escape(self.TOOL_CALL_END)}",
            re.DOTALL,
        )
        matches = tool_call_pattern.findall(content)

        for match in matches:
            try:
                data = json.loads(match.strip())
                tool_call = ToolCall(
                    id=str(uuid.uuid4()),
                    name=data.get("name", ""),
                    arguments=data.get("arguments", {}),
                )
                tool_calls.append(tool_call)
                logger.info(f"Parsed Falcon tool call: {tool_call.name}")
            except json.JSONDecodeError:
                recovered = self._try_recover_json(match.strip())
                if recovered:
                    tool_calls.append(recovered)
                else:
                    logger.warning(f"Failed to parse tool call: {match}")

        # Fallback: bare JSON on its own line
        if not tool_calls:
            for line in content.split("\n"):
                stripped = line.strip()
                if stripped.startswith("{") and '"name"' in stripped:
                    try:
                        data = json.loads(stripped)
                        if "name" in data:
                            tool_call = ToolCall(
                                id=str(uuid.uuid4()),
                                name=data["name"],
                                arguments=data.get("arguments", {}),
                            )
                            tool_calls.append(tool_call)
                            logger.info(f"Parsed Falcon bare JSON tool call: {tool_call.name}")
                    except json.JSONDecodeError:
                        pass

        # Clean content - remove tool call blocks
        cleaned = tool_call_pattern.sub("", content).strip()

        # Remove thinking blocks from displayed output
        think_pattern = re.compile(
            rf"{re.escape(self.THINK_START)}.*?{re.escape(self.THINK_END)}",
            re.DOTALL,
        )
        cleaned = think_pattern.sub("", cleaned).strip()

        # Clean bare JSON lines if we parsed them
        if tool_calls:
            cleaned_lines = []
            for line in cleaned.split("\n"):
                stripped = line.strip()
                if stripped.startswith("{") and '"name"' in stripped:
                    try:
                        data = json.loads(stripped)
                        if "name" in data:
                            continue  # Skip this line
                    except json.JSONDecodeError:
                        pass
                cleaned_lines.append(line)
            cleaned = "\n".join(cleaned_lines).strip()

        if tool_calls:
            logger.info(f"Parsed {len(tool_calls)} tool calls: {[tc.name for tc in tool_calls]}")
        else:
            logger.debug("No tool calls parsed")
            logger.debug(f"  - Contains '{{': {'{' in content}")
            logger.debug(f"  - Contains '\"name\"': {'\"name\"' in content}")
            logger.debug(f"  - Contains '<tool_call>': {'<tool_call>' in content}")

        return cleaned, tool_calls

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

    def _try_recover_json(self, json_str: str) -> ToolCall | None:
        """Attempt to recover malformed JSON."""
        fixed = json_str
        fixed = re.sub(r",\s*}", "}", fixed)
        fixed = re.sub(r",\s*]", "]", fixed)
        fixed = fixed.replace("'", '"')

        try:
            data = json.loads(fixed)
            if "name" in data:
                return ToolCall(
                    id=str(uuid.uuid4()),
                    name=data["name"],
                    arguments=data.get("arguments", {}),
                )
        except json.JSONDecodeError:
            pass

        # Last resort: regex extraction
        name_match = re.search(r'"name"\s*:\s*"([^"]+)"', json_str)
        if name_match:
            name = name_match.group(1)
            arguments = {}

            args_match = re.search(r'"arguments"\s*:\s*(\{[^}]+\})', json_str)
            if args_match:
                try:
                    arguments = json.loads(args_match.group(1))
                except json.JSONDecodeError:
                    pass

            return ToolCall(
                id=str(uuid.uuid4()),
                name=name,
                arguments=arguments,
            )

        return None

    def format_tool_result(self, tool_call: ToolCall, result: str) -> Message:
        """Format tool result as a user message."""
        content = f"""Tool `{tool_call.name}` returned:

{result}

Use this information to respond to the user."""

        return Message(role="user", content=content)

    def is_response_complete(self, content: str, tool_calls: list[ToolCall]) -> bool:
        """
        Determine if this response represents task completion for Falcon.

        Falcon uses <think> blocks for reasoning. If the response is ONLY
        a think block with no other content or tool calls, the model is
        still working and we should continue the loop.

        Args:
            content: Model output content (before cleaning)
            tool_calls: Parsed tool calls

        Returns:
            True if this is a final response, False if model is still working
        """
        # If there are tool calls, not complete - need to execute them
        if tool_calls:
            return False

        # Check for unclosed think block (model was cut off mid-generation)
        has_think_start = self.THINK_START in content
        has_think_end = self.THINK_END in content
        if has_think_start and not has_think_end:
            logger.debug("Unclosed think block detected - continuing loop")
            return False

        # Remove complete think blocks and see if there's any content left
        think_pattern = re.compile(
            rf"{re.escape(self.THINK_START)}.*?{re.escape(self.THINK_END)}",
            re.DOTALL,
        )
        content_without_think = think_pattern.sub("", content).strip()

        # Check for unparsed tool call indicators in code blocks
        if "```" in content_without_think:
            code_block_pattern = re.compile(r"```\w*\s*\n?([\s\S]*?)\n?```", re.MULTILINE)
            for block in code_block_pattern.findall(content_without_think):
                block_stripped = block.strip()

                # Check for JSON-style tool call
                if block_stripped.startswith("{") and '"name"' in block_stripped:
                    logger.debug("Found unparsed JSON tool call in code block - continuing loop")
                    return False

        # If there's content outside of think blocks, that's the response
        if content_without_think:
            return True

        # Only think block(s) with no other content - model still working
        logger.debug("Response is only think block(s) - continuing loop")
        return False


# Register adapter
register_adapter("falcon", FalconAdapter)
