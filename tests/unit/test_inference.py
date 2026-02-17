"""Tests for inference engine."""

from entropi.core.base import Message, ToolCall
from entropi.inference.adapters.qwen3 import Qwen3Adapter


class TestQwen3Adapter:
    """Tests for Qwen3 adapter."""

    def setup_method(self) -> None:
        """Set up test fixtures."""
        self.adapter = Qwen3Adapter(tier="normal")

    def test_chat_format(self) -> None:
        """Test chat format is chatml."""
        assert self.adapter.chat_format == "chatml"

    def test_format_system_prompt_no_tools(self) -> None:
        """Test formatting system prompt without tools."""
        result = self.adapter.format_system_prompt("You are helpful.")
        # Qwen3Adapter prepends identity prompt (constitution + tier identity)
        assert "You are helpful." in result
        assert "Constitution" in result  # Constitution should be included

    def test_format_system_prompt_with_tools(self) -> None:
        """Test formatting system prompt with tools."""
        tools = [
            {
                "name": "read_file",
                "description": "Read a file",
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "path": {"type": "string", "description": "File path"},
                    },
                    "required": ["path"],
                },
            }
        ]
        result = self.adapter.format_system_prompt("You are helpful.", tools)

        assert "read_file" in result
        assert "Read a file" in result
        assert "path" in result
        assert "<tool_call>" in result
        assert '"required"' in result
        assert "Batch independent calls" in result

    def test_format_system_prompt_multiple_tools(self) -> None:
        """Test formatting with multiple tools."""
        tools = [
            {
                "name": "read_file",
                "description": "Read a file",
                "inputSchema": {"type": "object", "properties": {}},
            },
            {
                "name": "write_file",
                "description": "Write a file",
                "inputSchema": {"type": "object", "properties": {}},
            },
        ]
        result = self.adapter.format_system_prompt("Base prompt.", tools)

        assert "read_file" in result
        assert "write_file" in result

    def test_parse_tool_calls_single(self) -> None:
        """Test parsing single tool call."""
        content = """I'll read that file for you.

<tool_call>
{"name": "read_file", "arguments": {"path": "test.py"}}
</tool_call>"""

        cleaned, tool_calls = self.adapter.parse_tool_calls(content)

        assert "I'll read that file for you." in cleaned
        assert "<tool_call>" not in cleaned
        assert len(tool_calls) == 1
        assert tool_calls[0].name == "read_file"
        assert tool_calls[0].arguments == {"path": "test.py"}
        assert tool_calls[0].id  # Should have generated an ID

    def test_parse_tool_calls_multiple(self) -> None:
        """Test parsing multiple tool calls."""
        content = """<tool_call>
{"name": "read_file", "arguments": {"path": "a.py"}}
</tool_call>

<tool_call>
{"name": "read_file", "arguments": {"path": "b.py"}}
</tool_call>"""

        _, tool_calls = self.adapter.parse_tool_calls(content)
        assert len(tool_calls) == 2
        assert tool_calls[0].arguments["path"] == "a.py"
        assert tool_calls[1].arguments["path"] == "b.py"

    def test_parse_tool_calls_with_text_before_and_after(self) -> None:
        """Test parsing with surrounding text."""
        content = """Let me check that file.

<tool_call>
{"name": "read_file", "arguments": {"path": "test.py"}}
</tool_call>

I'll analyze the contents."""

        cleaned, tool_calls = self.adapter.parse_tool_calls(content)

        assert "Let me check that file." in cleaned
        assert "I'll analyze the contents." in cleaned
        assert len(tool_calls) == 1

    def test_parse_tool_calls_malformed_trailing_comma(self) -> None:
        """Test parsing JSON with trailing comma."""
        content = """<tool_call>
{"name": "read_file", "arguments": {"path": "test.py",}}
</tool_call>"""

        _, tool_calls = self.adapter.parse_tool_calls(content)
        assert len(tool_calls) == 1
        assert tool_calls[0].name == "read_file"

    def test_parse_tool_calls_malformed_single_quotes(self) -> None:
        """Test parsing JSON with single quotes."""
        content = """<tool_call>
{'name': 'read_file', 'arguments': {'path': 'test.py'}}
</tool_call>"""

        _, tool_calls = self.adapter.parse_tool_calls(content)
        assert len(tool_calls) == 1
        assert tool_calls[0].name == "read_file"

    def test_parse_tool_calls_completely_broken(self) -> None:
        """Test parsing completely broken JSON falls back to regex."""
        content = """<tool_call>
completely broken but has "name": "test_tool" somewhere "arguments": {"key": "value"}
</tool_call>"""

        _, tool_calls = self.adapter.parse_tool_calls(content)
        # Should extract at least the name via regex fallback
        assert len(tool_calls) == 1
        assert tool_calls[0].name == "test_tool"

    def test_parse_tool_calls_no_tool_calls(self) -> None:
        """Test parsing content with no tool calls."""
        content = "Just a regular response without any tool calls."

        cleaned, tool_calls = self.adapter.parse_tool_calls(content)

        assert cleaned == content
        assert len(tool_calls) == 0

    def test_parse_tool_calls_empty_arguments(self) -> None:
        """Test parsing tool call with empty arguments."""
        content = """<tool_call>
{"name": "status", "arguments": {}}
</tool_call>"""

        _, tool_calls = self.adapter.parse_tool_calls(content)
        assert len(tool_calls) == 1
        assert tool_calls[0].name == "status"
        assert tool_calls[0].arguments == {}

    def test_parse_tool_calls_nested_arguments(self) -> None:
        """Test parsing tool call with nested arguments."""
        content = """<tool_call>
{"name": "complex_tool", "arguments": {"config": {"nested": "value"}, "items": [1, 2, 3]}}
</tool_call>"""

        _, tool_calls = self.adapter.parse_tool_calls(content)
        assert len(tool_calls) == 1
        assert tool_calls[0].arguments["config"] == {"nested": "value"}
        assert tool_calls[0].arguments["items"] == [1, 2, 3]

    def test_format_tool_result(self) -> None:
        """Test formatting tool result."""
        tool_call = ToolCall(
            id="123",
            name="read_file",
            arguments={"path": "test.py"},
        )

        message = self.adapter.format_tool_result(tool_call, "file contents here")

        # Qwen3Adapter uses role="user" because llama-cpp doesn't render role="tool"
        assert message.role == "user"
        assert "read_file" in message.content
        assert "file contents here" in message.content
        assert "Batch multiple tool calls" in message.content

    def test_format_tool_result_multiline(self) -> None:
        """Test formatting tool result with multiline content."""
        tool_call = ToolCall(
            id="456",
            name="read_file",
            arguments={"path": "test.py"},
        )

        multiline_result = """def hello():
    print("Hello, world!")
    return True"""

        message = self.adapter.format_tool_result(tool_call, multiline_result)

        assert "def hello():" in message.content
        assert 'print("Hello, world!")' in message.content


