"""Tests for inference engine."""

from entropic.core.base import Message, ToolCall
from entropic.inference.adapters.falcon import FalconAdapter
from entropic.inference.adapters.qwen2 import Qwen2Adapter
from entropic.inference.adapters.qwen3 import Qwen3Adapter
from entropic.inference.adapters.qwen35 import Qwen35Adapter
from entropic.prompts.manager import PromptManager


class TestQwen3Adapter:
    """Tests for Qwen3 adapter."""

    def setup_method(self) -> None:
        """Set up test fixtures."""
        pm = PromptManager(tier_identities={"normal": None}, quiet=True)
        self.adapter = Qwen3Adapter(tier="normal", prompt_manager=pm)

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


class TestFalconAdapter:
    """Tests for FalconAdapter — validates shared base class methods work via Falcon."""

    def setup_method(self) -> None:
        """Set up test fixtures."""
        self.adapter = FalconAdapter(tier="normal")

    def test_chat_format(self) -> None:
        """Test chat format is chatml."""
        assert self.adapter.chat_format == "chatml"

    def test_parse_tagged_tool_call(self) -> None:
        """Test parsing single tagged tool call."""
        content = """I'll read that file.

<tool_call>
{"name": "read_file", "arguments": {"path": "test.py"}}
</tool_call>"""

        cleaned, tool_calls = self.adapter.parse_tool_calls(content)

        assert "I'll read that file." in cleaned
        assert "<tool_call>" not in cleaned
        assert len(tool_calls) == 1
        assert tool_calls[0].name == "read_file"
        assert tool_calls[0].arguments == {"path": "test.py"}

    def test_parse_bare_json_tool_call(self) -> None:
        """Test Falcon parses bare JSON when no tags present."""
        content = '{"name": "git.status", "arguments": {}}'

        _, tool_calls = self.adapter.parse_tool_calls(content)

        assert len(tool_calls) == 1
        assert tool_calls[0].name == "git.status"

    def test_parse_malformed_json_recovery(self) -> None:
        """Test JSON recovery shared from base class."""
        content = """<tool_call>
{'name': 'read_file', 'arguments': {'path': 'test.py',}}
</tool_call>"""

        _, tool_calls = self.adapter.parse_tool_calls(content)
        assert len(tool_calls) == 1
        assert tool_calls[0].name == "read_file"

    def test_think_blocks_stripped(self) -> None:
        """Test think blocks are removed from cleaned content."""
        content = """<think>Let me think about this...</think>

Here is my response.

<tool_call>
{"name": "read_file", "arguments": {"path": "a.py"}}
</tool_call>"""

        cleaned, tool_calls = self.adapter.parse_tool_calls(content)

        assert "<think>" not in cleaned
        assert "Here is my response." in cleaned
        assert len(tool_calls) == 1

    def test_is_response_complete_with_think_only(self) -> None:
        """Test that think-only response is incomplete."""
        content = "<think>Still reasoning about this...</think>"
        assert not self.adapter.is_response_complete(content, [])

    def test_is_response_complete_with_content(self) -> None:
        """Test that response with content after think block is complete."""
        content = "<think>Reasoning...</think>\n\nHere is my answer."
        assert self.adapter.is_response_complete(content, [])

    def test_parameters_fallback(self) -> None:
        """Test that 'parameters' key works as fallback for 'arguments'."""
        content = """<tool_call>
{"name": "read_file", "parameters": {"path": "test.py"}}
</tool_call>"""

        _, tool_calls = self.adapter.parse_tool_calls(content)
        assert len(tool_calls) == 1
        assert tool_calls[0].arguments == {"path": "test.py"}


