"""Tests for command system."""

from pathlib import Path

import pytest
from entropi.core.base import Message
from entropi.core.commands import CommandContext, CommandRegistry
from entropi.core.context import ContextCompactor, ProjectContext


class TestCommandRegistry:
    """Tests for command registry."""

    @pytest.fixture
    def registry(self, tmp_path: Path) -> CommandRegistry:
        """Create registry with temp directory."""
        return CommandRegistry(tmp_path, tmp_path / "global")

    @pytest.mark.asyncio
    async def test_discover_builtins(self, registry: CommandRegistry) -> None:
        """Test built-in command discovery."""
        await registry.discover()
        assert "help" in registry._commands
        assert "clear" in registry._commands
        assert "compact" in registry._commands
        assert "status" in registry._commands
        assert "model" in registry._commands
        assert "save" in registry._commands
        assert "load" in registry._commands
        assert "config" in registry._commands

    @pytest.mark.asyncio
    async def test_execute_help(self, registry: CommandRegistry) -> None:
        """Test help command execution."""
        await registry.discover()
        context = CommandContext(
            app=None,
            conversation_id=None,
            project_dir=Path("."),
            config=None,
        )

        result = await registry.execute("/help", context)
        assert result.success
        assert "Available Commands" in result.message

    @pytest.mark.asyncio
    async def test_execute_help_specific_command(self, registry: CommandRegistry) -> None:
        """Test help for specific command."""
        await registry.discover()
        context = CommandContext(
            app=None,
            conversation_id=None,
            project_dir=Path("."),
            config=None,
        )

        result = await registry.execute("/help model", context)
        assert result.success
        assert "/model" in result.message

    @pytest.mark.asyncio
    async def test_unknown_command(self, registry: CommandRegistry) -> None:
        """Test unknown command handling."""
        await registry.discover()
        context = CommandContext(
            app=None,
            conversation_id=None,
            project_dir=Path("."),
            config=None,
        )

        result = await registry.execute("/unknown", context)
        assert not result.success
        assert "Unknown command" in result.message

    def test_is_command(self, registry: CommandRegistry) -> None:
        """Test command detection."""
        assert registry.is_command("/help")
        assert registry.is_command("/clear")
        assert registry.is_command("  /help")
        assert not registry.is_command("help")
        assert not registry.is_command("hello /help")

    @pytest.mark.asyncio
    async def test_model_command_valid(self, registry: CommandRegistry) -> None:
        """Test model command with valid model."""
        await registry.discover()
        context = CommandContext(
            app=None,
            conversation_id=None,
            project_dir=Path("."),
            config=None,
        )

        result = await registry.execute("/model normal", context)
        assert result.success
        assert result.data["action"] == "switch_model"
        assert result.data["model"] == "normal"

    @pytest.mark.asyncio
    async def test_model_command_invalid(self, registry: CommandRegistry) -> None:
        """Test model command with invalid model."""
        await registry.discover()
        context = CommandContext(
            app=None,
            conversation_id=None,
            project_dir=Path("."),
            config=None,
        )

        result = await registry.execute("/model invalid", context)
        assert not result.success
        assert "Invalid model" in result.message

    @pytest.mark.asyncio
    async def test_clear_command(self, registry: CommandRegistry) -> None:
        """Test clear command."""
        await registry.discover()
        context = CommandContext(
            app=None,
            conversation_id=None,
            project_dir=Path("."),
            config=None,
        )

        result = await registry.execute("/clear", context)
        assert result.success
        assert result.data["action"] == "clear_history"

    @pytest.mark.asyncio
    async def test_save_command(self, registry: CommandRegistry) -> None:
        """Test save command."""
        await registry.discover()
        context = CommandContext(
            app=None,
            conversation_id=None,
            project_dir=Path("."),
            config=None,
        )

        result = await registry.execute("/save myconvo", context)
        assert result.success
        assert result.data["action"] == "save_conversation"
        assert result.data["name"] == "myconvo"

    @pytest.mark.asyncio
    async def test_load_command_no_args(self, registry: CommandRegistry) -> None:
        """Test load command without arguments."""
        await registry.discover()
        context = CommandContext(
            app=None,
            conversation_id=None,
            project_dir=Path("."),
            config=None,
        )

        result = await registry.execute("/load", context)
        assert not result.success
        assert "specify" in result.message.lower()

    @pytest.mark.asyncio
    async def test_custom_command(self, tmp_path: Path) -> None:
        """Test custom command from file."""
        commands_dir = tmp_path / ".entropi" / "commands"
        commands_dir.mkdir(parents=True)

        # Create custom command file
        (commands_dir / "review.md").write_text(
            """---
name: review
description: Review code
---

Review the following code for issues: $ARGUMENTS
"""
        )

        registry = CommandRegistry(tmp_path)
        await registry.discover()

        assert "review" in registry._commands
        assert registry._commands["review"].prompt_template is not None

    @pytest.mark.asyncio
    async def test_custom_command_execution(self, tmp_path: Path) -> None:
        """Test executing a custom command."""
        commands_dir = tmp_path / ".entropi" / "commands"
        commands_dir.mkdir(parents=True)

        (commands_dir / "test.md").write_text(
            """---
name: test
description: Test command
---

Testing: $ARGUMENTS
"""
        )

        registry = CommandRegistry(tmp_path)
        await registry.discover()

        context = CommandContext(
            app=None,
            conversation_id=None,
            project_dir=tmp_path,
            config=None,
        )

        result = await registry.execute("/test hello world", context)
        assert result.success
        assert "Testing: hello world" in result.message
        assert result.data["inject_as_user"] is True

    def test_list_commands(self, registry: CommandRegistry) -> None:
        """Test listing all commands."""
        registry._register_builtins()
        commands = registry.list_commands()
        assert len(commands) >= 8  # At least 8 built-in commands
        names = [c.name for c in commands]
        assert "help" in names
        assert "clear" in names


