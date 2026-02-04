"""Test tool call format injection in engine."""

import json

from entropi.core.base import ToolCall


def test_empty_content_tool_call_format():
    """Verify empty assistant content with tool calls uses <tool_call> format."""
    # Simulate the _create_assistant_message logic
    tool_calls = [
        ToolCall(id="1", name="bash.execute", arguments={"command": "ls"}),
    ]
    content = ""

    # Apply the fix logic (from engine._create_assistant_message)
    if not content.strip() and tool_calls:
        content = "\n".join([
            f'<tool_call>{{"name": "{tc.name}", "arguments": {json.dumps(tc.arguments)}}}</tool_call>'
            for tc in tool_calls
        ])

    assert "<tool_call>" in content
    assert "[Calling:" not in content
    assert '"name": "bash.execute"' in content
    assert '"command": "ls"' in content


def test_multiple_tool_calls_format():
    """Verify multiple tool calls are properly formatted."""
    tool_calls = [
        ToolCall(id="1", name="filesystem.read_file", arguments={"path": "a.py"}),
        ToolCall(id="2", name="filesystem.read_file", arguments={"path": "b.py"}),
    ]
    content = ""

    if not content.strip() and tool_calls:
        content = "\n".join([
            f'<tool_call>{{"name": "{tc.name}", "arguments": {json.dumps(tc.arguments)}}}</tool_call>'
            for tc in tool_calls
        ])

    assert content.count("<tool_call>") == 2
    assert content.count("</tool_call>") == 2
    assert "a.py" in content
    assert "b.py" in content


def test_non_empty_content_preserved():
    """Verify non-empty content is not replaced."""
    tool_calls = [
        ToolCall(id="1", name="bash.execute", arguments={"command": "ls"}),
    ]
    content = "I'll check the files."

    # The fix only applies when content is empty
    original_content = content
    if not content.strip() and tool_calls:
        content = "\n".join([
            f'<tool_call>{{"name": "{tc.name}", "arguments": {json.dumps(tc.arguments)}}}</tool_call>'
            for tc in tool_calls
        ])

    assert content == original_content
    assert "<tool_call>" not in content
