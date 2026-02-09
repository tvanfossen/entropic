"""
Slash command system.

Provides built-in commands and supports user-defined commands
from project and global directories.
"""

import re
from abc import ABC, abstractmethod
from collections.abc import Callable, Coroutine
from dataclasses import dataclass
from pathlib import Path
from typing import Any

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
        self.register_builtin(ThinkCommand())
        self.register_builtin(SaveCommand())
        self.register_builtin(LoadCommand())
        # Session commands
        self.register_builtin(SessionsCommand())
        self.register_builtin(SessionCommand())
        self.register_builtin(NewCommand())
        self.register_builtin(RenameCommand())
        self.register_builtin(DeleteSessionCommand())
        self.register_builtin(ExportCommand())
        # Voice commands
        self.register_builtin(VoiceSetupCommand())

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
        name, args = self._parse_command(command_str)

        if name not in self._commands:
            return CommandResult(
                success=False,
                message=f"Unknown command: /{name}. Use /help for available commands.",
            )

        return await self._execute_definition(self._commands[name], args, context)

    def _parse_command(self, command_str: str) -> tuple[str, str]:
        """Parse command string into name and args."""
        parts = command_str.lstrip("/").split(maxsplit=1)
        name = parts[0].lower()
        args = parts[1] if len(parts) > 1 else ""
        return name, args

    async def _execute_definition(
        self,
        definition: CommandDefinition,
        args: str,
        context: CommandContext,
    ) -> CommandResult:
        """Execute a command definition."""
        if definition.handler:
            return await definition.handler(args, context)
        if definition.prompt_template:
            return await self._execute_custom_command(definition, args, context)
        return CommandResult(success=False, message=f"Command {definition.name} has no handler")

    async def _execute_custom_command(
        self,
        definition: CommandDefinition,
        args: str,
        context: CommandContext,
    ) -> CommandResult:
        """Execute a custom command by injecting prompt."""
        # Replace $ARGUMENTS placeholder (prompt_template is validated before this call)
        assert definition.prompt_template is not None
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
        return "/model [thinking|normal|code|micro]"

    async def execute(self, args: str, context: CommandContext) -> CommandResult:
        if not args:
            return CommandResult(
                success=True,
                data={"action": "show_model"},
            )

        model = args.lower().strip()
        valid = {"thinking", "normal", "code", "micro"}
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


