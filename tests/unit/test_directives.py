"""Tests for directive processing — tool-to-engine communication protocol."""

from __future__ import annotations

import json
from dataclasses import dataclass

import pytest
from entropi.core.base import Message
from entropi.core.directives import (
    _DIRECTIVE_REGISTRY,
    ClearSelfTodos,
    Directive,
    DirectiveProcessor,
    DirectiveResult,
    InjectContext,
    PruneMessages,
    StopProcessing,
    TierChange,
    TodoStateChanged,
    deserialize_directive,
    extract_directives,
)


def _make_ctx() -> object:
    """Create a minimal LoopContext for testing."""
    from entropi.core.engine import LoopContext

    return LoopContext()


class TestExtractDirectives:
    """extract_directives deserializes _directives from JSON tool results."""

    def test_extracts_stop_processing(self) -> None:
        """Valid JSON with _directives returns typed directive."""
        content = json.dumps(
            {
                "result": "ok",
                "_directives": [{"type": "stop_processing"}],
            }
        )
        result = extract_directives(content)
        assert len(result) == 1
        assert isinstance(result[0], StopProcessing)

    def test_returns_empty_for_no_directives(self) -> None:
        """JSON without _directives returns empty list."""
        content = json.dumps({"result": "ok"})
        assert extract_directives(content) == []

    def test_returns_empty_for_non_json(self) -> None:
        """Non-JSON string returns empty list (no crash)."""
        assert extract_directives("plain text result") == []

    def test_returns_empty_for_empty_string(self) -> None:
        assert extract_directives("") == []

    def test_returns_empty_for_json_array(self) -> None:
        """JSON array (not object) returns empty list."""
        assert extract_directives(json.dumps([1, 2, 3])) == []

    def test_returns_empty_for_non_list_directives(self) -> None:
        """_directives that isn't a list returns empty."""
        content = json.dumps({"_directives": "not a list"})
        assert extract_directives(content) == []

    def test_multiple_directives_preserved(self) -> None:
        """Multiple directives deserialized in order."""
        directives = [
            {"type": "clear_self_todos"},
            {"type": "tier_change", "params": {"tier": "code"}},
            {"type": "stop_processing"},
        ]
        content = json.dumps({"result": "ok", "_directives": directives})
        result = extract_directives(content)
        assert len(result) == 3
        assert isinstance(result[0], ClearSelfTodos)
        assert isinstance(result[1], TierChange)
        assert result[1].tier == "code"
        assert isinstance(result[2], StopProcessing)

    def test_unknown_directive_skipped(self) -> None:
        """Unknown directive type is skipped with warning, not crash."""
        content = json.dumps(
            {
                "_directives": [
                    {"type": "nonexistent"},
                    {"type": "stop_processing"},
                ]
            }
        )
        result = extract_directives(content)
        assert len(result) == 1
        assert isinstance(result[0], StopProcessing)


class TestDeserializeDirective:
    """deserialize_directive converts raw dicts to typed Directive objects."""

    def test_stop_processing(self) -> None:
        assert deserialize_directive({"type": "stop_processing"}) == StopProcessing()

    def test_tier_change(self) -> None:
        d = deserialize_directive(
            {"type": "tier_change", "params": {"tier": "code", "reason": "test"}}
        )
        assert isinstance(d, TierChange)
        assert d.tier == "code"
        assert d.reason == "test"

    def test_tier_change_defaults(self) -> None:
        d = deserialize_directive({"type": "tier_change", "params": {"tier": "code"}})
        assert isinstance(d, TierChange)
        assert d.reason == ""

    def test_clear_self_todos(self) -> None:
        assert deserialize_directive({"type": "clear_self_todos"}) == ClearSelfTodos()

    def test_inject_context(self) -> None:
        d = deserialize_directive({"type": "inject_context", "params": {"content": "hello"}})
        assert isinstance(d, InjectContext)
        assert d.content == "hello"
        assert d.role == "user"

    def test_prune_messages(self) -> None:
        d = deserialize_directive({"type": "prune_messages", "params": {"keep_recent": 5}})
        assert isinstance(d, PruneMessages)
        assert d.keep_recent == 5

    def test_prune_messages_default(self) -> None:
        d = deserialize_directive({"type": "prune_messages"})
        assert isinstance(d, PruneMessages)
        assert d.keep_recent == 2

    def test_todo_state_changed(self) -> None:
        d = deserialize_directive(
            {
                "type": "todo_state_changed",
                "params": {"state": "todo list", "count": 3},
            }
        )
        assert isinstance(d, TodoStateChanged)
        assert d.state == "todo list"
        assert d.count == 3
        assert d.items is None

    def test_unknown_type_raises(self) -> None:
        with pytest.raises(KeyError, match="nonexistent"):
            deserialize_directive({"type": "nonexistent"})

    def test_missing_type_raises(self) -> None:
        with pytest.raises(KeyError):
            deserialize_directive({})


