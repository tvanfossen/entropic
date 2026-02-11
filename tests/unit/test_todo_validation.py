"""Tests for strict todo validation — no silent fallbacks."""

from entropi.core.todos import TodoItem, TodoList, TodoStatus


class TestUpdateFromToolCallValidation:
    """update_from_tool_call rejects malformed input with clear errors."""

    def test_valid_todos_accepted(self) -> None:
        """Well-formed todos update the list successfully."""
        todo_list = TodoList()
        result = todo_list.update_from_tool_call(
            [
                {"content": "Fix bug", "active_form": "Fixing bug", "status": "in_progress"},
                {"content": "Add test", "active_form": "Adding test", "status": "pending"},
            ]
        )
        assert "2" in result
        assert "Error" not in result
        assert todo_list.items[0].status == TodoStatus.IN_PROGRESS
        assert todo_list.items[1].status == TodoStatus.PENDING

    def test_missing_content_rejected(self) -> None:
        """Missing 'content' field returns error, not silent fallback."""
        todo_list = TodoList()
        result = todo_list.update_from_tool_call(
            [
                {"active_form": "Fixing bug", "status": "pending"},
            ]
        )
        assert "Error" in result
        assert "missing required field 'content'" in result
        assert len(todo_list.items) == 0

    def test_missing_active_form_rejected(self) -> None:
        """Missing 'active_form' field returns error."""
        todo_list = TodoList()
        result = todo_list.update_from_tool_call(
            [
                {"content": "Fix bug", "status": "pending"},
            ]
        )
        assert "Error" in result
        assert "missing required field 'active_form'" in result

    def test_missing_status_rejected(self) -> None:
        """Missing 'status' field returns error, not default to pending."""
        todo_list = TodoList()
        result = todo_list.update_from_tool_call(
            [
                {"content": "Fix bug", "active_form": "Fixing bug"},
            ]
        )
        assert "Error" in result
        assert "missing required field 'status'" in result

    def test_wrong_field_name_state_rejected(self) -> None:
        """Using 'state' instead of 'status' returns error."""
        todo_list = TodoList()
        result = todo_list.update_from_tool_call(
            [
                {"content": "Fix bug", "active_form": "Fixing bug", "state": "pending"},
            ]
        )
        assert "Error" in result
        assert "missing required field 'status'" in result

    def test_invalid_status_value_rejected(self) -> None:
        """Invalid status value returns error, not default to pending."""
        todo_list = TodoList()
        result = todo_list.update_from_tool_call(
            [
                {"content": "Fix bug", "active_form": "Fixing bug", "status": "done"},
            ]
        )
        assert "Error" in result
        assert "invalid status 'done'" in result

    def test_multiple_errors_reported(self) -> None:
        """All validation errors reported in single response."""
        todo_list = TodoList()
        result = todo_list.update_from_tool_call(
            [
                {"task": "Fix bug", "state": "pending"},
            ]
        )
        assert "Error" in result
        assert "content" in result
        assert "active_form" in result
        assert "status" in result

    def test_multiple_in_progress_rejected(self) -> None:
        """More than one in_progress item returns error."""
        todo_list = TodoList()
        result = todo_list.update_from_tool_call(
            [
                {"content": "Fix bug", "active_form": "Fixing bug", "status": "in_progress"},
                {"content": "Add test", "active_form": "Adding test", "status": "in_progress"},
            ]
        )
        assert "Error" in result
        assert "Only one task" in result

    def test_items_not_modified_on_validation_error(self) -> None:
        """Existing items preserved when new input fails validation."""
        todo_list = TodoList()
        todo_list.update_from_tool_call(
            [
                {"content": "First", "active_form": "Working", "status": "pending"},
            ]
        )
        assert len(todo_list.items) == 1

        result = todo_list.update_from_tool_call([{"bad": "data"}])
        assert "Error" in result
        assert len(todo_list.items) == 1
        assert todo_list.items[0].content == "First"


