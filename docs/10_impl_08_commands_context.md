# Implementation 08: Commands & Context

> Slash commands, ENTROPI.md loading, and context management

**Prerequisites:** Implementation 07 complete
**Estimated Time:** 2-3 hours with Claude Code
**Checkpoint:** Slash commands and ENTROPI.md work

---

## Objectives

1. Implement slash command system with discovery
2. Create ENTROPI.md loader for project context
3. Build context compaction for long conversations
4. Implement custom command definition format

---

## 1. Command System

### File: `src/entropi/core/commands.py`

```python
"""
Slash command system.

Provides built-in commands and supports user-defined commands
from project and global directories.
"""
import re
from abc import ABC, abstractmethod
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Coroutine

import yaml

from entropi.core.logging import get_logger

logger = get_logger("core.commands")


@dataclass
class CommandResult:
    """Result of command execution."""

    success: bool
    message: str = ""
    should_continue: bool = True  # Whether to continue conversation
    data: Any = None


@dataclass
class CommandDefinition:
    """Definition of a command."""

    name: str
    description: str
    usage: str = ""
    arguments: list[dict[str, Any]] | None = None
    handler: Callable[..., Coroutine[Any, Any, CommandResult]] | None = None
    prompt_template: str | None = None  # For custom commands


class Command(ABC):
    """Base class for built-in commands."""

    @property
    @abstractmethod
    def name(self) -> str:
        """Command name (without slash)."""
        pass

    @property
    @abstractmethod
    def description(self) -> str:
        """Command description."""
        pass

    @property
    def usage(self) -> str:
        """Usage string."""
        return f"/{self.name}"

    @abstractmethod
    async def execute(self, args: str, context: "CommandContext") -> CommandResult:
        """
        Execute the command.

        Args:
            args: Arguments string
            context: Command execution context

        Returns:
            Command result
        """
        pass


@dataclass
class CommandContext:
    """Context passed to command handlers."""

    app: Any  # Application instance
    conversation_id: str | None
    project_dir: Path
    config: Any


class CommandRegistry:
    """
    Registry for slash commands.

    Discovers and manages both built-in and custom commands.
    """

    def __init__(
        self,
        project_dir: Path,
        global_commands_dir: Path | None = None,
    ) -> None:
        """
        Initialize command registry.

        Args:
            project_dir: Project directory
            global_commands_dir: Global commands directory
        """
        self.project_dir = project_dir
        self.global_commands_dir = global_commands_dir or Path("~/.entropi/commands").expanduser()
        self._commands: dict[str, CommandDefinition] = {}
        self._builtin_commands: dict[str, Command] = {}

    def register_builtin(self, command: Command) -> None:
        """Register a built-in command."""
        self._builtin_commands[command.name] = command
        self._commands[command.name] = CommandDefinition(
            name=command.name,
            description=command.description,
            usage=command.usage,
            handler=command.execute,
        )

    async def discover(self) -> None:
        """Discover all available commands."""
        # Register built-in commands
        self._register_builtins()

        # Discover project commands
        project_commands = self.project_dir / ".entropi" / "commands"
        if project_commands.exists():
            await self._discover_custom_commands(project_commands)

        # Discover global commands
        if self.global_commands_dir.exists():
            await self._discover_custom_commands(self.global_commands_dir)

        logger.info(f"Discovered {len(self._commands)} commands")

    def _register_builtins(self) -> None:
        """Register built-in commands."""
        self.register_builtin(HelpCommand(self))
        self.register_builtin(ClearCommand())
        self.register_builtin(CompactCommand())
        self.register_builtin(StatusCommand())
        self.register_builtin(ConfigCommand())
        self.register_builtin(ModelCommand())
        self.register_builtin(SaveCommand())
        self.register_builtin(LoadCommand())

    async def _discover_custom_commands(self, directory: Path) -> None:
        """Discover custom commands from markdown files."""
        for md_file in directory.glob("*.md"):
            try:
                definition = self._parse_command_file(md_file)
                if definition:
                    self._commands[definition.name] = definition
                    logger.debug(f"Loaded custom command: {definition.name}")
            except Exception as e:
                logger.warning(f"Failed to parse command {md_file}: {e}")

    def _parse_command_file(self, path: Path) -> CommandDefinition | None:
        """
        Parse a command definition from markdown file.

        Format:
        ---
        name: review
        description: Review code
        arguments:
          - name: file
            description: File to review
            required: true
        ---

        Prompt template content...
        """
        content = path.read_text()

        # Extract YAML frontmatter
        frontmatter_match = re.match(r"^---\n(.*?)\n---\n(.*)$", content, re.DOTALL)
        if not frontmatter_match:
            return None

        frontmatter = yaml.safe_load(frontmatter_match.group(1))
        template = frontmatter_match.group(2).strip()

        return CommandDefinition(
            name=frontmatter.get("name", path.stem),
            description=frontmatter.get("description", "Custom command"),
            usage=f"/{frontmatter.get('name', path.stem)} {frontmatter.get('usage', '')}".strip(),
            arguments=frontmatter.get("arguments"),
            prompt_template=template,
        )

    async def execute(
        self,
        command_str: str,
        context: CommandContext,
    ) -> CommandResult:
        """
        Execute a command string.

        Args:
            command_str: Full command string (e.g., "/help config")
            context: Command context

        Returns:
            Command result
        """
        # Parse command
        parts = command_str.lstrip("/").split(maxsplit=1)
        name = parts[0].lower()
        args = parts[1] if len(parts) > 1 else ""

        # Find command
        if name not in self._commands:
            return CommandResult(
                success=False,
                message=f"Unknown command: /{name}. Use /help for available commands.",
            )

        definition = self._commands[name]

        # Execute
        if definition.handler:
            # Built-in command
            return await definition.handler(args, context)
        elif definition.prompt_template:
            # Custom command - inject as user message
            return await self._execute_custom_command(definition, args, context)
        else:
            return CommandResult(success=False, message=f"Command {name} has no handler")

    async def _execute_custom_command(
        self,
        definition: CommandDefinition,
        args: str,
        context: CommandContext,
    ) -> CommandResult:
        """Execute a custom command by injecting prompt."""
        # Replace $ARGUMENTS placeholder
        prompt = definition.prompt_template.replace("$ARGUMENTS", args)

        return CommandResult(
            success=True,
            message=prompt,
            should_continue=True,
            data={"inject_as_user": True},
        )

    def list_commands(self) -> list[CommandDefinition]:
        """List all available commands."""
        return list(self._commands.values())

    def is_command(self, text: str) -> bool:
        """Check if text is a command."""
        return text.strip().startswith("/")


# Built-in command implementations

class HelpCommand(Command):
    """Display help information."""

    def __init__(self, registry: CommandRegistry) -> None:
        self._registry = registry

    @property
    def name(self) -> str:
        return "help"

    @property
    def description(self) -> str:
        return "Show available commands"

    @property
    def usage(self) -> str:
        return "/help [command]"

    async def execute(self, args: str, context: CommandContext) -> CommandResult:
        if args:
            # Help for specific command
            cmd = self._registry._commands.get(args.lower())
            if cmd:
                return CommandResult(
                    success=True,
                    message=f"**/{cmd.name}**\n{cmd.description}\n\nUsage: {cmd.usage}",
                )
            return CommandResult(success=False, message=f"Unknown command: {args}")

        # List all commands
        lines = ["**Available Commands:**\n"]
        for cmd in sorted(self._registry._commands.values(), key=lambda c: c.name):
            lines.append(f"  `/{cmd.name}` - {cmd.description}")

        return CommandResult(success=True, message="\n".join(lines))


class ClearCommand(Command):
    """Clear conversation history."""

    @property
    def name(self) -> str:
        return "clear"

    @property
    def description(self) -> str:
        return "Clear conversation history"

    async def execute(self, args: str, context: CommandContext) -> CommandResult:
        # Signal to app to clear history
        return CommandResult(
            success=True,
            message="Conversation cleared.",
            data={"action": "clear_history"},
        )


class CompactCommand(Command):
    """Compact conversation context."""

    @property
    def name(self) -> str:
        return "compact"

    @property
    def description(self) -> str:
        return "Summarize and compact conversation context"

    async def execute(self, args: str, context: CommandContext) -> CommandResult:
        return CommandResult(
            success=True,
            message="Compacting conversation...",
            data={"action": "compact"},
        )


class StatusCommand(Command):
    """Show system status."""

    @property
    def name(self) -> str:
        return "status"

    @property
    def description(self) -> str:
        return "Show model and system status"

    async def execute(self, args: str, context: CommandContext) -> CommandResult:
        return CommandResult(
            success=True,
            data={"action": "show_status"},
        )


class ConfigCommand(Command):
    """View or edit configuration."""

    @property
    def name(self) -> str:
        return "config"

    @property
    def description(self) -> str:
        return "View or edit configuration"

    @property
    def usage(self) -> str:
        return "/config [key] [value]"

    async def execute(self, args: str, context: CommandContext) -> CommandResult:
        if not args:
            return CommandResult(
                success=True,
                data={"action": "show_config"},
            )

        parts = args.split(maxsplit=1)
        key = parts[0]
        value = parts[1] if len(parts) > 1 else None

        return CommandResult(
            success=True,
            data={"action": "set_config", "key": key, "value": value},
        )


class ModelCommand(Command):
    """Switch model."""

    @property
    def name(self) -> str:
        return "model"

    @property
    def description(self) -> str:
        return "Switch active model"

    @property
    def usage(self) -> str:
        return "/model [primary|workhorse|fast|micro]"

    async def execute(self, args: str, context: CommandContext) -> CommandResult:
        if not args:
            return CommandResult(
                success=True,
                data={"action": "show_model"},
            )

        model = args.lower().strip()
        valid = {"primary", "workhorse", "fast", "micro"}
        if model not in valid:
            return CommandResult(
                success=False,
                message=f"Invalid model. Choose from: {', '.join(valid)}",
            )

        return CommandResult(
            success=True,
            message=f"Switched to {model} model",
            data={"action": "switch_model", "model": model},
        )


class SaveCommand(Command):
    """Save conversation."""

    @property
    def name(self) -> str:
        return "save"

    @property
    def description(self) -> str:
        return "Save current conversation"

    @property
    def usage(self) -> str:
        return "/save [name]"

    async def execute(self, args: str, context: CommandContext) -> CommandResult:
        return CommandResult(
            success=True,
            data={"action": "save_conversation", "name": args or None},
        )


class LoadCommand(Command):
    """Load conversation."""

    @property
    def name(self) -> str:
        return "load"

    @property
    def description(self) -> str:
        return "Load a saved conversation"

    @property
    def usage(self) -> str:
        return "/load <name>"

    async def execute(self, args: str, context: CommandContext) -> CommandResult:
        if not args:
            return CommandResult(
                success=False,
                message="Please specify a conversation name to load",
            )

        return CommandResult(
            success=True,
            data={"action": "load_conversation", "name": args},
        )
```