class TestDirectiveProcessor:
    """DirectiveProcessor dispatches to registered handlers by type."""

    def test_process_empty_list(self) -> None:
        """Empty directive list returns default result."""
        proc = DirectiveProcessor()
        result = proc.process(_make_ctx(), [])
        assert result.stop_processing is False
        assert result.tier_changed is False

    def test_unregistered_directive_ignored(self) -> None:
        """Unregistered directive type logged but doesn't crash."""

        @dataclass
        class UnknownDirective(Directive):
            pass

        proc = DirectiveProcessor()
        result = proc.process(_make_ctx(), [UnknownDirective()])
        assert result.stop_processing is False

    def test_registered_handler_called(self) -> None:
        """Registered handler receives ctx, directive, result."""
        calls = []

        def handler(ctx, directive, result):
            calls.append(directive)

        proc = DirectiveProcessor()
        proc.register(StopProcessing, handler)
        proc.process(_make_ctx(), [StopProcessing()])

        assert len(calls) == 1
        assert isinstance(calls[0], StopProcessing)

    def test_stop_processing_handler(self) -> None:
        """Handler can set stop_processing on result."""

        def stop_handler(ctx, directive, result):
            result.stop_processing = True

        proc = DirectiveProcessor()
        proc.register(StopProcessing, stop_handler)
        result = proc.process(_make_ctx(), [StopProcessing()])
        assert result.stop_processing is True

    def test_tier_change_handler(self) -> None:
        """Handler can set tier_changed on result."""

        def tier_handler(ctx, directive, result):
            result.tier_changed = True

        proc = DirectiveProcessor()
        proc.register(TierChange, tier_handler)
        result = proc.process(
            _make_ctx(),
            [TierChange(tier="code", reason="test")],
        )
        assert result.tier_changed is True

    def test_multiple_directives_processed_in_order(self) -> None:
        """Directives processed sequentially, result accumulates."""
        order = []

        @dataclass
        class DirectiveA(Directive):
            pass

        @dataclass
        class DirectiveB(Directive):
            pass

        def first(ctx, directive, result):
            order.append("first")

        def second(ctx, directive, result):
            order.append("second")
            result.stop_processing = True

        proc = DirectiveProcessor()
        proc.register(DirectiveA, first)
        proc.register(DirectiveB, second)
        result = proc.process(_make_ctx(), [DirectiveA(), DirectiveB()])

        assert order == ["first", "second"]
        assert result.stop_processing is True

    def test_handler_receives_directive_attributes(self) -> None:
        """Handler can access typed directive attributes."""
        received = []

        def handler(ctx, directive, result):
            received.append(directive.content)

        proc = DirectiveProcessor()
        proc.register(InjectContext, handler)
        proc.process(_make_ctx(), [InjectContext(content="hello")])

        assert received == ["hello"]

    def test_registered_types_property(self) -> None:
        """registered_types lists all registered directive types."""
        proc = DirectiveProcessor()
        proc.register(StopProcessing, lambda c, d, r: None)
        proc.register(TierChange, lambda c, d, r: None)
        assert set(proc.registered_types) == {StopProcessing, TierChange}

    def test_inject_context_via_result(self) -> None:
        """Handler can append to injected_messages."""

        def inject_handler(ctx, directive, result):
            result.injected_messages.append(Message(role="user", content=directive.content))

        proc = DirectiveProcessor()
        proc.register(InjectContext, inject_handler)
        result = proc.process(
            _make_ctx(),
            [InjectContext(content="test warning")],
        )
        assert len(result.injected_messages) == 1
        assert result.injected_messages[0].content == "test warning"


class TestDirectiveResult:
    """DirectiveResult dataclass defaults."""

    def test_defaults(self) -> None:
        result = DirectiveResult()
        assert result.stop_processing is False
        assert result.tier_changed is False
        assert result.injected_messages == []


class TestDirectiveRegistry:
    """Directive registry maps string type names to dataclass types."""

    def test_all_six_types_registered(self) -> None:
        expected = {
            "stop_processing": StopProcessing,
            "tier_change": TierChange,
            "clear_self_todos": ClearSelfTodos,
            "inject_context": InjectContext,
            "prune_messages": PruneMessages,
            "todo_state_changed": TodoStateChanged,
        }
        assert _DIRECTIVE_REGISTRY == expected

    def test_all_registered_types_are_directive_subclasses(self) -> None:
        for cls in _DIRECTIVE_REGISTRY.values():
            assert issubclass(cls, Directive)