class TestQwen2Adapter:
    """Tests for Qwen2Adapter — validates unique parsing and shared base methods."""

    def setup_method(self) -> None:
        """Set up test fixtures."""
        self.adapter = Qwen2Adapter(tier="code")

    def test_chat_format(self) -> None:
        """Test chat format is chatml."""
        assert self.adapter.chat_format == "chatml"

    def test_parse_bare_json(self) -> None:
        """Test parsing bare JSON (Qwen2 primary format)."""
        content = '{"name": "filesystem.read_file", "arguments": {"path": "test.py"}}'

        cleaned, tool_calls = self.adapter.parse_tool_calls(content)

        assert len(tool_calls) == 1
        assert tool_calls[0].name == "filesystem.read_file"
        assert tool_calls[0].arguments == {"path": "test.py"}
        assert cleaned.strip() == ""

    def test_parse_markdown_block(self) -> None:
        """Test parsing tool call from markdown code block."""
        content = """Here's what I'll do:

```json
{"name": "filesystem.read_file", "arguments": {"path": "test.py"}}
```"""

        _, tool_calls = self.adapter.parse_tool_calls(content)

        assert len(tool_calls) == 1
        assert tool_calls[0].name == "filesystem.read_file"

    def test_parse_function_call_syntax(self) -> None:
        """Test parsing function-call syntax: tool_name({...})."""
        content = 'filesystem.read_file({"path": "test.py"})'

        _, tool_calls = self.adapter.parse_tool_calls(content)

        assert len(tool_calls) == 1
        assert tool_calls[0].name == "filesystem.read_file"
        assert tool_calls[0].arguments == {"path": "test.py"}

    def test_parse_python_style_kwargs(self) -> None:
        """Test parsing Python-style kwargs via base _parse_key_value_args."""
        # Need to set up tool prefixes for _is_known_tool_prefix
        self.adapter._extract_tool_prefixes([{"name": "filesystem.read_file"}])
        content = 'filesystem.read_file(path="test.py")'

        _, tool_calls = self.adapter.parse_tool_calls(content)

        assert len(tool_calls) == 1
        assert tool_calls[0].arguments == {"path": "test.py"}

    def test_parameters_fallback_bare_json(self) -> None:
        """Test 'parameters' fallback for bare JSON lines."""
        content = '{"name": "read_file", "parameters": {"path": "test.py"}}'

        _, tool_calls = self.adapter.parse_tool_calls(content)
        assert len(tool_calls) == 1
        assert tool_calls[0].arguments == {"path": "test.py"}

    def test_is_response_complete_no_think_blocks(self) -> None:
        """Test Qwen2 completion: no think block awareness."""
        # Qwen2 doesn't use think blocks, so any content = complete
        content = "Here's the answer."
        assert self.adapter.is_response_complete(content, [])

    def test_is_response_complete_with_tool_calls(self) -> None:
        """Test incomplete when tool calls present."""
        tc = ToolCall(id="1", name="test", arguments={})
        assert not self.adapter.is_response_complete("content", [tc])


