"""
Agentic todo list for tracking task progress.

Provides structured task tracking for complex multi-step operations.
The model manages todos through the entropi.todo_write internal tool.
"""

import uuid
from dataclasses import dataclass, field
from datetime import datetime
from enum import Enum
from typing import Any


class TodoStatus(Enum):
    """Status of a todo item."""

    PENDING = "pending"
    IN_PROGRESS = "in_progress"
    COMPLETED = "completed"


@dataclass
class TodoItem:
    """A single todo item."""

    content: str  # Task description (imperative: "Fix bug")
    active_form: str  # Present continuous ("Fixing bug")
    status: TodoStatus = TodoStatus.PENDING
    id: str = field(default_factory=lambda: str(uuid.uuid4())[:8])
    created_at: datetime = field(default_factory=datetime.utcnow)
    completed_at: datetime | None = None

    def mark_in_progress(self) -> None:
        """Mark this item as in progress."""
        self.status = TodoStatus.IN_PROGRESS

    def mark_completed(self) -> None:
        """Mark this item as completed."""
        self.status = TodoStatus.COMPLETED
        self.completed_at = datetime.utcnow()

    def to_dict(self) -> dict[str, Any]:
        """Convert to dictionary."""
        return {
            "id": self.id,
            "content": self.content,
            "active_form": self.active_form,
            "status": self.status.value,
            "created_at": self.created_at.isoformat(),
            "completed_at": self.completed_at.isoformat() if self.completed_at else None,
        }

    @classmethod
    def from_dict(cls, data: dict[str, Any]) -> "TodoItem":
        """Create from dictionary."""
        return cls(
            content=data["content"],
            active_form=data["active_form"],
            status=TodoStatus(data["status"]),
            id=data.get("id", str(uuid.uuid4())[:8]),
            created_at=(
                datetime.fromisoformat(data["created_at"])
                if data.get("created_at")
                else datetime.utcnow()
            ),
            completed_at=(
                datetime.fromisoformat(data["completed_at"]) if data.get("completed_at") else None
            ),
        )


@dataclass
class TodoList:
    """Manages a list of todo items for a conversation."""

    conversation_id: str | None = None
    items: list[TodoItem] = field(default_factory=list)

    def add(self, content: str, active_form: str) -> TodoItem:
        """Add a new todo item."""
        item = TodoItem(content=content, active_form=active_form)
        self.items.append(item)
        return item

    def get_current(self) -> TodoItem | None:
        """Get the currently in-progress item."""
        for item in self.items:
            if item.status == TodoStatus.IN_PROGRESS:
                return item
        return None

    def get_pending(self) -> list[TodoItem]:
        """Get all pending items."""
        return [i for i in self.items if i.status == TodoStatus.PENDING]

    def get_completed(self) -> list[TodoItem]:
        """Get all completed items."""
        return [i for i in self.items if i.status == TodoStatus.COMPLETED]

    @property
    def progress(self) -> tuple[int, int]:
        """Return (completed, total) counts."""
        completed = len(self.get_completed())
        return completed, len(self.items)

    @property
    def is_empty(self) -> bool:
        """Check if todo list is empty."""
        return len(self.items) == 0

    def clear(self) -> None:
        """Clear all items."""
        self.items = []

    def format_for_context(self) -> str:
        """Format todo list for injection into conversation context."""
        if self.is_empty:
            return ""
        status_icons = {"pending": "[ ]", "in_progress": "[>]", "completed": "[x]"}
        lines = ["[CURRENT TODO STATE]"]
        for item in self.items:
            icon = status_icons.get(item.status.value, "[ ]")
            lines.append(f"  {icon} {item.content}")
        completed, total = self.progress
        lines.append(f"Progress: {completed}/{total} completed")
        lines.append("[END TODO STATE]")
        return "\n".join(lines)

    def update_from_tool_call(self, todos: list[dict[str, Any]]) -> str:
        """
        Update the todo list from a tool call.

        Args:
            todos: List of todo dicts with content, active_form, status

        Returns:
            Status message
        """
        errors = []
        for i, t in enumerate(todos):
            if "content" not in t:
                errors.append(f"Item {i}: missing required field 'content'")
            if "active_form" not in t:
                errors.append(f"Item {i}: missing required field 'active_form'")
            if "status" not in t:
                errors.append(f"Item {i}: missing required field 'status'")
            elif t["status"] not in ("pending", "in_progress", "completed"):
                errors.append(
                    f"Item {i}: invalid status '{t['status']}' "
                    f"(must be: pending, in_progress, completed)"
                )

        if errors:
            return "Error: Invalid todo format.\n" + "\n".join(errors)

        # Validate: only one in_progress
        in_progress = [t for t in todos if t["status"] == "in_progress"]
        if len(in_progress) > 1:
            return "Error: Only one task can be in_progress at a time"

        parsed_items = []
        for t in todos:
            parsed_items.append(
                TodoItem(
                    content=t["content"],
                    active_form=t["active_form"],
                    status=TodoStatus(t["status"]),
                )
            )

        self.items = parsed_items

        completed, total = self.progress
        return f"Todo list updated: {completed}/{total} completed"

    def to_dict(self) -> dict[str, Any]:
        """Convert to dictionary for storage."""
        return {
            "conversation_id": self.conversation_id,
            "items": [item.to_dict() for item in self.items],
        }

    @classmethod
    def from_dict(cls, data: dict[str, Any]) -> "TodoList":
        """Create from dictionary."""
        todo_list = cls(conversation_id=data.get("conversation_id"))
        todo_list.items = [TodoItem.from_dict(item) for item in data.get("items", [])]
        return todo_list
