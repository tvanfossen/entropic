"""Tests for strict todo validation — no silent fallbacks."""

from entropi.core.todos import TodoList, TodoStatus


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
