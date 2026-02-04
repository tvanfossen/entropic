"""
Auto-compaction for context management.

Automatically summarizes conversation history when approaching
token limits to prevent context overflow.
"""

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
    ) -> tuple[list[Message], CompactionResult]:
        """
        Check if compaction needed and perform if so.

        Args:
            conversation_id: Current conversation ID
            messages: Current message list

        Returns:
            Tuple of (possibly compacted messages, result info)
        """
        current_tokens = self.counter.count_messages(messages)
        threshold_tokens = int(self.counter.max_tokens * self.config.threshold_percent)

        logger.debug(
            f"Token check: {current_tokens}/{self.counter.max_tokens} "
            f"(threshold: {threshold_tokens})"
        )

        # No compaction needed
        if current_tokens < threshold_tokens:
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
        recent_messages = []

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

        # Generate summary of old messages with budget
        summary = await self._generate_summary(old_messages, summary_budget)

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

    async def _generate_summary(
        self, messages: list[Message], max_tokens: int | None = None
    ) -> str:
        """Generate a summary of conversation history."""
        if not self.orchestrator:
            # Fallback: simple truncation if no orchestrator
            return self._simple_summary(messages, max_tokens)

        # Build context for summarization
        conversation_text = self._format_for_summary(messages)

        # Calculate target summary length
        target_tokens = max_tokens or self.config.summary_max_tokens
        max_chars = target_tokens * 4  # ~4 chars per token

        summary_prompt = f"""Summarize this conversation briefly in {target_tokens} tokens or less.

CRITICAL - You MUST include:
1. What task the user requested
2. ALL tool calls that were made and their results (list each tool with outcome)
3. Current state/progress
4. Any files that were read or modified

This summary will be used to continue the conversation, so the model needs to know what tools have already been called to avoid duplicates.

CONVERSATION:
{conversation_text}

Brief summary (include tool call history):"""

        try:
            result = await self.orchestrator.generate(
                messages=[Message(role="user", content=summary_prompt)],
            )
            # Enforce max length - truncate if necessary
            summary = result.content
            if len(summary) > max_chars:
                logger.warning(
                    f"Summary too long ({len(summary)} chars), truncating to {max_chars}"
                )
                summary = summary[:max_chars] + "..."
            return summary
        except Exception as e:
            logger.error(f"Summary generation failed: {e}")
            return self._simple_summary(messages, max_tokens)

    def _simple_summary(self, messages: list[Message], max_tokens: int | None = None) -> str:
        """Create a simple summary without model generation."""
        max_chars = (max_tokens or 500) * 4

        lines = [f"[Summary of {len(messages)} messages]"]

        # Extract key points from recent messages
        for msg in messages[-5:]:  # Last 5 messages
            preview = msg.content[:100].replace("\n", " ")
            if len(msg.content) > 100:
                preview += "..."
            lines.append(f"- {msg.role}: {preview}")

        result = "\n".join(lines)
        if len(result) > max_chars:
            result = result[:max_chars] + "..."
        return result

    def _format_for_summary(self, messages: list[Message]) -> str:
        """Format messages for summarization prompt."""
        lines = []
        for msg in messages:
            role = msg.role.upper()
            # Truncate very long messages
            content = msg.content
            if len(content) > 2000:
                content = content[:2000] + "..."
            lines.append(f"[{role}]: {content}")
        return "\n\n".join(lines)

    def _format_summary(self, summary: str, message_count: int) -> str:
        """Format the summary as a system message."""
        return f"""[CONVERSATION SUMMARY]
The following summarizes {message_count} previous messages that have been compacted to save context space.

{summary}

[END SUMMARY - Recent conversation continues below]"""

    def _aggressive_compact(self, messages: list[Message]) -> list[Message]:
        """Emergency compaction: keep only system + minimal recent context."""
        result = []

        # Keep system message if present
        working_messages = messages
        if messages and messages[0].role == "system":
            result.append(messages[0])
            working_messages = messages[1:]

        # Keep only last 2 messages (1 turn)
        if len(working_messages) >= 2:
            result.extend(working_messages[-2:])
        elif working_messages:
            result.extend(working_messages)

        logger.debug(
            f"Aggressive compaction: kept {len(result)} messages "
            f"from original {len(messages)}"
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
                f"Would save compaction snapshot for {conversation_id} "
                f"({len(messages)} messages)"
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

        Creates a focused summary for the new tier that includes:
        - Original user request
        - Tool calls made and their results
        - Files touched
        - Handoff reason and task state

        Args:
            messages: Current conversation
            handoff_reason: Why handoff is happening
            target_context_length: Target tier's context limit

        Returns:
            Compressed messages suitable for new tier
        """
        # Calculate target budget (aim for 40% of target context to leave room)
        target_tokens = int(target_context_length * 0.4)

        # Keep system message
        system_message = None
        working_messages = messages
        if messages and messages[0].role == "system":
            system_message = messages[0]
            working_messages = messages[1:]

        # Generate handoff-focused summary
        summary = await self._generate_handoff_summary(
            working_messages,
            handoff_reason,
            max_tokens=target_tokens // 2,
        )

        # Build compressed context
        result = []
        if system_message:
            result.append(system_message)

        # Add handoff context as user message
        handoff_context = f"""[HANDOFF CONTEXT]
{summary}

Handoff reason: {handoff_reason}
[END HANDOFF CONTEXT]"""

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

    async def _generate_handoff_summary(
        self,
        messages: list[Message],
        reason: str,
        max_tokens: int,
    ) -> str:
        """Generate a focused summary for handoff."""
        if not self.orchestrator:
            return self._simple_handoff_summary(messages, reason)

        conversation_text = self._format_for_summary(messages)
        max_chars = max_tokens * 4

        summary_prompt = f"""Summarize this conversation for handoff to another model tier.

The handoff reason is: {reason}

You MUST include:
1. The original user request/goal
2. ALL tool calls made and their results (be specific)
3. Files that were read or modified
4. Current state and what remains to be done
5. Any decisions made or problems encountered

Keep summary under {max_tokens} tokens.

CONVERSATION:
{conversation_text}

Summary:"""

        try:
            result = await self.orchestrator.generate(
                messages=[Message(role="user", content=summary_prompt)],
            )
            summary = result.content
            if len(summary) > max_chars:
                summary = summary[:max_chars] + "..."
            return summary
        except Exception as e:
            logger.error(f"Handoff summary generation failed: {e}")
            return self._simple_handoff_summary(messages, reason)

    def _simple_handoff_summary(self, messages: list[Message], reason: str) -> str:
        """Create simple handoff summary without model generation."""
        lines = [f"[Handoff Summary - {len(messages)} messages]"]
        lines.append(f"Reason: {reason}")
        lines.append("")

        # Extract tool calls
        tool_calls = []
        for msg in messages:
            if msg.tool_results:
                for tr in msg.tool_results:
                    if isinstance(tr, dict):
                        tool_calls.append(f"- {tr.get('name', 'unknown')}: {tr.get('result', '')[:100]}")

        if tool_calls:
            lines.append("Tool calls made:")
            lines.extend(tool_calls[:10])  # Limit to 10
            lines.append("")

        # Last user message as context
        for msg in reversed(messages):
            if msg.role == "user" and not msg.content.startswith("["):
                lines.append(f"Last user request: {msg.content[:200]}")
                break

        return "\n".join(lines)
