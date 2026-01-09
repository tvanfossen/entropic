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
            created_at=datetime.fromisoformat(data["created_at"]) if data.get("created_at") else datetime.utcnow(),
            completed_at=datetime.fromisoformat(data["completed_at"]) if data.get("completed_at") else None,
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

    def update_from_tool_call(self, todos: list[dict[str, Any]]) -> str:
        """
        Update the todo list from a tool call.

        Args:
            todos: List of todo dicts with content, active_form, status

        Returns:
            Status message
        """
        # Validate: only one in_progress
        in_progress = [t for t in todos if t.get("status") == "in_progress"]
        if len(in_progress) > 1:
            return "Error: Only one task can be in_progress at a time"

        # Update the todo list
        self.items = [
            TodoItem(
                content=t["content"],
                active_form=t["active_form"],
                status=TodoStatus(t["status"]),
            )
            for t in todos
        ]

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


# Tool definition for system prompt injection
TODO_TOOL_DEFINITION = {
    "name": "entropi.todo_write",
    "description": """Manage your task list for tracking progress on complex tasks.

Use this tool when:
- Starting a multi-step task (3+ steps)
- User provides multiple things to do
- You need to track progress on a complex operation
- After completing a task to mark it done

Task states:
- pending: Not started yet
- in_progress: Currently working on (only ONE at a time)
- completed: Finished successfully

IMPORTANT:
- Mark tasks complete IMMEDIATELY after finishing
- Only have ONE task in_progress at a time
- Use imperative form for content ("Fix bug") and active form for display ("Fixing bug")
""",
    "inputSchema": {
        "type": "object",
        "properties": {
            "todos": {
                "type": "array",
                "description": "The complete updated todo list",
                "items": {
                    "type": "object",
                    "properties": {
                        "content": {
                            "type": "string",
                            "description": "Task description in imperative form",
                        },
                        "active_form": {
                            "type": "string",
                            "description": "Task description in present continuous",
                        },
                        "status": {
                            "type": "string",
                            "enum": ["pending", "in_progress", "completed"],
                        },
                    },
                    "required": ["content", "status", "active_form"],
                },
            },
        },
        "required": ["todos"],
    },
}


# System prompt addition for todo usage guidance
TODO_SYSTEM_PROMPT = """
# Task Management

You have access to a todo list tool (`entropi.todo_write`) to track progress on complex tasks.

## When to Use Todos

USE the todo list when:
- Task requires 3+ distinct steps
- User provides multiple tasks (numbered or comma-separated)
- Working on non-trivial features or refactoring
- You need to track what's done vs remaining

DO NOT use todos for:
- Single, straightforward tasks
- Simple questions or explanations
- Tasks completable in 1-2 trivial steps

## Todo Guidelines

1. **Mark in_progress BEFORE starting work** - Shows user what you're doing
2. **Mark completed IMMEDIATELY after finishing** - Don't batch completions
3. **Only ONE task in_progress at a time** - Focused execution
4. **Break complex tasks into concrete steps** - "Implement auth" → specific files/functions
5. **Update list as you learn more** - Add discovered subtasks

## Examples

User: "Add dark mode to the settings page"
→ Create todos: research current theme system, add theme state, create toggle component, update styles, test

User: "Fix the typo in README"
→ Don't use todos (single trivial task)

User: "1. Add login page 2. Add logout button 3. Add password reset"
→ Create todos for each numbered item
"""
