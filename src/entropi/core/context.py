"""
Context builder for managing conversation context.

Handles token budgeting, message assembly, context window management,
ENTROPI.md loading, and context compaction for long conversations.
"""

from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from entropi.config.schema import EntropyConfig
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
        count_fn: Callable[[str], int],
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


@dataclass
class TokenBudget:
    """Token budget allocation."""

    total: int
    system: int
    history: int
    tools: int
    response: int

    @classmethod
    def from_context_length(cls, context_length: int) -> "TokenBudget":
        """Create budget from context length."""
        return cls(
            total=context_length,
            system=2000,
            history=context_length - 6000,  # Leave room for response
            tools=1000,
            response=4000,
        )


class ContextBuilder:
    """
    Builds and manages conversation context.

    Responsibilities:
    - System prompt assembly
    - Token budgeting
    - History truncation
    - ENTROPI.md loading
    """

    DEFAULT_SYSTEM_PROMPT = """You are Entropi, a local AI coding assistant.

You help developers write, review, and improve code. You have access to tools for:
- Reading and writing files
- Executing shell commands
- Git operations

When you need to perform actions, use the available tools. Think step by step and explain your reasoning.

Be concise but thorough. Write clean, well-documented code following best practices."""

    def __init__(self, config: EntropyConfig) -> None:
        """
        Initialize context builder.

        Args:
            config: Application configuration
        """
        self.config = config
        self._default_system_prompt = self._load_default_prompt()
        self._project_context: str | None = None

    def _load_default_prompt(self) -> str:
        """Load default system prompt."""
        default_path = self.config.prompts_dir / "system" / "default.md"

        if default_path.exists():
            return default_path.read_text()

        return self.DEFAULT_SYSTEM_PROMPT

    def load_project_context(self, project_dir: Path) -> None:
        """
        Load ENTROPI.md from project directory.

        Args:
            project_dir: Project directory path
        """
        entropi_md = project_dir / "ENTROPI.md"

        if entropi_md.exists():
            self._project_context = entropi_md.read_text()
            logger.info(f"Loaded project context from {entropi_md}")
        else:
            self._project_context = None

    def build_system_prompt(
        self,
        base_prompt: str | None = None,
        tools: list[dict[str, Any]] | None = None,
    ) -> str:
        """
        Build complete system prompt.

        Args:
            base_prompt: Optional base prompt override
            tools: Available tools (formatted by adapter)

        Returns:
            Complete system prompt
        """
        parts = []

        # Base prompt
        parts.append(base_prompt or self._default_system_prompt)

        # Project context
        if self._project_context:
            parts.append("\n# Project Context\n")
            parts.append(self._project_context)

        # Tool formatting is handled by the adapter
        return "\n".join(parts)

    def truncate_history(
        self,
        messages: list[Message],
        budget: TokenBudget,
        count_tokens: Callable[[str], int],
    ) -> list[Message]:
        """
        Truncate history to fit within token budget.

        Preserves system messages and ensures most recent messages are kept.

        Args:
            messages: All messages
            budget: Token budget
            count_tokens: Token counting function

        Returns:
            Truncated messages
        """
        if not messages:
            return messages

        # Separate system messages from conversation
        system_msgs = [m for m in messages if m.role == "system"]
        other_msgs = [m for m in messages if m.role != "system"]

        if not other_msgs:
            return messages

        # Count tokens in system messages
        system_tokens = sum(count_tokens(m.content) for m in system_msgs)
        available = budget.history - system_tokens

        # Build result from most recent messages
        result: list[Message] = []
        current_tokens = 0

        for msg in reversed(other_msgs):
            msg_tokens = count_tokens(msg.content)
            if current_tokens + msg_tokens > available:
                # Add truncation marker if we're dropping messages
                if result:
                    logger.debug(
                        f"Truncating history, dropped {len(other_msgs) - len(result)} messages"
                    )
                break
            result.insert(0, msg)
            current_tokens += msg_tokens

        return system_msgs + result

    def estimate_tokens(self, text: str) -> int:
        """
        Rough token estimation (~4 chars per token).

        Args:
            text: Text to estimate

        Returns:
            Estimated token count
        """
        return len(text) // 4

    def get_budget(self, context_length: int | None = None) -> TokenBudget:
        """
        Get token budget for current configuration.

        Args:
            context_length: Override context length

        Returns:
            Token budget
        """
        length = context_length or 16384  # Default
        return TokenBudget.from_context_length(length)
