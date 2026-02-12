"""Tests for context compaction system."""

import json

import pytest
from entropi.config.schema import CompactionConfig
from entropi.core.base import Message
from entropi.core.compaction import CompactionManager, CompactionResult, TokenCounter


class TestTokenCounter:
    """Tests for TokenCounter class."""

    def test_token_counter_basic(self) -> None:
        """Verify token counting heuristic (~4 chars per token)."""
        counter = TokenCounter(max_tokens=1000)

        # "Hello world" = 11 chars -> ~3 tokens + 1 = 4
        msg = Message(role="user", content="Hello world")
        count = counter.count_message(msg)

        # 11 // 4 + 1 = 3 (content) + 4 (role overhead) = 7
        assert count == 7

    def test_token_counter_with_tool_calls(self) -> None:
        """Tokens include tool calls overhead."""
        counter = TokenCounter(max_tokens=1000)

        tool_call = {"name": "read_file", "arguments": {"path": "test.py"}}
        msg = Message(
            role="assistant",
            content="Let me read that file.",
            tool_calls=[tool_call],
        )

        count_with_tools = counter.count_message(msg)

        # Compare to message without tool calls
        msg_no_tools = Message(role="assistant", content="Let me read that file.")
        count_no_tools = counter.count_message(msg_no_tools)

        assert count_with_tools > count_no_tools

    def test_token_counter_with_tool_results(self) -> None:
        """Tokens include tool results overhead."""
        counter = TokenCounter(max_tokens=1000)

        tool_result = {"name": "read_file", "result": "file contents here"}
        msg = Message(
            role="user",
            content="Here are the results:",
            tool_results=[tool_result],
        )

        count_with_results = counter.count_message(msg)

        msg_no_results = Message(role="user", content="Here are the results:")
        count_no_results = counter.count_message(msg_no_results)

        assert count_with_results > count_no_results

    def test_token_counter_caching(self) -> None:
        """Token counts are cached by message identity."""
        counter = TokenCounter(max_tokens=1000)

        msg = Message(role="user", content="Hello world")

        # First call computes and caches
        count1 = counter.count_message(msg)
        # Second call uses cache
        count2 = counter.count_message(msg)

        assert count1 == count2
        assert id(msg) in counter._cache

    def test_count_messages_sums_all(self) -> None:
        """count_messages sums all message tokens."""
        counter = TokenCounter(max_tokens=1000)

        messages = [
            Message(role="system", content="You are helpful."),
            Message(role="user", content="Hello"),
            Message(role="assistant", content="Hi there!"),
        ]

        total = counter.count_messages(messages)
        individual_sum = sum(counter.count_message(m) for m in messages)

        assert total == individual_sum

    def test_usage_percent(self) -> None:
        """usage_percent returns correct percentage."""
        counter = TokenCounter(max_tokens=100)

        # Create message that uses ~25 tokens
        msg = Message(role="user", content="a" * 80)  # ~20 content + 4 role = 24
        messages = [msg]

        usage = counter.usage_percent(messages)
        assert 0.2 <= usage <= 0.3  # ~24%

    def test_usage_percent_zero_max(self) -> None:
        """usage_percent handles zero max_tokens."""
        counter = TokenCounter(max_tokens=0)

        messages = [Message(role="user", content="Hello")]
        usage = counter.usage_percent(messages)

        assert usage == 0.0

    def test_clear_cache(self) -> None:
        """clear_cache removes all cached counts."""
        counter = TokenCounter(max_tokens=1000)

        msg = Message(role="user", content="Hello")
        counter.count_message(msg)
        assert len(counter._cache) > 0

        counter.clear_cache()
        assert len(counter._cache) == 0