class ThinkCommand(Command):
    """Control thinking model usage."""

    @property
    def name(self) -> str:
        return "think"

    @property
    def description(self) -> str:
        return "Control when the thinking model is used"

    @property
    def usage(self) -> str:
        return "/think [on|off|status] - on=force for all, off=auto (complex tasks only)"

    async def execute(self, args: str, context: CommandContext) -> CommandResult:
        arg = args.lower().strip()

        results = {
            "": CommandResult(success=True, data={"action": "show_thinking_status"}),
            "status": CommandResult(success=True, data={"action": "show_thinking_status"}),
            "on": CommandResult(
                success=True,
                message="Thinking model forced ON for all reasoning tasks",
                data={"action": "set_thinking_mode", "enabled": True},
            ),
            "off": CommandResult(
                success=True,
                message="Thinking model AUTO - used only for complex tasks",
                data={"action": "set_thinking_mode", "enabled": False},
            ),
            "auto": CommandResult(
                success=True,
                message="Thinking model AUTO - used only for complex tasks",
                data={"action": "set_thinking_mode", "enabled": False},
            ),
        }

        return results.get(
            arg,
            CommandResult(
                success=False, message=f"Unknown argument: {arg}. Use: on, off/auto, or status"
            ),
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


# Session management commands


class SessionsCommand(Command):
    """List sessions for current project."""

    @property
    def name(self) -> str:
        return "sessions"

    @property
    def description(self) -> str:
        return "List all sessions for this project"

    async def execute(self, args: str, context: CommandContext) -> CommandResult:
        return CommandResult(
            success=True,
            data={"action": "list_sessions"},
        )


class SessionCommand(Command):
    """Switch to a specific session."""

    @property
    def name(self) -> str:
        return "session"

    @property
    def description(self) -> str:
        return "Switch to a specific session"

    @property
    def usage(self) -> str:
        return "/session <id>"

    async def execute(self, args: str, context: CommandContext) -> CommandResult:
        if not args:
            return CommandResult(
                success=False,
                message="Usage: /session <id>. Use /sessions to list available sessions.",
            )

        return CommandResult(
            success=True,
            data={"action": "switch_session", "session_id": args.strip()},
        )


class NewCommand(Command):
    """Start a new session."""

    @property
    def name(self) -> str:
        return "new"

    @property
    def description(self) -> str:
        return "Start a new conversation session"

    @property
    def usage(self) -> str:
        return "/new [name]"

    async def execute(self, args: str, context: CommandContext) -> CommandResult:
        name = args.strip() if args else "New session"

        return CommandResult(
            success=True,
            data={"action": "new_session", "name": name},
        )


class RenameCommand(Command):
    """Rename current session."""

    @property
    def name(self) -> str:
        return "rename"

    @property
    def description(self) -> str:
        return "Rename the current session"

    @property
    def usage(self) -> str:
        return "/rename <new name>"

    async def execute(self, args: str, context: CommandContext) -> CommandResult:
        if not args:
            return CommandResult(
                success=False,
                message="Usage: /rename <new name>",
            )

        return CommandResult(
            success=True,
            data={"action": "rename_session", "name": args.strip()},
        )


class DeleteSessionCommand(Command):
    """Delete a session."""

    @property
    def name(self) -> str:
        return "delete"

    @property
    def description(self) -> str:
        return "Delete a session (not the current one)"

    @property
    def usage(self) -> str:
        return "/delete <session_id>"

    async def execute(self, args: str, context: CommandContext) -> CommandResult:
        if not args:
            return CommandResult(
                success=False,
                message="Usage: /delete <session_id>. Use /sessions to list sessions.",
            )

        return CommandResult(
            success=True,
            data={"action": "delete_session", "session_id": args.strip()},
        )


class ExportCommand(Command):
    """Export a session to markdown."""

    @property
    def name(self) -> str:
        return "export"

    @property
    def description(self) -> str:
        return "Export a session to markdown"

    @property
    def usage(self) -> str:
        return "/export [session_id]"

    async def execute(self, args: str, context: CommandContext) -> CommandResult:
        session_id = args.strip() if args else None

        return CommandResult(
            success=True,
            data={"action": "export_session", "session_id": session_id},
        )


# Voice commands


class VoiceSetupCommand(Command):
    """Generate thinking audio using PersonaPlex."""

    @property
    def name(self) -> str:
        return "voice-setup"

    @property
    def description(self) -> str:
        return "Generate thinking audio clip using PersonaPlex voice"

    @property
    def usage(self) -> str:
        return '/voice-setup [--text "Custom thinking text..."]'

    async def execute(self, args: str, context: CommandContext) -> CommandResult:
        """
        Generate thinking audio using the configured voice prompt.

        This command:
        1. Loads the selected voice prompt from config
        2. Initializes PersonaPlex temporarily
        3. Generates audio from text (default: "Let me think about that...")
        4. Saves to thinking_moment.wav in the voices directory
        5. Unloads PersonaPlex to free VRAM
        """
        # Parse arguments
        text = "Let me think about that for a moment..."
        if args:
            # Check for --text flag
            if args.startswith("--text "):
                text = args[7:].strip().strip('"').strip("'")
            else:
                text = args.strip()

        # Verify voice is configured
        voice_config = context.config.voice
        if not voice_config.enabled:
            return CommandResult(
                success=False,
                message="Voice mode is not enabled. Set voice.enabled: true in config first.",
            )

        # Check voice prompt exists
        voice_prompt_path = (
            voice_config.voice_prompt.prompt_dir / voice_config.voice_prompt.prompt_file
        )
        if not voice_prompt_path.exists():
            return CommandResult(
                success=False,
                message=f"Voice prompt not found: {voice_prompt_path}\n"
                "Please download or create a voice prompt file first.",
            )

        return CommandResult(
            success=True,
            message=f'Generating thinking audio with text: "{text}"',
            data={
                "action": "voice_setup",
                "text": text,
                "voice_prompt_path": str(voice_prompt_path),
                "output_path": str(
                    voice_config.voice_prompt.prompt_dir / voice_config.voice_prompt.thinking_audio
                ),
            },
        )
