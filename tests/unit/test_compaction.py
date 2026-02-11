"""Tests for context compaction system."""

from unittest.mock import AsyncMock, MagicMock

import pytest
from entropi.config.schema import CompactionConfig
from entropi.core.base import GenerationResult, Message
from entropi.core.compaction import CompactionManager, CompactionResult, TokenCounter
from entropi.inference.orchestrator import ModelTier


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
        """Below threshold → no compaction."""
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
        """At threshold → triggers compaction."""
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
    async def test_simple_summary_fallback(self) -> None:
        """No orchestrator → _simple_summary() is used."""
        # Manager without orchestrator
        manager = CompactionManager(self.config, self.counter, orchestrator=None)

        messages = self._create_messages(30, content_size=100)

        result_messages, result = await manager.check_and_compact("conv-1", messages)

        assert result.compacted is True
        # Summary should contain "[Summary of X messages]"
        summary_msg = result_messages[1]  # After system message
        assert "[Summary of" in summary_msg.content

    @pytest.mark.asyncio
    async def test_aggressive_compact_fallback(self) -> None:
        """<20% reduction → aggressive fallback."""
        # Create a mock orchestrator that returns a summary as long as input
        mock_orchestrator = MagicMock()

        async def mock_generate(messages, **kwargs):
            # Return a summary that's intentionally too long
            return GenerationResult(content="x" * 10000, finish_reason="stop")

        mock_orchestrator.generate = mock_generate

        manager = CompactionManager(self.config, self.counter, orchestrator=mock_orchestrator)

        messages = self._create_messages(30, content_size=100)
        original_count = len(messages)

        result_messages, result = await manager.check_and_compact("conv-1", messages)

        assert result.compacted is True
        # Aggressive compact keeps system + last 2 messages
        # Result should be much smaller than original
        assert len(result_messages) < original_count

    @pytest.mark.asyncio
    async def test_aggressive_compact_keeps_system_plus_two(self) -> None:
        """Aggressive fallback keeps system + last 2 messages."""
        manager = CompactionManager(self.config, self.counter)

        messages = self._create_messages(10, content_size=100)
        messages[-1] = Message(role="assistant", content="LAST")
        messages[-2] = Message(role="user", content="SECOND_LAST")

        result = manager._aggressive_compact(messages)

        # Should be system + last 2 = 3 messages
        assert len(result) == 3
        assert result[0].role == "system"
        assert result[1].content == "SECOND_LAST"
        assert result[2].content == "LAST"

    @pytest.mark.asyncio
    async def test_generate_summary_exception_fallback(self) -> None:
        """Exception during summary generation → graceful fallback."""
        mock_orchestrator = MagicMock()
        mock_orchestrator.generate = AsyncMock(side_effect=Exception("Model error"))

        manager = CompactionManager(self.config, self.counter, orchestrator=mock_orchestrator)

        messages = self._create_messages(30, content_size=100)

        result_messages, result = await manager.check_and_compact("conv-1", messages)

        # Should still compact using simple summary fallback
        assert result.compacted is True
        summary_msg = result_messages[1]
        assert "[Summary of" in summary_msg.content

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
        """Compaction disabled in config → no compaction even at threshold."""
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


class TestCompactionTierSelection:
    """Tests that compaction uses the currently-active model tier."""

    def _make_orchestrator(self, last_tier: ModelTier | None) -> MagicMock:
        """Create a mock orchestrator with a given last_used_tier."""
        orch = MagicMock()
        orch.last_used_tier = last_tier

        async def mock_generate(messages, **kwargs):
            return GenerationResult(content="Summary.", finish_reason="stop")

        orch.generate = AsyncMock(side_effect=mock_generate)
        return orch

    @pytest.mark.asyncio
    async def test_summary_uses_current_tier(self) -> None:
        """_generate_summary passes last_used_tier to orchestrator."""
        orch = self._make_orchestrator(ModelTier.THINKING)
        config = CompactionConfig(enabled=True)
        counter = TokenCounter(max_tokens=1000)
        manager = CompactionManager(config, counter, orchestrator=orch)

        messages = [Message(role="user", content="Hello")]
        await manager._generate_summary(messages)

        orch.generate.assert_called_once()
        _, kwargs = orch.generate.call_args
        assert kwargs["tier"] == ModelTier.THINKING

    @pytest.mark.asyncio
    async def test_summary_falls_back_to_normal(self) -> None:
        """_generate_summary uses NORMAL when no tier has been used."""
        orch = self._make_orchestrator(None)
        config = CompactionConfig(enabled=True)
        counter = TokenCounter(max_tokens=1000)
        manager = CompactionManager(config, counter, orchestrator=orch)

        messages = [Message(role="user", content="Hello")]
        await manager._generate_summary(messages)

        orch.generate.assert_called_once()
        _, kwargs = orch.generate.call_args
        assert kwargs["tier"] == ModelTier.NORMAL

    @pytest.mark.asyncio
    async def test_handoff_summary_uses_current_tier(self) -> None:
        """_generate_handoff_summary passes last_used_tier to orchestrator."""
        orch = self._make_orchestrator(ModelTier.THINKING)
        config = CompactionConfig(enabled=True)
        counter = TokenCounter(max_tokens=1000)
        manager = CompactionManager(config, counter, orchestrator=orch)

        messages = [Message(role="user", content="Hello")]
        await manager._generate_handoff_summary(messages, "plan_ready", 500)

        orch.generate.assert_called_once()
        _, kwargs = orch.generate.call_args
        assert kwargs["tier"] == ModelTier.THINKING
