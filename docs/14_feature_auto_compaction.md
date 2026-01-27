# Feature Proposal: Auto-Compaction

> Automatic context summarization when approaching token limits

**Status:** Implemented (Core)
**Priority:** High (prevents context overflow failures)
**Complexity:** Medium
**Dependencies:** Inference engine

## Implementation Status

| Component | Status | Location |
|-----------|--------|----------|
| CompactionConfig | Done | `src/entropi/config/schema.py` |
| TokenCounter | Done | `src/entropi/core/compaction.py` |
| CompactionManager | Done | `src/entropi/core/compaction.py` |
| AgentEngine integration | Done | `src/entropi/core/engine.py` |
| UI notifications | Done | `src/entropi/ui/terminal.py`, `src/entropi/app.py` |
| Context bar in StatusBar | Done | `src/entropi/ui/components.py` |
| /compact commands | Not started | - |

---

## Problem Statement

Long coding sessions accumulate context (messages, tool results, code snippets) that eventually exceeds the model's context window. When this happens:

1. **Hard failure**: Model refuses to generate (context too long)
2. **Quality degradation**: Important early context gets truncated
3. **User frustration**: Must manually start new session, losing context

OpenCode solves this with "auto-compact" at 95% threshold. Entropi needs equivalent functionality.

---

## Solution: Auto-Compaction

Monitor token usage continuously. When threshold reached, automatically:
1. Summarize older conversation history
2. Preserve recent turns verbatim
3. Replace old messages with summary
4. Continue seamlessly

### User Experience

```
╭─ Entropi ──────────────────────────────────────────────────────╮
│ 🧠 Thinking │ VRAM: 14.2/16 GB │ Tokens: 14,892/16,384 (91%)  │
╰────────────────────────────────────────────────────────────────╯

[Context approaching limit - auto-compacting...]

╭─ Entropi ──────────────────────────────────────────────────────╮
│ 🧠 Thinking │ VRAM: 14.2/16 GB │ Tokens: 3,241/16,384 (20%)   │
╰────────────────────────────────────────────────────────────────╯

✓ Compacted conversation (14,892 → 3,241 tokens)
  Preserved: Last 2 exchanges
  Summary saved to session history
```

---

## Configuration

```yaml
# ~/.entropi/config.yaml
compaction:
  enabled: true
  threshold_percent: 0.90      # Trigger at 90% of context window
  preserve_recent_turns: 2     # Keep last N user+assistant pairs verbatim
  summary_max_tokens: 1500     # Max tokens for summary
  summary_model: "micro"       # Model to use for summarization (fast)
  notify_user: true            # Show notification when compacting
  save_full_history: true      # Save pre-compaction history to database
```

---

## Implementation

### 1. Token Counter

```python
class TokenCounter:
    """Track token usage across conversation."""

    def __init__(self, model_context_length: int) -> None:
        self.max_tokens = model_context_length
        self._cache: dict[str, int] = {}  # message_id -> token_count

    def count_messages(self, messages: list[Message]) -> int:
        """Count total tokens in message list."""
        total = 0
        for msg in messages:
            if msg.id in self._cache:
                total += self._cache[msg.id]
            else:
                count = self._count_text(msg.content)
                self._cache[msg.id] = count
                total += count
        return total

    def _count_text(self, text: str) -> int:
        """Estimate token count for text.

        Uses simple heuristic: ~4 chars per token for English.
        More accurate would be to use the model's tokenizer.
        """
        return len(text) // 4 + 1

    def usage_percent(self, messages: list[Message]) -> float:
        """Get usage as percentage of context window."""
        return self.count_messages(messages) / self.max_tokens
```

### 2. Compaction Manager