class TestCompactionManager:
    """Tests for CompactionManager class."""

    def setup_method(self) -> None:
        """Set up test fixtures."""
        self.config = CompactionConfig(
            enabled=True,
            threshold_percent=0.75,
            preserve_recent_turns=2,
            summary_max_tokens=500,
        )
        self.counter = TokenCounter(max_tokens=1000)

    def _create_messages(self, count: int, content_size: int = 100) -> list[Message]:
        """Helper to create test messages."""
        messages = [Message(role="system", content="You are helpful.")]
        for i in range(count):
            role = "user" if i % 2 == 0 else "assistant"
            messages.append(Message(role=role, content="x" * content_size))
        return messages

    @pytest.mark.asyncio
    async def test_check_below_threshold_no_compact(self) -> None:
        """Below threshold -> no compaction."""
        manager = CompactionManager(self.config, self.counter)

        # Small message list - well under threshold
        messages = [
            Message(role="system", content="You are helpful."),
            Message(role="user", content="Hello"),
            Message(role="assistant", content="Hi!"),
        ]

        result_messages, result = await manager.check_and_compact("conv-1", messages)

        assert result.compacted is False
        assert result.old_token_count == result.new_token_count
        assert result_messages == messages

    @pytest.mark.asyncio
    async def test_check_at_threshold_triggers_compact(self) -> None:
        """At threshold -> triggers compaction."""
        manager = CompactionManager(self.config, self.counter)

        # Create messages that exceed 75% of 1000 tokens
        # Each message is ~30 tokens (100 chars / 4 + 1 + 4 role = ~29)
        # Need ~25+ messages to hit 750 tokens
        messages = self._create_messages(30, content_size=100)

        # Verify we're above threshold
        usage = self.counter.usage_percent(messages)
        assert usage >= 0.75

        result_messages, result = await manager.check_and_compact("conv-1", messages)

        assert result.compacted is True
        assert result.new_token_count < result.old_token_count

    @pytest.mark.asyncio
    async def test_compact_preserves_system_message(self) -> None:
        """System message always preserved."""
        manager = CompactionManager(self.config, self.counter)

        messages = self._create_messages(30, content_size=100)
        system_content = messages[0].content

        result_messages, result = await manager.check_and_compact("conv-1", messages)

        assert result.compacted is True
        assert result_messages[0].role == "system"
        assert result_messages[0].content == system_content

    @pytest.mark.asyncio
    async def test_compact_preserves_recent_turns(self) -> None:
        """Recent messages kept per config (preserve_recent_turns)."""
        manager = CompactionManager(self.config, self.counter)

        messages = self._create_messages(30, content_size=100)
        # Mark the last few messages with unique content
        messages[-1] = Message(role="assistant", content="LAST_ASSISTANT_MESSAGE")
        messages[-2] = Message(role="user", content="LAST_USER_MESSAGE")

        result_messages, result = await manager.check_and_compact("conv-1", messages)

        assert result.compacted is True
        # Last 2 messages should be preserved (but may be more depending on budget)
        message_contents = [m.content for m in result_messages]
        assert "LAST_ASSISTANT_MESSAGE" in message_contents
        assert "LAST_USER_MESSAGE" in message_contents

    @pytest.mark.asyncio
    async def test_compact_uses_structured_summary(self) -> None:
        """Compaction produces structured summary (not model-generated)."""
        # No orchestrator needed — structured summary is deterministic
        manager = CompactionManager(self.config, self.counter, orchestrator=None)

        messages = self._create_messages(30, content_size=100)
        # Set first user message to a recognizable task
        messages[1] = Message(role="user", content="Analyze the error handling")

        result_messages, result = await manager.check_and_compact("conv-1", messages)

        assert result.compacted is True
        # Summary message should contain structured format
        summary_msg = result_messages[1]  # After system message
        assert "[CONVERSATION SUMMARY]" in summary_msg.content
        assert "Original task: Analyze the error handling" in summary_msg.content

    @pytest.mark.asyncio
    async def test_aggressive_compact_structure(self) -> None:
        """Aggressive compact: system + structured summary + last 2 messages."""
        manager = CompactionManager(self.config, self.counter)

        messages = self._create_messages(10, content_size=100)
        messages[1] = Message(role="user", content="Original task here")
        messages[-1] = Message(role="assistant", content="LAST")
        messages[-2] = Message(role="user", content="SECOND_LAST")

        result = manager._aggressive_compact(messages)

        # system + summary + last 2 = 4 messages
        assert len(result) == 4
        assert result[0].role == "system"
        assert "[CONVERSATION SUMMARY]" in result[1].content
        assert "Original task: Original task here" in result[1].content
        assert result[2].content == "SECOND_LAST"
        assert result[3].content == "LAST"

    @pytest.mark.asyncio
    async def test_forced_compaction_bypasses_threshold(self) -> None:
        """force=True compacts even when under threshold."""
        manager = CompactionManager(self.config, self.counter)

        # Small message list - well under 75% threshold
        messages = [
            Message(role="system", content="You are helpful."),
            Message(role="user", content="x" * 400),
            Message(role="assistant", content="y" * 400),
            Message(role="user", content="z" * 400),
            Message(role="assistant", content="w" * 400),
        ]

        # Verify we're under threshold
        usage = self.counter.usage_percent(messages)
        assert usage < 0.75

        # Without force: no compaction
        _, result_normal = await manager.check_and_compact("conv-1", messages)
        assert result_normal.compacted is False

        # With force: compaction happens
        _, result_forced = await manager.check_and_compact("conv-1", messages, force=True)
        assert result_forced.compacted is True
        assert result_forced.new_token_count < result_forced.old_token_count

    @pytest.mark.asyncio
    async def test_compaction_disabled(self) -> None:
        """Compaction disabled in config -> no compaction even at threshold."""
        disabled_config = CompactionConfig(
            enabled=False,
            threshold_percent=0.75,
        )
        manager = CompactionManager(disabled_config, self.counter)

        messages = self._create_messages(30, content_size=100)

        result_messages, result = await manager.check_and_compact("conv-1", messages)

        assert result.compacted is False
        assert result_messages == messages


