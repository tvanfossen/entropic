"""Tests for agent engine."""

from entropi.core.context import ContextBuilder, TokenBudget
from entropi.core.engine import AgentState, LoopConfig, LoopContext, LoopMetrics
from entropi.core.parser import ToolCallParser


class TestLoopConfig:
    """Tests for loop configuration."""

    def test_defaults(self) -> None:
        """Test default values."""
        config = LoopConfig()
        assert config.max_iterations == 15
        assert config.max_consecutive_errors == 3
        assert config.stream_output is True
        assert config.auto_approve_tools is False

    def test_custom_values(self) -> None:
        """Test custom configuration."""
        config = LoopConfig(max_iterations=5, stream_output=False)
        assert config.max_iterations == 5
        assert config.stream_output is False


class TestLoopMetrics:
    """Tests for loop metrics."""

    def test_duration_calculation(self) -> None:
        """Test duration calculation."""
        metrics = LoopMetrics()
        metrics.start_time = 1000.0
        metrics.end_time = 1002.5
        assert metrics.duration_ms == 2500

    def test_default_values(self) -> None:
        """Test default metric values."""
        metrics = LoopMetrics()
        assert metrics.iterations == 0
        assert metrics.tool_calls == 0
        assert metrics.tokens_used == 0
        assert metrics.errors == 0


class TestLoopContext:
    """Tests for loop context."""

    def test_default_state(self) -> None:
        """Test default state is IDLE."""
        ctx = LoopContext()
        assert ctx.state == AgentState.IDLE

    def test_empty_messages(self) -> None:
        """Test empty message list by default."""
        ctx = LoopContext()
        assert ctx.messages == []


class TestToolCallParser:
    """Tests for tool call parser."""

    def setup_method(self) -> None:
        """Set up test fixtures."""
        self.parser = ToolCallParser()

    def test_parse_valid_json(self) -> None:
        """Test parsing valid JSON."""
        content = """Here's my plan.

<tool_call>
{"name": "read_file", "arguments": {"path": "test.py"}}
</tool_call>"""

        cleaned, calls = self.parser.parse(content)

        assert "Here's my plan." in cleaned
        assert len(calls) == 1
        assert calls[0].name == "read_file"
        assert calls[0].arguments == {"path": "test.py"}

    def test_parse_malformed_json_trailing_comma(self) -> None:
        """Test parsing malformed JSON with trailing comma."""
        content = """<tool_call>
{"name": "read_file", "arguments": {"path": "test.py",}}
</tool_call>"""

        _, calls = self.parser.parse(content)
        assert len(calls) == 1
        assert calls[0].name == "read_file"

    def test_parse_malformed_json_single_quotes(self) -> None:
        """Test parsing malformed JSON with single quotes."""
        content = """<tool_call>
{'name': 'read_file', 'arguments': {'path': 'test.py'}}
</tool_call>"""

        _, calls = self.parser.parse(content)
        assert len(calls) == 1
        assert calls[0].name == "read_file"

    def test_parse_multiple(self) -> None:
        """Test parsing multiple tool calls."""
        content = """<tool_call>
{"name": "read_file", "arguments": {"path": "a.py"}}
</tool_call>
<tool_call>
{"name": "read_file", "arguments": {"path": "b.py"}}
</tool_call>"""

        _, calls = self.parser.parse(content)
        assert len(calls) == 2

    def test_regex_fallback(self) -> None:
        """Test regex extraction as fallback."""
        content = """<tool_call>
completely broken {"name": "test_tool", something "arguments": {"key": "value"}}
</tool_call>"""

        _, calls = self.parser.parse(content)
        # Should extract at least the name
        assert len(calls) == 1
        assert calls[0].name == "test_tool"

    def test_no_tool_calls(self) -> None:
        """Test content with no tool calls."""
        content = "Just a regular response."
        cleaned, calls = self.parser.parse(content)
        assert cleaned == content
        assert len(calls) == 0

    def test_custom_markers(self) -> None:
        """Test custom start/end markers."""
        content = """[[TOOL]]
{"name": "test", "arguments": {}}
[[/TOOL]]"""

        _, calls = self.parser.parse(content, "[[TOOL]]", "[[/TOOL]]")
        assert len(calls) == 1
        assert calls[0].name == "test"


class TestTokenBudget:
    """Tests for token budget."""

    def test_from_context_length(self) -> None:
        """Test creating budget from context length."""
        budget = TokenBudget.from_context_length(16384)

        assert budget.total == 16384
        assert budget.system == 2000
        assert budget.history == 10384  # 16384 - 6000
        assert budget.tools == 1000
        assert budget.response == 4000

    def test_custom_budget(self) -> None:
        """Test creating custom budget."""
        budget = TokenBudget(
            total=8192,
            system=1000,
            history=5000,
            tools=500,
            response=2000,
        )
        assert budget.total == 8192


class TestContextBuilder:
    """Tests for context builder."""

    def test_build_system_prompt_returns_base(self) -> None:
        """Test build_system_prompt returns the base prompt."""
        from entropi.config.schema import EntropyConfig

        config = EntropyConfig()
        builder = ContextBuilder(config)

        prompt = builder.build_system_prompt("Test base prompt")
        assert "Test base prompt" in prompt

    def test_estimate_tokens(self) -> None:
        """Test token estimation."""
        from entropi.config.schema import EntropyConfig

        config = EntropyConfig()
        builder = ContextBuilder(config)

        # ~4 chars per token
        assert builder.estimate_tokens("hello world") == 2  # 11 // 4 = 2
        assert builder.estimate_tokens("a" * 100) == 25  # 100 // 4 = 25

    def test_build_system_prompt_with_project_context(self) -> None:
        """Test building system prompt appends project context."""
        from entropi.config.schema import EntropyConfig

        config = EntropyConfig()
        builder = ContextBuilder(config)
        builder._project_context = "Project-specific info here"

        prompt = builder.build_system_prompt("Base prompt")
        assert "Base prompt" in prompt
        assert "Project Context" in prompt
        assert "Project-specific info here" in prompt
