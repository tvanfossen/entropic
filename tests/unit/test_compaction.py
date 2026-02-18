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


def _user_msg(content: str) -> Message:
    """Create a tagged user message (simulates real user input)."""
    msg = Message(role="user", content=content)
    msg.metadata["source"] = "user"
    return msg


def _tool_result_msg(tool_name: str, content: str, result: str = "") -> Message:
    """Create a tool result message (simulates adapter format_tool_result)."""
    msg = Message(
        role="user",
        content=content,
        tool_results=[{"name": tool_name, "result": result}],
    )
    msg.metadata["tool_name"] = tool_name
    return msg


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

    @pytest.mark.asyncio
    async def test_check_below_threshold_no_compact(self) -> None:
        """Below threshold -> no compaction."""
        manager = CompactionManager(self.config, self.counter)

        # Small message list - well under threshold
        messages = [
            Message(role="system", content="You are helpful."),
            _user_msg("Hello"),
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

        # Build messages that exceed 75% of 1000 tokens
        messages = [Message(role="system", content="You are helpful.")]
        messages.append(_user_msg("Analyze error handling"))
        for _ in range(15):
            messages.append(Message(role="assistant", content="x" * 100))
            messages.append(_tool_result_msg("bash.execute", "y" * 100, "output"))

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

        messages = [Message(role="system", content="You are helpful.")]
        messages.append(_user_msg("Task"))
        for _ in range(15):
            messages.append(Message(role="assistant", content="x" * 100))
            messages.append(_tool_result_msg("bash.execute", "y" * 100))

        result_messages, result = await manager.check_and_compact("conv-1", messages)

        assert result.compacted is True
        assert result_messages[0].role == "system"
        assert result_messages[0].content == "You are helpful."

    @pytest.mark.asyncio
    async def test_compact_uses_structured_summary(self) -> None:
        """Compaction produces structured summary (not model-generated)."""
        manager = CompactionManager(self.config, self.counter, orchestrator=None)

        messages = [Message(role="system", content="System")]
        messages.append(_user_msg("Analyze the error handling"))
        for _ in range(15):
            messages.append(Message(role="assistant", content="x" * 100))
            messages.append(_tool_result_msg("bash.execute", "y" * 100))

        result_messages, result = await manager.check_and_compact("conv-1", messages)

        assert result.compacted is True
        # Summary message should contain structured format
        summary_msg = result_messages[1]  # After system message
        assert "[CONVERSATION SUMMARY]" in summary_msg.content
        assert "Original task: Analyze the error handling" in summary_msg.content

    @pytest.mark.asyncio
    async def test_forced_compaction_bypasses_threshold(self) -> None:
        """force=True compacts even when under threshold."""
        manager = CompactionManager(self.config, self.counter)

        messages = [
            Message(role="system", content="System"),
            _user_msg("Task here"),
            Message(role="assistant", content="y" * 400),
            _tool_result_msg("bash.execute", "z" * 400, "output"),
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

        messages = [Message(role="system", content="System")]
        messages.append(_user_msg("Task"))
        for _ in range(15):
            messages.append(Message(role="assistant", content="x" * 100))
            messages.append(_tool_result_msg("bash.execute", "y" * 100))

        result_messages, result = await manager.check_and_compact("conv-1", messages)

        assert result.compacted is False
        assert result_messages == messages


class TestValueDensityCompaction:
    """Tests for value-density compaction behavior."""

    def setup_method(self) -> None:
        """Set up test fixtures."""
        self.config = CompactionConfig(
            enabled=True, threshold_percent=0.5, warning_threshold_percent=0.3
        )
        self.counter = TokenCounter(max_tokens=1000)
        self.manager = CompactionManager(self.config, self.counter)

    @pytest.mark.asyncio
    async def test_user_messages_always_survive(self) -> None:
        """Real user messages (source=user) are never stripped."""
        messages = [
            Message(role="system", content="System"),
            _user_msg("First task"),
            Message(role="assistant", content="x" * 200),
            _tool_result_msg("filesystem.read_file", "y" * 500, "big file"),
            Message(role="assistant", content="z" * 200),
            _user_msg("Follow-up question"),
            Message(role="assistant", content="w" * 200),
            _tool_result_msg("bash.execute", "v" * 500, "output"),
        ]

        result_msgs, result = await self.manager.check_and_compact("conv-1", messages, force=True)

        contents = [m.content for m in result_msgs]
        assert "First task" in contents
        assert "Follow-up question" in contents

    @pytest.mark.asyncio
    async def test_tool_results_stripped(self) -> None:
        """Tool result messages are removed during compaction."""
        messages = [
            Message(role="system", content="System"),
            _user_msg("Task"),
            Message(role="assistant", content="checking..."),
            _tool_result_msg("filesystem.read_file", "x" * 500, "big content"),
            Message(role="assistant", content="done"),
        ]

        result_msgs, _ = await self.manager.check_and_compact("conv-1", messages, force=True)

        # No message should contain the large tool result content
        for msg in result_msgs:
            assert "x" * 500 not in msg.content

    @pytest.mark.asyncio
    async def test_last_assistant_kept(self) -> None:
        """Most recent assistant message is preserved."""
        messages = [
            Message(role="system", content="System"),
            _user_msg("Task"),
            Message(role="assistant", content="OLD_ASSISTANT"),
            _tool_result_msg("bash.execute", "tool output" * 50, "output"),
            Message(role="assistant", content="LAST_ASSISTANT"),
        ]

        result_msgs, _ = await self.manager.check_and_compact("conv-1", messages, force=True)

        contents = [m.content for m in result_msgs]
        assert "LAST_ASSISTANT" in contents
        assert "OLD_ASSISTANT" not in contents

    @pytest.mark.asyncio
    async def test_compaction_never_increases_tokens(self) -> None:
        """Compaction always reduces token count."""
        messages = [
            Message(role="system", content="System prompt " * 20),
            _user_msg("Task"),
            Message(role="assistant", content="a" * 200),
            _tool_result_msg("filesystem.read_file", "b" * 500, "content"),
            Message(role="assistant", content="c" * 200),
            _tool_result_msg("bash.execute", "d" * 300, "output"),
            Message(role="assistant", content="e" * 100),
        ]

        old_tokens = self.counter.count_messages(messages)
        result_msgs, result = await self.manager.check_and_compact("conv-1", messages, force=True)

        assert result.new_token_count < old_tokens

    @pytest.mark.asyncio
    async def test_system_injections_stripped(self) -> None:
        """Context warnings, todo state, and other injections are stripped."""
        messages = [
            Message(role="system", content="System"),
            _user_msg("Task"),
            Message(role="assistant", content="working..."),
            Message(role="user", content="[CONTEXT WARNING] At 80%"),
            Message(role="user", content="[CURRENT TODO STATE]\n..."),
            Message(role="assistant", content="continuing..."),
        ]

        result_msgs, _ = await self.manager.check_and_compact("conv-1", messages, force=True)

        for msg in result_msgs:
            assert "[CONTEXT WARNING]" not in msg.content
            assert "[CURRENT TODO STATE]" not in msg.content

    @pytest.mark.asyncio
    async def test_result_structure(self) -> None:
        """Compacted messages follow: system + summary + user msgs + last assistant."""
        messages = [
            Message(role="system", content="System"),
            _user_msg("Task"),
            Message(role="assistant", content="first"),
            _tool_result_msg("bash.execute", "output" * 50, "out"),
            Message(role="assistant", content="LAST"),
        ]

        result_msgs, _ = await self.manager.check_and_compact("conv-1", messages, force=True)

        assert result_msgs[0].role == "system"
        assert "[CONVERSATION SUMMARY]" in result_msgs[1].content
        assert result_msgs[2].content == "Task"  # User message
        assert result_msgs[3].content == "LAST"  # Last assistant


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

    def test_extract_original_task_with_source_tag(self) -> None:
        """Source-tagged user message extracted as original task."""
        messages = [
            _user_msg("Analyze the error handling in base.py"),
            Message(role="assistant", content="I'll look at that."),
        ]

        result = self.manager._extract_original_task(messages)
        assert result == "Analyze the error handling in base.py"

    def test_extract_original_task_fallback_heuristic(self) -> None:
        """Untagged user message found by heuristic fallback."""
        messages = [
            Message(role="user", content="Analyze the error handling in base.py"),
            Message(role="assistant", content="I'll look at that."),
        ]

        result = self.manager._extract_original_task(messages)
        assert result == "Analyze the error handling in base.py"

    def test_extract_original_task_truncates_long(self) -> None:
        """Messages over 500 chars are truncated."""
        long_content = "a" * 600
        messages = [_user_msg(long_content)]

        result = self.manager._extract_original_task(messages)
        assert len(result) == 503  # 500 + "..."
        assert result.endswith("...")

    def test_extract_original_task_skips_summary(self) -> None:
        """Messages starting with '[' (summaries) are skipped by fallback."""
        messages = [
            Message(role="user", content="[CONVERSATION SUMMARY] old stuff"),
            Message(role="user", content="The real task"),
        ]

        result = self.manager._extract_original_task(messages)
        assert result == "The real task"

    def test_extract_original_task_skips_tool_results(self) -> None:
        """Messages starting with 'Tool ' are skipped by fallback."""
        messages = [
            Message(role="user", content="Tool `bash.execute` returned:\noutput"),
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
            _user_msg("Analyze error handling"),
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
            _user_msg("Hello world"),
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
            _user_msg("Build a feature"),
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
            _user_msg("Task"),
        ]

        result = await self.manager.compress_for_handoff(
            messages, "escalation", target_context_length=4000
        )

        assert result[0].role == "system"
        assert result[0].content == "System prompt"

    @pytest.mark.asyncio
    async def test_handoff_keeps_user_messages(self) -> None:
        """User messages preserved, tool results stripped in handoff."""
        messages = [
            Message(role="system", content="System prompt"),
            _user_msg("First task"),
            Message(role="assistant", content="working..."),
            _tool_result_msg("filesystem.read_file", "big content" * 50, "data"),
            Message(role="assistant", content="done"),
            _user_msg("Follow-up"),
        ]

        result = await self.manager.compress_for_handoff(
            messages, "reason", target_context_length=4000
        )

        contents = [m.content for m in result]
        assert "First task" in contents
        assert "Follow-up" in contents
        # Tool result content should not appear
        for msg in result:
            assert "big content" * 50 not in msg.content