class TestQwen35Adapter:
    """Tests for Qwen3.5 MoE adapter — XML tool call format."""

    def setup_method(self) -> None:
        """Set up test fixtures."""
        self.adapter = Qwen35Adapter(tier="normal")

    def test_chat_format(self) -> None:
        """Test chat format is chatml."""
        assert self.adapter.chat_format == "chatml"

    def test_parse_xml_tool_call_single(self) -> None:
        """Test parsing a single XML-style tool call."""
        content = """I'll check the weather for you.

<tool_call>
<function=get_weather>
<parameter=city>Tokyo</parameter>
</function>
</tool_call>"""

        cleaned, tool_calls = self.adapter.parse_tool_calls(content)

        assert "I'll check the weather for you." in cleaned
        assert "<tool_call>" not in cleaned
        assert "<function=" not in cleaned
        assert len(tool_calls) == 1
        assert tool_calls[0].name == "get_weather"
        assert tool_calls[0].arguments == {"city": "Tokyo"}
        assert tool_calls[0].id

    def test_parse_xml_tool_call_multiple_params(self) -> None:
        """Test parsing XML tool call with multiple parameters."""
        content = """<tool_call>
<function=filesystem.write_file>
<parameter=path>/tmp/test.py</parameter>
<parameter=content>print("hello")</parameter>
</function>
</tool_call>"""

        _, tool_calls = self.adapter.parse_tool_calls(content)

        assert len(tool_calls) == 1
        assert tool_calls[0].name == "filesystem.write_file"
        assert tool_calls[0].arguments["path"] == "/tmp/test.py"
        assert tool_calls[0].arguments["content"] == 'print("hello")'

    def test_parse_xml_tool_call_multiple_calls(self) -> None:
        """Test parsing multiple XML tool calls in one response."""
        content = """<tool_call>
<function=filesystem.read_file>
<parameter=path>a.py</parameter>
</function>
</tool_call>

<tool_call>
<function=filesystem.read_file>
<parameter=path>b.py</parameter>
</function>
</tool_call>"""

        _, tool_calls = self.adapter.parse_tool_calls(content)

        assert len(tool_calls) == 2
        assert tool_calls[0].arguments["path"] == "a.py"
        assert tool_calls[1].arguments["path"] == "b.py"

    def test_parse_xml_tool_call_numeric_value(self) -> None:
        """Test that numeric parameter values are converted to numbers."""
        content = """<tool_call>
<function=set_timeout>
<parameter=seconds>30</parameter>
<parameter=retry>true</parameter>
</function>
</tool_call>"""

        _, tool_calls = self.adapter.parse_tool_calls(content)

        assert len(tool_calls) == 1
        assert tool_calls[0].arguments["seconds"] == 30
        assert tool_calls[0].arguments["retry"] is True

    def test_parse_xml_tool_call_multiline_value(self) -> None:
        """Test parsing parameter with multiline value."""
        content = """<tool_call>
<function=filesystem.write_file>
<parameter=path>test.py</parameter>
<parameter=content>def hello():
    print("world")
    return True</parameter>
</function>
</tool_call>"""

        _, tool_calls = self.adapter.parse_tool_calls(content)

        assert len(tool_calls) == 1
        assert "def hello():" in tool_calls[0].arguments["content"]
        assert 'print("world")' in tool_calls[0].arguments["content"]

    def test_parse_no_tool_calls(self) -> None:
        """Test parsing content with no tool calls."""
        content = "Just a regular response."

        cleaned, tool_calls = self.adapter.parse_tool_calls(content)

        assert cleaned == content
        assert len(tool_calls) == 0

    def test_parse_xml_tool_call_no_params(self) -> None:
        """Test parsing XML tool call with no parameters."""
        content = """<tool_call>
<function=git.status>
</function>
</tool_call>"""

        _, tool_calls = self.adapter.parse_tool_calls(content)

        assert len(tool_calls) == 1
        assert tool_calls[0].name == "git.status"
        assert tool_calls[0].arguments == {}

    def test_parse_xml_json_array_parameter(self) -> None:
        """Test XML parameter containing a JSON array is parsed as list, not string."""
        content = """<tool_call>
<function=entropic.todo_write>
<parameter=action>bulk_update</parameter>
<parameter=updates>[{"index": 0, "status": "completed"}, {"index": 1, "status": "in_progress"}]</parameter>
</function>
</tool_call>"""

        _, tool_calls = self.adapter.parse_tool_calls(content)

        assert len(tool_calls) == 1
        assert tool_calls[0].name == "entropic.todo_write"
        updates = tool_calls[0].arguments["updates"]
        assert isinstance(updates, list), f"Expected list, got {type(updates).__name__}: {updates}"
        assert len(updates) == 2
        assert updates[0]["index"] == 0

    def test_parse_xml_json_object_parameter(self) -> None:
        """Test XML parameter containing a JSON object is parsed as dict, not string."""
        content = """<tool_call>
<function=filesystem.write_file>
<parameter=path>config.json</parameter>
<parameter=content>{"key": "value", "nested": {"a": 1}}</parameter>
</function>
</tool_call>"""

        _, tool_calls = self.adapter.parse_tool_calls(content)

        assert len(tool_calls) == 1
        # Content that looks like JSON but is meant as a string value:
        # The adapter should parse it as a dict, which is actually correct
        # for structured parameters. File content strings won't start with {.
        content_val = tool_calls[0].arguments["content"]
        assert isinstance(content_val, dict)

    def test_format_tools_uses_tools_tags(self) -> None:
        """Test that tool definitions are wrapped in <tools> tags."""
        tools = [
            {
                "name": "filesystem.read_file",
                "description": "Read a file",
                "inputSchema": {
                    "type": "object",
                    "properties": {"path": {"type": "string"}},
                },
            }
        ]
        result = self.adapter.format_system_prompt("Base prompt.", tools)

        assert "<tools>" in result
        assert "</tools>" in result
        assert "filesystem.read_file" in result
        assert "<function=" in result
        assert "<parameter=" in result

    def test_format_tool_result_uses_tool_response(self) -> None:
        """Test that tool results use <tool_response> tags."""
        tool_call = ToolCall(id="123", name="read_file", arguments={"path": "test.py"})
        message = self.adapter.format_tool_result(tool_call, "file contents")

        assert message.role == "user"
        assert "<tool_response>" in message.content
        assert "</tool_response>" in message.content
        assert "file contents" in message.content

    def test_no_nothink_suffix(self) -> None:
        """Test that Qwen3.5 does NOT append /no-think (unlike Qwen3)."""
        result = self.adapter.format_system_prompt("Base prompt.", enable_thinking=False)
        assert "/no-think" not in result

    def test_think_blocks_stripped(self) -> None:
        """Test think blocks are removed from cleaned content."""
        content = """<think>Let me analyze this...</think>

Here is the answer.

<tool_call>
<function=read_file>
<parameter=path>a.py</parameter>
</function>
</tool_call>"""

        cleaned, tool_calls = self.adapter.parse_tool_calls(content)

        assert "<think>" not in cleaned
        assert "Here is the answer." in cleaned
        assert len(tool_calls) == 1

    def test_is_response_complete_with_think_only(self) -> None:
        """Test that think-only response is incomplete."""
        content = "<think>Still reasoning...</think>"
        assert not self.adapter.is_response_complete(content, [])

    def test_is_response_complete_with_content(self) -> None:
        """Test that response with content after think block is complete."""
        content = "<think>Reasoning...</think>\n\nHere is my answer."
        assert self.adapter.is_response_complete(content, [])

    def test_fallback_to_json_tool_calls(self) -> None:
        """Test fallback to JSON format inside <tool_call> tags."""
        content = """<tool_call>
{"name": "read_file", "arguments": {"path": "test.py"}}
</tool_call>"""

        _, tool_calls = self.adapter.parse_tool_calls(content)

        assert len(tool_calls) == 1
        assert tool_calls[0].name == "read_file"