class TestCompactionResult:
    """Tests for CompactionResult dataclass."""

    def test_compaction_result_not_compacted(self) -> None:
        """Test result when no compaction occurred."""
        result = CompactionResult(
            compacted=False,
            old_token_count=500,
            new_token_count=500,
        )

        assert result.compacted is False
        assert result.summary is None
        assert result.preserved_messages == 0
        assert result.messages_summarized == 0

    def test_compaction_result_compacted(self) -> None:
        """Test result when compaction occurred."""
        result = CompactionResult(
            compacted=True,
            old_token_count=1000,
            new_token_count=400,
            summary="Summary of conversation",
            preserved_messages=5,
            messages_summarized=20,
        )

        assert result.compacted is True
        assert result.old_token_count == 1000
        assert result.new_token_count == 400
        assert result.summary == "Summary of conversation"
        assert result.preserved_messages == 5
        assert result.messages_summarized == 20


class TestStructuredSummary:
    """Tests for deterministic structured summary extraction."""

    def setup_method(self) -> None:
        """Set up test fixtures."""
        self.config = CompactionConfig(enabled=True)
        self.counter = TokenCounter(max_tokens=1000)
        self.manager = CompactionManager(self.config, self.counter)

    def test_extract_original_task(self) -> None:
        """First non-system user message extracted as original task."""
        messages = [
            Message(role="user", content="Analyze the error handling in base.py"),
            Message(role="assistant", content="I'll look at that."),
        ]

        result = self.manager._extract_original_task(messages)
        assert result == "Analyze the error handling in base.py"

    def test_extract_original_task_truncates_long(self) -> None:
        """Messages over 500 chars are truncated."""
        long_content = "a" * 600
        messages = [Message(role="user", content=long_content)]

        result = self.manager._extract_original_task(messages)
        assert len(result) == 503  # 500 + "..."
        assert result.endswith("...")

    def test_extract_original_task_skips_summary(self) -> None:
        """Messages starting with '[' (summaries) are skipped."""
        messages = [
            Message(role="user", content="[CONVERSATION SUMMARY] old stuff"),
            Message(role="user", content="The real task"),
        ]

        result = self.manager._extract_original_task(messages)
        assert result == "The real task"

    def test_extract_original_task_no_user_message(self) -> None:
        """Returns fallback when no user message found."""
        messages = [Message(role="assistant", content="Hello")]

        result = self.manager._extract_original_task(messages)
        assert result == "(no user message found)"

    def test_extract_tool_log(self) -> None:
        """Tool results extracted with name and brief result."""
        messages = [
            Message(
                role="tool",
                content="Tool result",
                tool_results=[
                    {
                        "name": "bash.execute",
                        "result": "Command output line 1\nline 2\nline 3",
                    }
                ],
            ),
            Message(
                role="tool",
                content="Tool result",
                tool_results=[
                    {
                        "name": "filesystem.read_file",
                        "result": "File contents here",
                    }
                ],
            ),
        ]

        log = self.manager._extract_tool_log(messages)

        assert len(log) == 2
        assert log[0] == ("bash.execute", "Command output line 1")
        assert log[1] == ("filesystem.read_file", "File contents here")

    def test_extract_tool_log_pruned_messages(self) -> None:
        """Pruned tool results logged as '(pruned)'."""
        messages = [
            Message(
                role="tool",
                content="[Previous: filesystem.read_file result -- 5000 chars, pruned]",
                tool_results=[{"name": "filesystem.read_file", "result": ""}],
                metadata={"tool_name": "filesystem.read_file"},
            ),
        ]

        log = self.manager._extract_tool_log(messages)

        assert len(log) == 1
        assert log[0] == ("filesystem.read_file", "(pruned)")

    def test_extract_tool_log_falls_back_to_metadata(self) -> None:
        """When tool result has no name, falls back to metadata tool_name."""
        messages = [
            Message(
                role="tool",
                content="Tool result",
                tool_results=[{"result": "some output"}],
                metadata={"tool_name": "bash.execute"},
            ),
        ]

        log = self.manager._extract_tool_log(messages)

        assert len(log) == 1
        assert log[0][0] == "bash.execute"

    def test_extract_tool_log_brief_truncated(self) -> None:
        """Brief result truncated to 100 chars of first line."""
        long_result = "x" * 200
        messages = [
            Message(
                role="tool",
                content="Tool result",
                tool_results=[{"name": "test.tool", "result": long_result}],
            ),
        ]

        log = self.manager._extract_tool_log(messages)

        assert len(log) == 1
        assert len(log[0][1]) == 100

    def test_extract_files_touched(self) -> None:
        """Filesystem tool results parsed for file paths."""
        read_result = json.dumps({"path": "/workspace/src/base.py"})
        write_result = json.dumps({"path": "/workspace/src/engine.py"})
        messages = [
            Message(
                role="tool",
                content="Tool result",
                tool_results=[
                    {"name": "filesystem.read_file", "result": read_result},
                ],
            ),
            Message(
                role="tool",
                content="Tool result",
                tool_results=[
                    {"name": "filesystem.write_file", "result": write_result},
                ],
            ),
        ]

        files_read, files_modified = self.manager._extract_files_touched(messages)

        assert files_read == ["/workspace/src/base.py"]
        assert files_modified == ["/workspace/src/engine.py"]

    def test_extract_files_touched_deduplicates(self) -> None:
        """Same file path not listed twice."""
        read_result = json.dumps({"path": "/workspace/src/base.py"})
        messages = [
            Message(
                role="tool",
                content="Tool result",
                tool_results=[
                    {"name": "filesystem.read_file", "result": read_result},
                ],
            ),
            Message(
                role="tool",
                content="Tool result",
                tool_results=[
                    {"name": "filesystem.read_file", "result": read_result},
                ],
            ),
        ]

        files_read, _ = self.manager._extract_files_touched(messages)
        assert files_read == ["/workspace/src/base.py"]

    def test_extract_files_ignores_non_filesystem(self) -> None:
        """Non-filesystem tools don't contribute to files touched."""
        messages = [
            Message(
                role="tool",
                content="Tool result",
                tool_results=[
                    {"name": "bash.execute", "result": "output"},
                ],
            ),
        ]

        files_read, files_modified = self.manager._extract_files_touched(messages)
        assert files_read == []
        assert files_modified == []

    def test_structured_summary_complete(self) -> None:
        """Full structured summary includes task, tools, and files."""
        read_result = json.dumps({"path": "/workspace/src/base.py"})
        messages = [
            Message(role="user", content="Analyze error handling"),
            Message(role="assistant", content="I'll check that."),
            Message(
                role="tool",
                content="Tool result",
                tool_results=[
                    {"name": "filesystem.read_file", "result": read_result},
                ],
            ),
            Message(
                role="tool",
                content="Tool result",
                tool_results=[
                    {"name": "bash.execute", "result": "ls output"},
                ],
            ),
        ]

        summary = self.manager._structured_summary(messages)

        assert "Original task: Analyze error handling" in summary
        assert "Tool calls made (oldest first):" in summary
        assert "- filesystem.read_file:" in summary
        assert "- bash.execute: ls output" in summary
        assert "Files read: /workspace/src/base.py" in summary

    def test_structured_summary_no_tools(self) -> None:
        """Summary with no tool calls only includes original task."""
        messages = [
            Message(role="user", content="Hello world"),
            Message(role="assistant", content="Hi there!"),
        ]

        summary = self.manager._structured_summary(messages)

        assert summary == "Original task: Hello world"
        assert "Tool calls" not in summary
        assert "Files" not in summary

    def test_parse_path_from_result_valid(self) -> None:
        """Valid JSON with 'path' key extracted."""
        result = json.dumps({"path": "/workspace/test.py", "content": "..."})
        assert CompactionManager._parse_path_from_result(result) == "/workspace/test.py"

    def test_parse_path_from_result_invalid_json(self) -> None:
        """Invalid JSON returns None."""
        assert CompactionManager._parse_path_from_result("not json") is None

    def test_parse_path_from_result_no_path_key(self) -> None:
        """JSON without 'path' key returns None."""
        result = json.dumps({"output": "something"})
        assert CompactionManager._parse_path_from_result(result) is None