class TestToolCall:
    """Tests for ToolCall dataclass."""

    def test_tool_call_creation(self) -> None:
        """Test creating a tool call."""
        tc = ToolCall(
            id="test-id",
            name="test_tool",
            arguments={"arg1": "value1"},
        )
        assert tc.id == "test-id"
        assert tc.name == "test_tool"
        assert tc.arguments == {"arg1": "value1"}

    def test_tool_call_empty_arguments(self) -> None:
        """Test tool call with empty arguments."""
        tc = ToolCall(id="id", name="tool", arguments={})
        assert tc.arguments == {}


class TestMessage:
    """Tests for Message dataclass."""

    def test_message_creation(self) -> None:
        """Test creating a message."""
        msg = Message(role="user", content="Hello")
        assert msg.role == "user"
        assert msg.content == "Hello"
        assert msg.tool_calls == []
        assert msg.tool_results == []
        assert msg.metadata == {}

    def test_message_with_tool_calls(self) -> None:
        """Test message with tool calls."""
        msg = Message(
            role="assistant",
            content="Let me help",
            tool_calls=[{"id": "1", "name": "test"}],
        )
        assert len(msg.tool_calls) == 1
        assert msg.tool_calls[0]["id"] == "1"

    def test_message_with_metadata(self) -> None:
        """Test message with metadata."""
        msg = Message(
            role="user",
            content="Hello",
            metadata={"timestamp": 12345},
        )
        assert msg.metadata["timestamp"] == 12345
