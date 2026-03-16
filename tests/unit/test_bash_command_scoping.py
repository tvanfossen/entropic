"""Tests for bash_commands allowlist enforcement in ToolExecutor."""

from unittest.mock import MagicMock

from entropic.core.base import ModelTier, ToolCall
from entropic.core.engine import LoopConfig
from entropic.core.engine_types import EngineCallbacks, LoopContext
from entropic.core.tool_executor import ToolExecutor


def _make_executor(
    *,
    allowed_tools: set[str] | None = None,
    bash_commands: list[str] | None = None,
) -> ToolExecutor:
    """Create a ToolExecutor with mocked dependencies."""
    orchestrator = MagicMock()
    orchestrator.get_allowed_tools.return_value = allowed_tools
    orchestrator.get_tier_param.return_value = bash_commands
    return ToolExecutor(
        server_manager=MagicMock(),
        orchestrator=orchestrator,
        loop_config=LoopConfig(),
        callbacks=EngineCallbacks(),
    )


def _make_ctx(tier_name: str = "qa") -> LoopContext:
    """Create a LoopContext locked to a tier."""
    tier = ModelTier(name=tier_name, focus=["testing"])
    return LoopContext(locked_tier=tier)


def _bash_call(command: str) -> ToolCall:
    """Create a bash.execute ToolCall."""
    return ToolCall(id="test-1", name="bash.execute", arguments={"command": command})


class TestBashCommandScoping:
    """bash_commands allowlist blocks unauthorized commands."""

    def test_allowed_command_passes(self) -> None:
        """Command matching an allowed prefix is permitted."""
        executor = _make_executor(
            allowed_tools={"bash.execute"},
            bash_commands=["pytest", "python -m pytest"],
        )
        ctx = _make_ctx()
        result = executor._check_tier_allowed(ctx, _bash_call("pytest tests/"))
        assert result is None

    def test_allowed_command_with_args(self) -> None:
        """Prefix match works with arbitrary trailing arguments."""
        executor = _make_executor(
            allowed_tools={"bash.execute"},
            bash_commands=["pytest"],
        )
        ctx = _make_ctx()
        result = executor._check_tier_allowed(ctx, _bash_call("pytest -v --tb=short tests/unit/"))
        assert result is None

    def test_disallowed_command_rejected(self) -> None:
        """Command not matching any prefix is denied."""
        executor = _make_executor(
            allowed_tools={"bash.execute"},
            bash_commands=["pytest", "python -m pytest"],
        )
        ctx = _make_ctx()
        result = executor._check_tier_allowed(ctx, _bash_call("rm -rf /"))
        assert result is not None
        assert "not permitted" in result.content.lower()
        assert "pytest" in result.content

    def test_no_bash_commands_allows_all(self) -> None:
        """When bash_commands is None (not set), all commands are allowed."""
        executor = _make_executor(
            allowed_tools={"bash.execute"},
            bash_commands=None,
        )
        ctx = _make_ctx()
        result = executor._check_tier_allowed(ctx, _bash_call("rm -rf /"))
        assert result is None

    def test_non_bash_tool_skipped(self) -> None:
        """Non-bash tools are not checked against bash_commands."""
        executor = _make_executor(
            allowed_tools={"filesystem.read_file"},
            bash_commands=["pytest"],
        )
        ctx = _make_ctx()
        call = ToolCall(id="t1", name="filesystem.read_file", arguments={"path": "/etc/passwd"})
        result = executor._check_tier_allowed(ctx, call)
        assert result is None

    def test_whitespace_stripped(self) -> None:
        """Leading/trailing whitespace in command is stripped before matching."""
        executor = _make_executor(
            allowed_tools={"bash.execute"},
            bash_commands=["pytest"],
        )
        ctx = _make_ctx()
        result = executor._check_tier_allowed(ctx, _bash_call("  pytest tests/  "))
        assert result is None

    def test_partial_prefix_no_match(self) -> None:
        """A command that contains the prefix as substring but doesn't start with it is rejected."""
        executor = _make_executor(
            allowed_tools={"bash.execute"},
            bash_commands=["pytest"],
        )
        ctx = _make_ctx()
        result = executor._check_tier_allowed(ctx, _bash_call("echo pytest"))
        assert result is not None

    def test_no_locked_tier_skips_check(self) -> None:
        """When no tier is locked, all commands pass."""
        executor = _make_executor(bash_commands=["pytest"])
        ctx = LoopContext()  # no locked_tier
        result = executor._check_tier_allowed(ctx, _bash_call("rm -rf /"))
        assert result is None

    def test_empty_allowlist_blocks_all(self) -> None:
        """An empty bash_commands list blocks all commands."""
        executor = _make_executor(
            allowed_tools={"bash.execute"},
            bash_commands=[],
        )
        ctx = _make_ctx()
        result = executor._check_tier_allowed(ctx, _bash_call("pytest"))
        assert result is not None

    def test_multi_word_prefix_match(self) -> None:
        """Multi-word prefixes like 'python -m pytest' match correctly."""
        executor = _make_executor(
            allowed_tools={"bash.execute"},
            bash_commands=["python -m pytest"],
        )
        ctx = _make_ctx()
        result = executor._check_tier_allowed(ctx, _bash_call("python -m pytest tests/"))
        assert result is None
        # But plain 'python' should not match
        result2 = executor._check_tier_allowed(ctx, _bash_call("python script.py"))
        assert result2 is not None