class TestCompressForHandoff:
    """Tests for tier handoff compression."""

    def setup_method(self) -> None:
        """Set up test fixtures."""
        self.config = CompactionConfig(enabled=True)
        self.counter = TokenCounter(max_tokens=1000)
        self.manager = CompactionManager(self.config, self.counter)

    @pytest.mark.asyncio
    async def test_handoff_includes_reason(self) -> None:
        """Handoff compression includes the handoff reason."""
        messages = [
            Message(role="system", content="System prompt"),
            Message(role="user", content="Build a feature"),
            Message(role="assistant", content="Planning..."),
        ]

        result = await self.manager.compress_for_handoff(
            messages, "plan_ready", target_context_length=4000
        )

        handoff_msg = result[1]  # After system
        assert "[HANDOFF CONTEXT]" in handoff_msg.content
        assert "Handoff reason: plan_ready" in handoff_msg.content
        assert "Original task: Build a feature" in handoff_msg.content

    @pytest.mark.asyncio
    async def test_handoff_preserves_system(self) -> None:
        """System message preserved in handoff compression."""
        messages = [
            Message(role="system", content="System prompt"),
            Message(role="user", content="Task"),
        ]

        result = await self.manager.compress_for_handoff(
            messages, "escalation", target_context_length=4000
        )

        assert result[0].role == "system"
        assert result[0].content == "System prompt"

    @pytest.mark.asyncio
    async def test_handoff_keeps_recent_if_fits(self) -> None:
        """Recent messages preserved if they fit in target budget."""
        messages = [
            Message(role="system", content="System prompt"),
            Message(role="user", content="Short task"),
            Message(role="assistant", content="Short reply"),
            Message(role="user", content="RECENT_USER"),
            Message(role="assistant", content="RECENT_ASSISTANT"),
        ]

        result = await self.manager.compress_for_handoff(
            messages, "reason", target_context_length=4000
        )

        contents = [m.content for m in result]
        assert "RECENT_USER" in contents
        assert "RECENT_ASSISTANT" in contents