class TestTargetTier:
    """Tests for target_tier field on todo items."""

    def test_target_tier_persists_through_dict_roundtrip(self) -> None:
        """target_tier survives to_dict/from_dict."""
        item = TodoItem(content="Fix bug", active_form="Fixing bug", target_tier="code")
        data = item.to_dict()
        assert data["target_tier"] == "code"

        restored = TodoItem.from_dict(data)
        assert restored.target_tier == "code"

    def test_target_tier_none_omitted_from_dict(self) -> None:
        """None target_tier not included in dict output."""
        item = TodoItem(content="Fix bug", active_form="Fixing bug")
        data = item.to_dict()
        assert "target_tier" not in data

    def test_target_tier_none_from_dict_without_field(self) -> None:
        """Missing target_tier in dict results in None."""
        data = {
            "content": "Fix bug",
            "active_form": "Fixing bug",
            "status": "pending",
        }
        item = TodoItem.from_dict(data)
        assert item.target_tier is None

    def test_update_from_tool_call_passes_target_tier(self) -> None:
        """target_tier flows through update_from_tool_call."""
        todo_list = TodoList()
        result = todo_list.update_from_tool_call(
            [
                {
                    "content": "Fix engine.py",
                    "active_form": "Fixing engine",
                    "status": "pending",
                    "target_tier": "code",
                },
            ]
        )
        assert "Error" not in result
        assert todo_list.items[0].target_tier == "code"

    def test_update_from_tool_call_without_target_tier(self) -> None:
        """Omitting target_tier still works (backward compatible)."""
        todo_list = TodoList()
        result = todo_list.update_from_tool_call(
            [
                {
                    "content": "Fix bug",
                    "active_form": "Fixing bug",
                    "status": "pending",
                },
            ]
        )
        assert "Error" not in result
        assert todo_list.items[0].target_tier is None

    def test_get_todos_for_tier(self) -> None:
        """get_todos_for_tier filters by target_tier."""
        todo_list = TodoList()
        todo_list.update_from_tool_call(
            [
                {
                    "content": "Read files",
                    "active_form": "Reading",
                    "status": "completed",
                },
                {
                    "content": "Fix engine",
                    "active_form": "Fixing",
                    "status": "pending",
                    "target_tier": "code",
                },
                {
                    "content": "Add tests",
                    "active_form": "Adding",
                    "status": "pending",
                    "target_tier": "code",
                },
                {
                    "content": "Think more",
                    "active_form": "Thinking",
                    "status": "pending",
                    "target_tier": "thinking",
                },
            ]
        )
        code_todos = todo_list.get_todos_for_tier("code")
        assert len(code_todos) == 2
        assert all(t.target_tier == "code" for t in code_todos)

        thinking_todos = todo_list.get_todos_for_tier("thinking")
        assert len(thinking_todos) == 1

        normal_todos = todo_list.get_todos_for_tier("normal")
        assert len(normal_todos) == 0

    def test_format_for_context_grouped_when_mixed(self) -> None:
        """Grouped format when items target different tiers."""
        todo_list = TodoList()
        todo_list.update_from_tool_call(
            [
                {
                    "content": "Read arch docs",
                    "active_form": "Reading",
                    "status": "completed",
                },
                {
                    "content": "Fix compaction",
                    "active_form": "Fixing",
                    "status": "pending",
                    "target_tier": "code",
                },
            ]
        )
        output = todo_list.format_for_context()
        assert "For code tier:" in output
        assert "For self:" in output
        assert "[CURRENT TODO STATE]" in output

    def test_format_for_context_flat_when_all_same(self) -> None:
        """Flat format when all items have same target (or all None)."""
        todo_list = TodoList()
        todo_list.update_from_tool_call(
            [
                {
                    "content": "Task A",
                    "active_form": "Doing A",
                    "status": "pending",
                },
                {
                    "content": "Task B",
                    "active_form": "Doing B",
                    "status": "pending",
                },
            ]
        )
        output = todo_list.format_for_context()
        assert "For " not in output
        assert "[ ] Task A" in output
        assert "[ ] Task B" in output