```python
@dataclass
class CompactionResult:
    """Result of a compaction operation."""

    compacted: bool
    old_token_count: int
    new_token_count: int
    summary: str | None = None
    preserved_messages: int = 0


class CompactionManager:
    """Manages automatic context compaction."""

    def __init__(
        self,
        config: CompactionConfig,
        token_counter: TokenCounter,
        orchestrator: ModelOrchestrator,
        storage: StorageBackend,
    ) -> None:
        self.config = config
        self.counter = token_counter
        self.orchestrator = orchestrator
        self.storage = storage

    async def check_and_compact(
        self,
        conversation_id: str,
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

        # Save full history before compacting (if enabled)
        if self.config.save_full_history:
            await self._save_pre_compaction_snapshot(conversation_id, messages)

        # Perform compaction
        compacted_messages, summary = await self._compact(messages)
        new_tokens = self.counter.count_messages(compacted_messages)

        logger.info(f"Compacted {current_tokens} → {new_tokens} tokens")

        return compacted_messages, CompactionResult(
            compacted=True,
            old_token_count=current_tokens,
            new_token_count=new_tokens,
            summary=summary,
            preserved_messages=len(compacted_messages) - 1,  # -1 for summary message
        )

    async def _compact(
        self,
        messages: list[Message],
    ) -> tuple[list[Message], str]:
        """Perform the actual compaction."""
        # Calculate how many messages to preserve
        # Each "turn" is typically user + assistant = 2 messages
        preserve_count = self.config.preserve_recent_turns * 2

        # Split messages
        if preserve_count > 0 and preserve_count < len(messages):
            old_messages = messages[:-preserve_count]
            recent_messages = messages[-preserve_count:]
        else:
            old_messages = messages
            recent_messages = []

        # Generate summary of old messages
        summary = await self._generate_summary(old_messages)

        # Build compacted message list
        summary_message = Message(
            role="system",
            content=self._format_summary(summary, len(old_messages)),
        )

        return [summary_message, *recent_messages], summary

    async def _generate_summary(self, messages: list[Message]) -> str:
        """Generate a summary of conversation history."""
        # Build context for summarization
        conversation_text = self._format_for_summary(messages)

        summary_prompt = f"""Summarize this conversation history concisely but completely.

PRESERVE:
- Key decisions made and their rationale
- Important files, functions, or code discussed
- Current task state and what's being worked on
- User preferences or constraints mentioned
- Any errors encountered and their resolutions

CONVERSATION TO SUMMARIZE:
{conversation_text}

Provide a structured summary that another AI can use to continue the conversation:"""

        result = await self.orchestrator.generate(
            messages=[Message(role="user", content=summary_prompt)],
            force_code_model=False,  # Use reasoning model
            max_tokens=self.config.summary_max_tokens,
            temperature=0.3,  # Lower temperature for factual summary
        )

        return result.content

    def _format_for_summary(self, messages: list[Message]) -> str:
        """Format messages for summarization prompt."""
        lines = []
        for msg in messages:
            role = msg.role.upper()
            # Truncate very long messages
            content = msg.content[:2000] + "..." if len(msg.content) > 2000 else msg.content
            lines.append(f"[{role}]: {content}")
        return "\n\n".join(lines)

    def _format_summary(self, summary: str, message_count: int) -> str:
        """Format the summary as a system message."""
        return f"""[CONVERSATION SUMMARY]
The following summarizes {message_count} previous messages that have been compacted to save context space.

{summary}

[END SUMMARY - Recent conversation continues below]"""

    async def _save_pre_compaction_snapshot(
        self,
        conversation_id: str,
        messages: list[Message],
    ) -> None:
        """Save full conversation before compaction for history."""
        await self.storage.save_compaction_snapshot(
            conversation_id=conversation_id,
            messages=messages,
            timestamp=datetime.utcnow(),
        )
```

### 3. Integration with Agentic Loop

```python
class AgentEngine:
    """Agentic loop with auto-compaction support."""

    async def process_turn(self, user_message: str) -> AsyncIterator[str]:
        # Add user message
        self.messages.append(Message(role="user", content=user_message))

        # Check for compaction BEFORE generating
        self.messages, result = await self.compaction_manager.check_and_compact(
            self.conversation_id,
            self.messages,
        )

        if result.compacted and self.config.compaction.notify_user:
            yield f"\n[Context compacted: {result.old_token_count} → {result.new_token_count} tokens]\n"

        # Continue with normal generation
        async for chunk in self._generate_response():
            yield chunk
```