---

## 2. Context Loader

### File: `src/entropi/core/context.py`

```python
"""
Context management.

Handles loading ENTROPI.md, project context, and context compaction.
"""
from pathlib import Path
from typing import Any

from entropi.core.base import Message
from entropi.core.logging import get_logger

logger = get_logger("core.context")


class ProjectContext:
    """
    Manages project context from ENTROPI.md and related files.
    """

    def __init__(self, project_dir: Path) -> None:
        """
        Initialize project context.

        Args:
            project_dir: Project root directory
        """
        self.project_dir = project_dir
        self._entropi_md: str | None = None
        self._gitignore_patterns: list[str] = []

    async def load(self) -> None:
        """Load all project context."""
        await self._load_entropi_md()
        await self._load_gitignore()

    async def _load_entropi_md(self) -> None:
        """Load ENTROPI.md if present."""
        entropi_path = self.project_dir / "ENTROPI.md"
        if entropi_path.exists():
            self._entropi_md = entropi_path.read_text()
            logger.info("Loaded ENTROPI.md")

    async def _load_gitignore(self) -> None:
        """Load .gitignore patterns."""
        gitignore_path = self.project_dir / ".gitignore"
        if gitignore_path.exists():
            self._gitignore_patterns = [
                line.strip()
                for line in gitignore_path.read_text().splitlines()
                if line.strip() and not line.startswith("#")
            ]

    def get_system_prompt_addition(self) -> str:
        """Get context to add to system prompt."""
        if not self._entropi_md:
            return ""

        return f"""

# Project Context

The following is project-specific context from ENTROPI.md:

{self._entropi_md}
"""

    @property
    def has_context(self) -> bool:
        """Check if any context is loaded."""
        return self._entropi_md is not None


class ContextCompactor:
    """
    Handles context compaction for long conversations.

    Summarizes older messages to free up context window space.
    """

    COMPACTION_PROMPT = """Summarize the following conversation history concisely.
Preserve:
- Key decisions made
- Important code changes or files modified
- Current task state and any blockers
- User preferences expressed

Conversation to summarize:
{conversation}

Provide a concise summary in 2-3 paragraphs:"""

    def __init__(
        self,
        max_tokens: int = 12000,
        compaction_threshold: float = 0.8,
    ) -> None:
        """
        Initialize compactor.

        Args:
            max_tokens: Maximum context tokens
            compaction_threshold: Trigger compaction at this % of max
        """
        self.max_tokens = max_tokens
        self.compaction_threshold = compaction_threshold

    def needs_compaction(
        self,
        messages: list[Message],
        count_fn: callable,
    ) -> bool:
        """
        Check if conversation needs compaction.

        Args:
            messages: Current messages
            count_fn: Token counting function

        Returns:
            True if compaction needed
        """
        total_tokens = sum(count_fn(m.content) for m in messages)
        threshold = self.max_tokens * self.compaction_threshold
        return total_tokens > threshold

    def prepare_for_compaction(
        self,
        messages: list[Message],
        keep_recent: int = 4,
    ) -> tuple[list[Message], list[Message]]:
        """
        Prepare messages for compaction.

        Args:
            messages: All messages
            keep_recent: Number of recent messages to preserve

        Returns:
            Tuple of (messages to compact, messages to keep)
        """
        if len(messages) <= keep_recent:
            return [], messages

        # Find compaction boundary (keep system + recent)
        system_msgs = [m for m in messages if m.role == "system"]
        non_system = [m for m in messages if m.role != "system"]

        to_compact = non_system[:-keep_recent] if len(non_system) > keep_recent else []
        to_keep = system_msgs + non_system[-keep_recent:]

        return to_compact, to_keep

    def create_compaction_message(self, summary: str) -> Message:
        """Create a message containing the compacted summary."""
        return Message(
            role="system",
            content=f"""[Previous conversation summary]

{summary}

[End of summary - conversation continues below]""",
            metadata={"is_compaction_summary": True},
        )

    def format_for_summarization(self, messages: list[Message]) -> str:
        """Format messages for summarization."""
        lines = []
        for msg in messages:
            role = msg.role.upper()
            content = msg.content[:500] + "..." if len(msg.content) > 500 else msg.content
            lines.append(f"[{role}]: {content}")
        return "\n\n".join(lines)
```

