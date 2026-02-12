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

from entropi.core.base import Message
from entropi.core.logging import get_logger

if TYPE_CHECKING:
    from entropi.config.schema import CompactionConfig
    from entropi.inference.orchestrator import ModelOrchestrator
    from entropi.storage.backend import SQLiteStorage

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

        # Perform compaction
        compacted_messages, summary, summarized_count = await self._compact(messages)

        # Clear token cache since message objects changed
        self.counter.clear_cache()
        new_tokens = self.counter.count_messages(compacted_messages)

        # Verify we actually reduced tokens (otherwise we might loop)
        # Must reduce by at least 20% to be considered successful
        if new_tokens >= current_tokens * 0.8:
            logger.warning(
                f"Compaction failed to reduce significantly: {current_tokens} -> {new_tokens}. "
                "Using aggressive fallback."
            )
            # Aggressive fallback: keep only system + last 2 messages
            compacted_messages = self._aggressive_compact(messages)
            self.counter.clear_cache()
            new_tokens = self.counter.count_messages(compacted_messages)
            logger.info(f"Aggressive compaction: {current_tokens} -> {new_tokens} tokens")

        logger.info(f"Compacted {current_tokens} -> {new_tokens} tokens")

        return compacted_messages, CompactionResult(
            compacted=True,
            old_token_count=current_tokens,
            new_token_count=new_tokens,
            summary=summary,
            preserved_messages=len(compacted_messages) - 1,  # -1 for summary
            messages_summarized=summarized_count,
        )

    async def _compact(
        self,
        messages: list[Message],
    ) -> tuple[list[Message], str, int]:
        """
        Perform the actual compaction.

        Targets 50% of current tokens to ensure meaningful reduction.

        Returns:
            Tuple of (compacted messages, summary text, count of summarized messages)
        """
        current_tokens = self.counter.count_messages(messages)
        # Target 50% of context window, not 50% of current
        # This ensures target is always under the context limit
        target_tokens = min(current_tokens // 2, int(self.counter.max_tokens * 0.5))

        # Keep system message if present
        system_message = None
        system_tokens = 0
        working_messages = messages
        if messages and messages[0].role == "system":
            system_message = messages[0]
            system_tokens = self.counter.count_message(system_message)
            working_messages = messages[1:]

        # Reserve tokens: system prompt + summary wrapper + buffer
        wrapper_tokens = 100  # Approximate tokens for wrapper text
        summary_budget = min(self.config.summary_max_tokens, target_tokens // 4)

        # Calculate how many recent messages we can preserve
        available_for_recent = target_tokens - system_tokens - wrapper_tokens - summary_budget

        # Start with configured preserve count and reduce if needed
        preserve_count = self.config.preserve_recent_turns * 2
        recent_messages: list[Message] = []

        # Work backwards from most recent, adding messages until we hit budget
        if preserve_count > 0 and preserve_count < len(working_messages):
            candidate_recent = working_messages[-preserve_count:]
        else:
            candidate_recent = working_messages[-(len(working_messages) // 2) :]

        recent_tokens = 0
        for msg in reversed(candidate_recent):
            msg_tokens = self.counter.count_message(msg)
            if recent_tokens + msg_tokens <= available_for_recent:
                recent_messages.insert(0, msg)
                recent_tokens += msg_tokens
            else:
                break

        # Ensure we keep at least 2 messages (1 turn) if possible
        if len(recent_messages) < 2 and len(candidate_recent) >= 2:
            recent_messages = candidate_recent[-2:]
            recent_tokens = sum(self.counter.count_message(m) for m in recent_messages)

        # Messages to summarize = everything except system and recent
        if recent_messages:
            # Find the split point
            recent_start_idx = len(working_messages) - len(recent_messages)
            old_messages = working_messages[:recent_start_idx]
        else:
            old_messages = working_messages
            recent_messages = []

        logger.debug(
            f"Compaction plan: summarize {len(old_messages)} msgs, "
            f"keep {len(recent_messages)} recent msgs, "
            f"summary budget: {summary_budget} tokens"
        )

        # Build structured summary from old messages (deterministic, no model)
        summary = self._structured_summary(old_messages)

        # Build compacted message list
        # Keep system message separate - don't concatenate into summary
        # This prevents doubling the system prompt size
        summary_message = Message(
            role="user",  # Use user role for summary to avoid system message bloat
            content=self._format_summary(summary, len(old_messages)),
        )

        result = []
        if system_message:
            result.append(system_message)
        result.append(summary_message)
        result.extend(recent_messages)

        return result, summary, len(old_messages)

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
        """Find the first non-system user message (the original task)."""
        for msg in messages:
            if msg.role == "user" and not msg.content.startswith("["):
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

    def _aggressive_compact(self, messages: list[Message]) -> list[Message]:
        """Emergency compaction: structured summary + last 2 messages.

        Unlike the old approach (system + last 2 only), this preserves the
        original task and tool call history via structured extraction.
        """
        result = []

        # Keep system message if present
        working_messages = messages
        if messages and messages[0].role == "system":
            result.append(messages[0])
            working_messages = messages[1:]

        # Build structured summary from all non-system messages
        summary = self._structured_summary(working_messages)
        result.append(
            Message(
                role="user",
                content=self._format_summary(summary, len(working_messages)),
            )
        )

        # Keep last 2 messages (1 turn) for immediate context
        if len(working_messages) >= 2:
            result.extend(working_messages[-2:])
        elif working_messages:
            result.extend(working_messages)

        logger.debug(
            f"Aggressive compaction: kept {len(result)} messages from original {len(messages)}"
        )
        return result

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
        """
        Compress context specifically for tier handoff.

        Uses structured extraction (same as compaction) plus handoff reason.

        Args:
            messages: Current conversation
            handoff_reason: Why handoff is happening
            target_context_length: Target tier's context limit

        Returns:
            Compressed messages suitable for new tier
        """
        target_tokens = int(target_context_length * 0.4)

        # Keep system message
        system_message = None
        working_messages = messages
        if messages and messages[0].role == "system":
            system_message = messages[0]
            working_messages = messages[1:]

        # Build structured summary (deterministic, no model inference)
        summary = self._structured_summary(working_messages)

        result = []
        if system_message:
            result.append(system_message)

        handoff_context = (
            f"[HANDOFF CONTEXT]\n{summary}\n\n"
            f"Handoff reason: {handoff_reason}\n[END HANDOFF CONTEXT]"
        )
        result.append(Message(role="user", content=handoff_context))

        # Keep last 2 messages for immediate context (if they fit)
        if len(working_messages) >= 2:
            recent = working_messages[-2:]
            recent_tokens = sum(self.counter.count_message(m) for m in recent)
            if recent_tokens < target_tokens // 4:
                result.extend(recent)

        logger.info(
            f"Handoff compression: {len(messages)} -> {len(result)} messages "
            f"for target context {target_context_length}"
        )

        return result