---

## Database Schema Addition

```sql
-- Compaction snapshots for history recovery
CREATE TABLE compaction_snapshots (
    id TEXT PRIMARY KEY,
    conversation_id TEXT REFERENCES conversations(id),
    messages JSON NOT NULL,          -- Full message list before compaction
    token_count INTEGER,
    summary TEXT,                     -- The generated summary
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX idx_compaction_conversation ON compaction_snapshots(conversation_id);
```

---

## UI Integration

### Status Bar Token Display

```python
class StatusBar:
    """Status bar with token usage indicator."""

    def render(self) -> RenderableType:
        # Token usage with color coding
        usage_percent = self.token_count / self.max_tokens * 100

        if usage_percent > 90:
            token_color = "red"
            token_warning = "⚠"
        elif usage_percent > 75:
            token_color = "yellow"
            token_warning = ""
        else:
            token_color = "green"
            token_warning = ""

        token_display = f"[{token_color}]{token_warning}Tokens: {self.token_count:,}/{self.max_tokens:,} ({usage_percent:.0f}%)[/]"
```

### Compaction Notification

```python
async def show_compaction_notification(result: CompactionResult) -> None:
    """Show notification when compaction occurs."""
    panel = Panel(
        f"Compacted conversation to save context space\n"
        f"  Before: {result.old_token_count:,} tokens\n"
        f"  After:  {result.new_token_count:,} tokens\n"
        f"  Preserved: {result.preserved_messages} recent messages\n"
        f"  Full history saved to database",
        title="[yellow]📦 Auto-Compaction[/]",
        border_style="yellow",
    )
    console.print(panel)
```

---

## Commands

```
/compact              Force compaction now (even if under threshold)
/compact status       Show current token usage and threshold
/compact history      View compaction history for this session
/compact restore N    Restore pre-compaction snapshot N
```

---

## Testing

### Unit Tests

```python
class TestCompactionManager:
    async def test_no_compaction_under_threshold(self):
        """Should not compact when under threshold."""
        manager = CompactionManager(config, counter, orchestrator, storage)
        messages = [Message(role="user", content="Hello")]

        result_messages, result = await manager.check_and_compact("conv1", messages)

        assert result.compacted is False
        assert result_messages == messages

    async def test_compaction_preserves_recent(self):
        """Should preserve recent messages."""
        config.preserve_recent_turns = 2
        messages = generate_messages(20)  # 20 messages to trigger compaction

        result_messages, result = await manager.check_and_compact("conv1", messages)

        assert result.compacted is True
        assert len(result_messages) == 5  # 1 summary + 4 preserved (2 turns)
        assert result_messages[-4:] == messages[-4:]  # Last 4 preserved

    async def test_summary_quality(self):
        """Summary should capture key information."""
        messages = [
            Message(role="user", content="Let's build a chess game"),
            Message(role="assistant", content="I'll create board.py first"),
            Message(role="user", content="Use algebraic notation"),
            # ... more messages
        ]

        _, result = await manager.check_and_compact("conv1", messages)

        assert "chess" in result.summary.lower()
        assert "algebraic notation" in result.summary.lower()
```

---

## Rollout Plan

1. **Phase 1**: Implement token counting and threshold detection
2. **Phase 2**: Add summarization with micro model
3. **Phase 3**: Add snapshot storage for recovery
4. **Phase 4**: Add UI notifications and status bar integration
5. **Phase 5**: Add `/compact` commands

---

## Future Enhancements

- **Smart preservation**: Preserve messages containing code blocks or decisions
- **Incremental summarization**: Summarize in chunks rather than all at once
- **Summary quality validation**: Check summary captures key points
- **User-guided compaction**: Let user mark messages as "important" to preserve
