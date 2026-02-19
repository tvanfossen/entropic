"""
Auto-compaction for context management.

Automatically summarizes conversation history when approaching
token limits to prevent context overflow.

Uses deterministic structured extraction (not model-generated summaries)
to produce predictable, compact briefings that preserve original task,
tool call history, and files touched.
"""

import json
from dataclasses import dataclass
from typing import TYPE_CHECKING

from entropic.core.base import Message
from entropic.core.logging import get_logger

if TYPE_CHECKING:
    from entropic.config.schema import CompactionConfig
    from entropic.inference.orchestrator import ModelOrchestrator
    from entropic.storage.backend import SQLiteStorage

logger = get_logger("core.compaction")


class TokenCounter:
    """
    Track token usage across conversation.

    Uses simple heuristic (~4 chars per token) for estimation.
    More accurate counting would require the model's tokenizer.
    """

    def __init__(self, max_tokens: int) -> None:
        """
        Initialize token counter.

        Args:
            max_tokens: Maximum context length
        """
        self.max_tokens = max_tokens
        self._cache: dict[int, int] = {}

    def count_message(self, message: Message) -> int:
        """Count tokens in a single message."""
        # Use message id for caching if available
        cache_key = id(message)
        if cache_key in self._cache:
            return self._cache[cache_key]

        count = self._count_text(message.content)

        # Add overhead for role and formatting
        count += 4  # role tokens

        # Tool calls add tokens
        if message.tool_calls:
            for tc in message.tool_calls:
                count += self._count_text(str(tc))

        if message.tool_results:
            for tr in message.tool_results:
                count += self._count_text(str(tr))

        self._cache[cache_key] = count
        return count

    def count_messages(self, messages: list[Message]) -> int:
        """Count total tokens in message list."""
        return sum(self.count_message(msg) for msg in messages)

    def _count_text(self, text: str) -> int:
        """
        Estimate token count for text.

        Uses ~4 chars per token heuristic for English.
        """
        if not text:
            return 0
        return len(text) // 4 + 1

    def usage_percent(self, messages: list[Message]) -> float:
        """Get usage as percentage of context window."""
        if self.max_tokens == 0:
            return 0.0
        return self.count_messages(messages) / self.max_tokens

    def clear_cache(self) -> None:
        """Clear the token count cache."""
        self._cache.clear()


@dataclass
class CompactionResult:
    """Result of a compaction operation."""

    compacted: bool
    old_token_count: int
    new_token_count: int
    summary: str | None = None
    preserved_messages: int = 0
    messages_summarized: int = 0