---

## 3. Tests

### File: `tests/unit/test_commands.py`

```python
"""Tests for command system."""
import pytest
import tempfile
from pathlib import Path

from entropi.core.commands import CommandRegistry, CommandContext, CommandResult


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
        assert not registry.is_command("help")
        assert not registry.is_command("hello /help")

    @pytest.mark.asyncio
    async def test_custom_command(self, tmp_path: Path) -> None:
        """Test custom command from file."""
        commands_dir = tmp_path / ".entropi" / "commands"
        commands_dir.mkdir(parents=True)

        # Create custom command file
        (commands_dir / "review.md").write_text("""---
name: review
description: Review code
---

Review the following code for issues: $ARGUMENTS
""")

        registry = CommandRegistry(tmp_path)
        await registry.discover()

        assert "review" in registry._commands
        assert registry._commands["review"].prompt_template is not None


class TestContextCompactor:
    """Tests for context compaction."""

    def test_needs_compaction(self) -> None:
        """Test compaction threshold detection."""
        from entropi.core.context import ContextCompactor
        from entropi.core.base import Message

        compactor = ContextCompactor(max_tokens=100, compaction_threshold=0.8)

        # Simple token count mock
        count_fn = lambda text: len(text.split())

        # Under threshold
        messages = [Message(role="user", content="hello world")]
        assert not compactor.needs_compaction(messages, count_fn)

        # Over threshold
        messages = [Message(role="user", content=" ".join(["word"] * 100))]
        assert compactor.needs_compaction(messages, count_fn)

    def test_prepare_for_compaction(self) -> None:
        """Test message splitting for compaction."""
        from entropi.core.context import ContextCompactor
        from entropi.core.base import Message

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
```

---

## Checkpoint: Verification

```bash
# Run tests
pytest tests/unit/test_commands.py -v

# Test commands interactively
entropi
> /help
> /status
> /model fast
> /clear
```

**Success Criteria:**
- [ ] Built-in commands work (/help, /clear, /status, /model)
- [ ] Custom command files are discovered
- [ ] ENTROPI.md is loaded into context
- [ ] Context compaction logic works

---

## Next Phase

Proceed to **Implementation 09: Quality Enforcement** to implement code quality checks.
