"""Tests for directive processing — tool-to-engine communication protocol."""

import json

from entropi.core.base import Message
from entropi.core.directives import (
    CLEAR_SELF_TODOS,
    INJECT_CONTEXT,
    PRUNE_MESSAGES,
    STOP_PROCESSING,
    TIER_CHANGE,
    TODO_STATE_CHANGED,
    DirectiveProcessor,
    DirectiveResult,
    extract_directives,
)


def _make_ctx() -> object:
    """Create a minimal LoopContext for testing."""
    from entropi.core.engine import LoopContext

    return LoopContext()


class TestExtractDirectives:
    """extract_directives parses _directives from JSON tool results."""

    def test_extracts_directives_list(self) -> None:
        """Valid JSON with _directives returns the list."""
        content = json.dumps(
            {
                "result": "ok",
                "_directives": [{"type": "stop_processing"}],
            }
        )
        assert extract_directives(content) == [{"type": "stop_processing"}]

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
        """Multiple directives in order."""
        directives = [
            {"type": "clear_self_todos"},
            {"type": "tier_change", "params": {"tier": "code"}},
            {"type": "stop_processing"},
        ]
        content = json.dumps({"result": "ok", "_directives": directives})
        assert extract_directives(content) == directives


class TestDirectiveProcessor:
    """DirectiveProcessor dispatches to registered handlers."""

    def test_process_empty_list(self) -> None:
        """Empty directive list returns default result."""
        proc = DirectiveProcessor()
        result = proc.process(_make_ctx(), [])
        assert result.stop_processing is False
        assert result.tier_changed is False

    def test_unknown_directive_ignored(self) -> None:
        """Unknown directive type logged but doesn't crash."""
        proc = DirectiveProcessor()
        result = proc.process(_make_ctx(), [{"type": "nonexistent"}])
        assert result.stop_processing is False

    def test_registered_handler_called(self) -> None:
        """Registered handler receives ctx, params, result."""
        calls = []

        def handler(ctx, params, result):
            calls.append((params, result))

        proc = DirectiveProcessor()
        proc.register("test_type", handler)
        ctx = _make_ctx()
        proc.process(ctx, [{"type": "test_type", "params": {"key": "val"}}])

        assert len(calls) == 1
        assert calls[0][0] == {"key": "val"}

    def test_stop_processing_handler(self) -> None:
        """Handler can set stop_processing on result."""

        def stop_handler(ctx, params, result):
            result.stop_processing = True

        proc = DirectiveProcessor()
        proc.register(STOP_PROCESSING, stop_handler)
        result = proc.process(_make_ctx(), [{"type": STOP_PROCESSING}])
        assert result.stop_processing is True

    def test_tier_change_handler(self) -> None:
        """Handler can set tier_changed on result."""

        def tier_handler(ctx, params, result):
            result.tier_changed = True

        proc = DirectiveProcessor()
        proc.register(TIER_CHANGE, tier_handler)
        result = proc.process(
            _make_ctx(),
            [{"type": TIER_CHANGE, "params": {"tier": "code", "reason": "test"}}],
        )
        assert result.tier_changed is True

    def test_multiple_directives_processed_in_order(self) -> None:
        """Directives processed sequentially, result accumulates."""
        order = []

        def first(ctx, params, result):
            order.append("first")

        def second(ctx, params, result):
            order.append("second")
            result.stop_processing = True

        proc = DirectiveProcessor()
        proc.register("a", first)
        proc.register("b", second)
        result = proc.process(_make_ctx(), [{"type": "a"}, {"type": "b"}])

        assert order == ["first", "second"]
        assert result.stop_processing is True

    def test_missing_params_defaults_to_empty_dict(self) -> None:
        """Directive without 'params' key passes empty dict."""
        received_params = []

        def handler(ctx, params, result):
            received_params.append(params)

        proc = DirectiveProcessor()
        proc.register("test", handler)
        proc.process(_make_ctx(), [{"type": "test"}])

        assert received_params == [{}]

    def test_registered_types_property(self) -> None:
        """registered_types lists all registered directive types."""
        proc = DirectiveProcessor()
        proc.register("a", lambda c, p, r: None)
        proc.register("b", lambda c, p, r: None)
        assert sorted(proc.registered_types) == ["a", "b"]

    def test_inject_context_via_result(self) -> None:
        """Handler can append to injected_messages."""

        def inject_handler(ctx, params, result):
            result.injected_messages.append(Message(role="user", content=params["content"]))

        proc = DirectiveProcessor()
        proc.register(INJECT_CONTEXT, inject_handler)
        result = proc.process(
            _make_ctx(),
            [{"type": INJECT_CONTEXT, "params": {"content": "test warning"}}],
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


class TestDirectiveConstants:
    """Directive type constants match expected strings."""

    def test_constants(self) -> None:
        assert STOP_PROCESSING == "stop_processing"
        assert TIER_CHANGE == "tier_change"
        assert CLEAR_SELF_TODOS == "clear_self_todos"
        assert INJECT_CONTEXT == "inject_context"
        assert PRUNE_MESSAGES == "prune_messages"
        assert TODO_STATE_CHANGED == "todo_state_changed"