class TestContextCompactor:
    """Tests for context compaction."""

    def test_needs_compaction_under_threshold(self) -> None:
        """Test compaction not needed under threshold."""
        compactor = ContextCompactor(max_tokens=100, compaction_threshold=0.8)

        # Simple token count mock
        def count_fn(text: str) -> int:
            return len(text.split())

        # Under threshold
        messages = [Message(role="user", content="hello world")]
        assert not compactor.needs_compaction(messages, count_fn)

    def test_needs_compaction_over_threshold(self) -> None:
        """Test compaction needed over threshold."""
        compactor = ContextCompactor(max_tokens=100, compaction_threshold=0.8)

        def count_fn(text: str) -> int:
            return len(text.split())

        # Over threshold (80 words > 80% of 100)
        messages = [Message(role="user", content=" ".join(["word"] * 100))]
        assert compactor.needs_compaction(messages, count_fn)

    def test_prepare_for_compaction(self) -> None:
        """Test message splitting for compaction."""
        compactor = ContextCompactor()

        messages = [
            Message(role="system", content="system"),
            Message(role="user", content="msg1"),
            Message(role="assistant", content="msg2"),
            Message(role="user", content="msg3"),
            Message(role="assistant", content="msg4"),
            Message(role="user", content="msg5"),
        ]

        to_compact, to_keep = compactor.prepare_for_compaction(messages, keep_recent=2)

        assert len(to_compact) == 3  # msg1, msg2, msg3
        assert len(to_keep) == 3  # system, msg4, msg5

    def test_prepare_for_compaction_few_messages(self) -> None:
        """Test compaction with fewer messages than keep_recent."""
        compactor = ContextCompactor()

        messages = [
            Message(role="user", content="msg1"),
            Message(role="assistant", content="msg2"),
        ]

        to_compact, to_keep = compactor.prepare_for_compaction(messages, keep_recent=4)

        assert len(to_compact) == 0
        assert len(to_keep) == 2

    def test_create_compaction_message(self) -> None:
        """Test creating compaction summary message."""
        compactor = ContextCompactor()
        summary = "User discussed implementing a feature."

        message = compactor.create_compaction_message(summary)

        assert message.role == "system"
        assert summary in message.content
        assert message.metadata["is_compaction_summary"] is True

    def test_format_for_summarization(self) -> None:
        """Test formatting messages for summarization."""
        compactor = ContextCompactor()

        messages = [
            Message(role="user", content="Hello"),
            Message(role="assistant", content="Hi there!"),
        ]

        formatted = compactor.format_for_summarization(messages)

        assert "[USER]:" in formatted
        assert "[ASSISTANT]:" in formatted
        assert "Hello" in formatted
        assert "Hi there!" in formatted

    def test_format_for_summarization_truncates_long_content(self) -> None:
        """Test that long content is truncated."""
        compactor = ContextCompactor()

        long_content = "x" * 1000
        messages = [Message(role="user", content=long_content)]

        formatted = compactor.format_for_summarization(messages)

        # Content should be truncated with ellipsis
        assert "..." in formatted
        assert len(formatted) < len(long_content)


class TestProjectContext:
    """Tests for project context loader."""

    @pytest.mark.asyncio
    async def test_load_entropi_md(self, tmp_path: Path) -> None:
        """Test loading ENTROPI.md from .entropi/ directory."""
        entropi_dir = tmp_path / ".entropi"
        entropi_dir.mkdir()
        entropi_md = entropi_dir / "ENTROPI.md"
        entropi_md.write_text("# My Project\n\nThis is a test project.")

        context = ProjectContext(tmp_path)
        await context.load()

        assert context.has_context
        assert "My Project" in context.get_system_prompt_addition()

    @pytest.mark.asyncio
    async def test_no_entropi_md(self, tmp_path: Path) -> None:
        """Test when ENTROPI.md doesn't exist."""
        context = ProjectContext(tmp_path)
        await context.load()

        assert not context.has_context
        assert context.get_system_prompt_addition() == ""

    @pytest.mark.asyncio
    async def test_load_gitignore(self, tmp_path: Path) -> None:
        """Test loading .gitignore patterns."""
        gitignore = tmp_path / ".gitignore"
        gitignore.write_text("*.pyc\n__pycache__/\n# comment\n.env")

        context = ProjectContext(tmp_path)
        await context.load()

        assert "*.pyc" in context._gitignore_patterns
        assert "__pycache__/" in context._gitignore_patterns
        assert ".env" in context._gitignore_patterns
        # Comments should be excluded
        assert "# comment" not in context._gitignore_patterns

    @pytest.mark.asyncio
    async def test_system_prompt_addition_format(self, tmp_path: Path) -> None:
        """Test format of system prompt addition."""
        entropi_dir = tmp_path / ".entropi"
        entropi_dir.mkdir()
        entropi_md = entropi_dir / "ENTROPI.md"
        entropi_md.write_text("Project context here.")

        context = ProjectContext(tmp_path)
        await context.load()

        addition = context.get_system_prompt_addition()
        assert "# Project Context" in addition
        assert "ENTROPI.md" in addition