class TestBaseClassSharedMethods:
    """Test shared ChatAdapter methods through concrete adapters."""

    def test_convert_tools_to_openai_format(self) -> None:
        """Test OpenAI format conversion shared across adapters."""
        adapter = Qwen3Adapter(tier="normal")
        tools = [
            {
                "name": "filesystem.read_file",
                "description": "Read a file",
                "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}}},
            }
        ]
        result = adapter.convert_tools_to_openai_format(tools)

        assert len(result) == 1
        assert result[0]["type"] == "function"
        assert result[0]["function"]["name"] == "filesystem.read_file"
        assert result[0]["function"]["description"] == "Read a file"
        assert "path" in result[0]["function"]["parameters"]["properties"]

    def test_try_numeric_conversion(self) -> None:
        """Test numeric conversion shared across adapters."""
        adapter = Qwen3Adapter(tier="normal")
        assert adapter._try_numeric_conversion("42") == 42
        assert adapter._try_numeric_conversion("3.14") == 3.14
        assert adapter._try_numeric_conversion("hello") == "hello"

    def test_parse_key_value_args(self) -> None:
        """Test key-value parsing (unified from _parse_shell_args/_parse_python_kwargs)."""
        adapter = Qwen3Adapter(tier="normal")

        result = adapter._parse_key_value_args('path="test.py" count=5')
        assert result == {"path": "test.py", "count": 5}

        result = adapter._parse_key_value_args("flag=true value=none")
        assert result == {"flag": True, "value": None}

    def test_extract_thinking(self) -> None:
        """Test think block extraction."""
        adapter = FalconAdapter(tier="normal")
        result = adapter._extract_thinking(
            "<think>First thought</think> text <think>Second thought</think>"
        )
        assert result is not None
        assert "First thought" in result
        assert "Second thought" in result

    def test_strip_think_blocks(self) -> None:
        """Test think block stripping."""
        adapter = FalconAdapter(tier="normal")
        result = adapter._strip_think_blocks("<think>hidden</think> visible content")
        assert "hidden" not in result
        assert "visible content" in result

    def test_logger_uses_class_name(self) -> None:
        """Test each adapter gets its own logger name."""
        qwen3 = Qwen3Adapter(tier="normal")
        qwen35 = Qwen35Adapter(tier="normal")
        falcon = FalconAdapter(tier="normal")
        qwen2 = Qwen2Adapter(tier="code")

        assert "Qwen3Adapter" in qwen3._logger.name
        assert "Qwen35Adapter" in qwen35._logger.name
        assert "FalconAdapter" in falcon._logger.name
        assert "Qwen2Adapter" in qwen2._logger.name


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