class CompactionManager:
    """
    Manages automatic context compaction.

    Monitors token usage and triggers summarization when
    approaching the context limit.
    """

    def __init__(
        self,
        config: "CompactionConfig",
        token_counter: TokenCounter,
        orchestrator: "ModelOrchestrator | None" = None,
        storage: "SQLiteStorage | None" = None,
    ) -> None:
        """
        Initialize compaction manager.

        Args:
            config: Compaction configuration
            token_counter: Token counter instance
            orchestrator: Model orchestrator for generating summaries
            storage: Storage backend for saving snapshots
        """
        self.config = config
        self.counter = token_counter
        self.orchestrator = orchestrator
        self.storage = storage

    async def check_and_compact(
        self,
        conversation_id: str | None,
        messages: list[Message],
        *,
        force: bool = False,
    ) -> tuple[list[Message], CompactionResult]:
        """
        Check if compaction needed and perform if so.

        Args:
            conversation_id: Current conversation ID
            messages: Current message list
            force: Bypass threshold check and compact immediately

        Returns:
            Tuple of (possibly compacted messages, result info)
        """
        current_tokens = self.counter.count_messages(messages)
        threshold_tokens = int(self.counter.max_tokens * self.config.threshold_percent)

        logger.debug(
            f"Token check: {current_tokens}/{self.counter.max_tokens} "
            f"(threshold: {threshold_tokens})"
        )

        # No compaction needed (unless forced)
        if not force and current_tokens < threshold_tokens:
            return messages, CompactionResult(
                compacted=False,
                old_token_count=current_tokens,
                new_token_count=current_tokens,
            )

        # Compaction disabled
        if not self.config.enabled:
            logger.warning(
                f"Context at {current_tokens}/{self.counter.max_tokens} tokens, "
                "compaction disabled - may fail soon"
            )
            return messages, CompactionResult(
                compacted=False,
                old_token_count=current_tokens,
                new_token_count=current_tokens,
            )

        logger.info(f"Compacting conversation ({current_tokens} tokens)")

        # Save full history before compacting (if enabled)
        if self.config.save_full_history and self.storage and conversation_id:
            await self._save_snapshot(conversation_id, messages)

        # Perform value-density compaction (single pass, no fallback chain)
        compacted_messages, summary, stripped_count = await self._compact(messages)

        # Clear token cache since message objects changed
        self.counter.clear_cache()
        new_tokens = self.counter.count_messages(compacted_messages)

        if new_tokens >= current_tokens:
            logger.error(
                f"Compaction did not reduce tokens: {current_tokens} -> {new_tokens}. "
                "System prompt may exceed context budget."
            )

        logger.info(f"Compacted {current_tokens} -> {new_tokens} tokens")

        return compacted_messages, CompactionResult(
            compacted=True,
            old_token_count=current_tokens,
            new_token_count=new_tokens,
            summary=summary,
            preserved_messages=len(compacted_messages) - 1,
            messages_summarized=stripped_count,
        )

    async def _compact(
        self,
        messages: list[Message],
    ) -> tuple[list[Message], str, int]:
        """Compact by value density: strip tool results, keep user messages.

        Classifies messages by disposability:
        - System message: always kept
        - Real user input (source=user): always kept
        - Tool results, injections, warnings: stripped (captured in summary)
        - Assistant messages: keep only the most recent

        Returns:
            Tuple of (compacted messages, summary text, count stripped)
        """
        system_message = None
        working = messages
        if messages and messages[0].role == "system":
            system_message = messages[0]
            working = messages[1:]

        # Classify by value density
        user_messages: list[Message] = []
        assistant_messages: list[Message] = []
        stripped_count = 0

        for msg in working:
            if msg.metadata.get("source") == "user":
                user_messages.append(msg)
            elif msg.role == "assistant":
                assistant_messages.append(msg)
            else:
                stripped_count += 1

        # Build structured summary from ALL working messages (before stripping)
        summary = self._structured_summary(working)
        summary_msg = Message(
            role="user",
            content=self._format_summary(summary, len(working)),
        )

        logger.debug(
            f"Value-density compaction: {len(user_messages)} user msgs kept, "
            f"{len(assistant_messages)} assistant msgs (keeping last), "
            f"{stripped_count} stripped"
        )

        # Assemble: system + summary + user messages + last assistant
        result: list[Message] = []
        if system_message:
            result.append(system_message)
        result.append(summary_msg)
        result.extend(user_messages)
        if assistant_messages:
            result.append(assistant_messages[-1])

        return result, summary, stripped_count

    def _structured_summary(self, messages: list[Message]) -> str:
        """Build a deterministic structured summary from message history.

        Extracts facts (original task, tool calls, files touched) without
        model inference. Predictable size proportional to tool call count.
        """
        lines = []

        task = self._extract_original_task(messages)
        lines.append(f"Original task: {task}")

        tool_log = self._extract_tool_log(messages)
        if tool_log:
            lines.append("")
            lines.append("Tool calls made (oldest first):")
            for name, brief in tool_log:
                lines.append(f"- {name}: {brief}")

        files_read, files_modified = self._extract_files_touched(messages)
        if files_read:
            lines.append("")
            lines.append(f"Files read: {', '.join(files_read)}")
        if files_modified:
            lines.append(f"Files modified: {', '.join(files_modified)}")

        return "\n".join(lines)

    def _extract_original_task(self, messages: list[Message]) -> str:
        """Find the first real user message (the original task)."""
        # Primary: use source metadata tag
        for msg in messages:
            if msg.metadata.get("source") == "user":
                content = msg.content
                if len(content) > 500:
                    content = content[:500] + "..."
                return content
        # Fallback: heuristic for untagged messages
        for msg in messages:
            if msg.role == "user" and not msg.content.startswith(("[", "Tool ")):
                content = msg.content
                if len(content) > 500:
                    content = content[:500] + "..."
                return content
        return "(no user message found)"

    def _extract_tool_log(self, messages: list[Message]) -> list[tuple[str, str]]:
        """Extract tool call names and brief results from messages."""
        log: list[tuple[str, str]] = []
        for msg in messages:
            if not msg.tool_results:
                continue
            if msg.content.startswith("[Previous:"):
                tool_name = msg.metadata.get("tool_name", "unknown")
                log.append((tool_name, "(pruned)"))
                continue
            for tr in msg.tool_results:
                if not isinstance(tr, dict):
                    continue
                name = tr.get("name", msg.metadata.get("tool_name", "unknown"))
                result_text = tr.get("result", "")
                brief = result_text.split("\n")[0][:100]
                log.append((name, brief))
        return log

    def _extract_files_touched(self, messages: list[Message]) -> tuple[list[str], list[str]]:
        """Extract file paths from filesystem tool results."""
        files_read: list[str] = []
        files_modified: list[str] = []
        for msg in messages:
            self._collect_file_paths(msg, files_read, files_modified)
        return files_read, files_modified

    def _collect_file_paths(
        self,
        msg: Message,
        files_read: list[str],
        files_modified: list[str],
    ) -> None:
        """Collect file paths from a single message's tool results."""
        if not msg.tool_results:
            return
        for tr in msg.tool_results:
            if not isinstance(tr, dict):
                continue
            name = tr.get("name", "")
            if not name.startswith("filesystem."):
                continue
            path = self._parse_path_from_result(tr.get("result", ""))
            if not path:
                continue
            if "read" in name and path not in files_read:
                files_read.append(path)
            elif "read" not in name and path not in files_modified:
                files_modified.append(path)

    @staticmethod
    def _parse_path_from_result(result: str) -> str | None:
        """Try to extract a file path from a tool result JSON string."""
        try:
            data = json.loads(result)
            if isinstance(data, dict):
                return data.get("path")
        except (json.JSONDecodeError, TypeError, ValueError):
            pass
        return None

    def _format_summary(self, summary: str, message_count: int) -> str:
        """Format the summary as a system message."""
        return f"""[CONVERSATION SUMMARY]
The following summarizes {message_count} previous messages that have been compacted to save context space.

{summary}

[END SUMMARY - Recent conversation continues below]"""

    async def _save_snapshot(
        self,
        conversation_id: str,
        messages: list[Message],
    ) -> None:
        """Save full conversation before compaction."""
        if not self.storage:
            return

        try:
            # Storage would need a method like save_compaction_snapshot
            # For now, log that we would save
            logger.debug(
                f"Would save compaction snapshot for {conversation_id} ({len(messages)} messages)"
            )
        except Exception as e:
            logger.error(f"Failed to save compaction snapshot: {e}")

    async def compress_for_handoff(
        self,
        messages: list[Message],
        handoff_reason: str,
        target_context_length: int,
    ) -> list[Message]:
        """Compress context for tier handoff using value-density stripping.

        Keeps system message, user messages, and handoff context.
        Strips tool results and old assistant messages.

        Args:
            messages: Current conversation
            handoff_reason: Why handoff is happening
            target_context_length: Target tier's context limit

        Returns:
            Compressed messages suitable for new tier
        """
        system_message = None
        working = messages
        if messages and messages[0].role == "system":
            system_message = messages[0]
            working = messages[1:]

        # Keep real user messages
        user_messages = [m for m in working if m.metadata.get("source") == "user"]

        # Build structured summary + handoff reason
        summary = self._structured_summary(working)
        handoff_context = (
            f"[HANDOFF CONTEXT]\n{summary}\n\n"
            f"Handoff reason: {handoff_reason}\n[END HANDOFF CONTEXT]"
        )

        result: list[Message] = []
        if system_message:
            result.append(system_message)
        result.append(Message(role="user", content=handoff_context))
        result.extend(user_messages)

        logger.info(
            f"Handoff compression: {len(messages)} -> {len(result)} messages "
            f"for target context {target_context_length}"
        )

        return result
