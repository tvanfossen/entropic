# Feature Proposal: Agentic Todo Lists

> Task tracking system for complex multi-step operations

**Status:** Implemented (Core)
**Priority:** High (improves task completion reliability)
**Complexity:** Medium
**Dependencies:** Terminal UI

## Implementation Status

| Component | Status | Location |
|-----------|--------|----------|
| Data model (TodoItem, TodoList, TodoStatus) | Done | `src/entropi/core/todos.py` |
| Tool definition (entropi.todo_write) | Done | `src/entropi/core/todos.py` |
| System prompt guidance | Done | `src/entropi/core/todos.py` |
| Engine integration | Done | `src/entropi/core/engine.py` |
| UI panel (TodoPanel) | Done | `src/entropi/ui/components.py` |
| Terminal rendering | Done | `src/entropi/ui/terminal.py` |
| App callback wiring | Done | `src/entropi/app.py` |
| Storage persistence | Not started | - |
| `/todos` commands | Not started | - |

---

## Problem Statement

When working on complex tasks, the model needs to:

1. **Track progress** - Know what's done and what remains
2. **Show transparency** - Let users see the plan and current status
3. **Recover from interrupts** - Resume where it left off after errors/interruptions
4. **Avoid forgetting steps** - Long conversations lose context, todos persist

Without explicit task tracking, the model may:
- Forget steps in multi-part tasks
- Repeat completed work
- Leave tasks incomplete without realizing
- Provide no visibility into its plan

---

## Solution: Agentic Todo System

A structured task list that the model maintains during complex operations:

### User Experience

```
You: Refactor the authentication module to use JWT tokens

╭─ Todo List ────────────────────────────────────────────────────────╮
│ ○ Research current auth implementation                             │
│ ● Installing PyJWT dependency                                      │
│ ○ Create JWT token generation utility                              │
│ ○ Update login endpoint to return JWT                              │
│ ○ Add JWT validation middleware                                    │
│ ○ Update protected routes to use middleware                        │
│ ○ Add token refresh endpoint                                       │
│ ○ Write tests for JWT flow                                         │
╰────────────────────────────────────────────────────────────────────╯

[Executing bash.execute: pip install PyJWT...]
Done bash.execute (1243ms, Successfully installed PyJWT-2.8.0)

╭─ Todo List ────────────────────────────────────────────────────────╮
│ ✓ Research current auth implementation                             │
│ ✓ Install PyJWT dependency                                         │
│ ● Creating JWT token generation utility                            │
│ ○ Update login endpoint to return JWT                              │
│ ...                                                                │
╰────────────────────────────────────────────────────────────────────╯
```

### Status Indicators

| Symbol | Status | Meaning |
|--------|--------|---------|
| ○ | `pending` | Not started |
| ● | `in_progress` | Currently working on |
| ✓ | `completed` | Finished successfully |

---

## Data Model

```python
from dataclasses import dataclass, field
from enum import Enum
from datetime import datetime


class TodoStatus(Enum):
    """Status of a todo item."""
    PENDING = "pending"
    IN_PROGRESS = "in_progress"
    COMPLETED = "completed"


@dataclass
class TodoItem:
    """A single todo item."""

    content: str                           # Task description (imperative: "Fix bug")
    active_form: str                       # Present continuous ("Fixing bug")
    status: TodoStatus = TodoStatus.PENDING
    id: str = field(default_factory=lambda: str(uuid.uuid4()))
    created_at: datetime = field(default_factory=datetime.utcnow)
    completed_at: datetime | None = None

    def mark_in_progress(self) -> None:
        """Mark this item as in progress."""
        self.status = TodoStatus.IN_PROGRESS

    def mark_completed(self) -> None:
        """Mark this item as completed."""
        self.status = TodoStatus.COMPLETED
        self.completed_at = datetime.utcnow()


@dataclass
class TodoList:
    """Manages a list of todo items for a conversation."""

    conversation_id: str
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

    def clear(self) -> None:
        """Clear all items."""
        self.items = []
```

---

## Tool Definition

The model interacts with todos through a dedicated tool:

```python
class TodoWriteTool:
    """Tool for model to manage its task list."""

    name = "entropi.todo_write"
    description = """Manage your task list for tracking progress on complex tasks.

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
- Use imperative form for content ("Fix bug") and active form for progress ("Fixing bug")
"""

    input_schema = {
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
    }

    async def execute(self, todos: list[dict]) -> str:
        """Update the todo list."""
        # Validate: only one in_progress
        in_progress = [t for t in todos if t["status"] == "in_progress"]
        if len(in_progress) > 1:
            return "Error: Only one task can be in_progress at a time"

        # Update the todo list
        self.todo_list.items = [
            TodoItem(
                content=t["content"],
                active_form=t["active_form"],
                status=TodoStatus(t["status"]),
            )
            for t in todos
        ]

        # Persist to storage
        await self.storage.save_todos(self.conversation_id, self.todo_list)

        completed, total = self.todo_list.progress
        return f"Todo list updated: {completed}/{total} completed"
```

---

## System Prompt Integration

Add to the base system prompt:

```markdown
# Task Management

You have access to a todo list tool to track progress on complex tasks.

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
```

---

## UI Components

### Todo Panel

```python
class TodoPanel:
    """Renders the current todo list."""

    STATUS_SYMBOLS = {
        TodoStatus.PENDING: "○",
        TodoStatus.IN_PROGRESS: "●",
        TodoStatus.COMPLETED: "✓",
    }

    STATUS_COLORS = {
        TodoStatus.PENDING: "dim",
        TodoStatus.IN_PROGRESS: "cyan bold",
        TodoStatus.COMPLETED: "green",
    }

    def render(self, todo_list: TodoList) -> Panel:
        """Render the todo list as a Rich panel."""
        if not todo_list.items:
            return None

        lines = []
        for item in todo_list.items:
            symbol = self.STATUS_SYMBOLS[item.status]
            color = self.STATUS_COLORS[item.status]

            # Show active_form for in_progress, content otherwise
            text = item.active_form if item.status == TodoStatus.IN_PROGRESS else item.content

            lines.append(f"[{color}]{symbol} {text}[/]")

        completed, total = todo_list.progress
        title = f"Todo List ({completed}/{total})"

        return Panel(
            "\n".join(lines),
            title=title,
            border_style="blue",
            padding=(0, 1),
        )
```

### Progress Indicator

```python
class TodoProgress:
    """Shows compact progress indicator."""

    def render(self, todo_list: TodoList) -> str:
        """Render progress bar."""
        completed, total = todo_list.progress
        if total == 0:
            return ""

        # Progress bar: [████░░░░░░] 4/10
        bar_width = 10
        filled = int((completed / total) * bar_width)
        empty = bar_width - filled

        bar = "█" * filled + "░" * empty
        return f"[{bar}] {completed}/{total}"
```

---

## Integration with Agentic Loop

```python
class AgentEngine:
    """Agentic loop with todo support."""

    def __init__(self, ...):
        self.todo_list = TodoList(conversation_id=conversation_id)
        self._todo_tool = TodoWriteTool(self.todo_list, self.storage)

    async def process_turn(self, user_message: str) -> AsyncIterator[str]:
        # Include todo context in system prompt if todos exist
        if self.todo_list.items:
            self._inject_todo_context()

        # Normal processing...
        async for chunk in self._generate_response():
            yield chunk

        # Check for stale in_progress items (might indicate incomplete task)
        current = self.todo_list.get_current()
        if current:
            logger.debug(f"Task still in progress: {current.content}")

    def _inject_todo_context(self) -> None:
        """Add current todo state to context."""
        completed, total = self.todo_list.progress
        pending = self.todo_list.get_pending()
        current = self.todo_list.get_current()

        context = f"""
Current task progress: {completed}/{total} completed
"""
        if current:
            context += f"Currently working on: {current.active_form}\n"

        if pending:
            context += f"Remaining tasks: {len(pending)}\n"

        self._add_system_context(context)
```

---

## Storage Schema

```sql
-- Todo items for conversations
CREATE TABLE todos (
    id TEXT PRIMARY KEY,
    conversation_id TEXT REFERENCES conversations(id),
    content TEXT NOT NULL,
    active_form TEXT NOT NULL,
    status TEXT NOT NULL DEFAULT 'pending',
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    completed_at TIMESTAMP,
    position INTEGER  -- For ordering
);

CREATE INDEX idx_todos_conversation ON todos(conversation_id);
CREATE INDEX idx_todos_status ON todos(status);
```

---

## Commands

```
/todos              Show current todo list
/todos clear        Clear all todos
/todos add <task>   Manually add a todo item
```

---

## Behavior Guidelines

### Model Should Use Todos When:

1. **Complex multi-step tasks**
   - "Implement user authentication" (many files, steps)
   - "Refactor the database layer" (research, plan, implement, test)

2. **User provides multiple tasks**
   - "Fix the login bug, add logout button, and update the tests"
   - Numbered lists or comma-separated items

3. **Tasks requiring tracking**
   - Running tests and fixing failures (each failure = todo)
   - Build errors to fix

### Model Should NOT Use Todos When:

1. **Single straightforward task**
   - "What does this function do?"
   - "Add a docstring to this class"

2. **Trivial operations**
   - "Run the tests"
   - "Show me the README"

3. **Pure conversation**
   - "Explain how authentication works"
   - "What are the best practices for..."

---

## Testing

```python
class TestTodoList:
    def test_single_in_progress(self):
        """Only one item can be in_progress."""
        todos = TodoList("conv1")
        todos.add("Task 1", "Doing task 1")
        todos.add("Task 2", "Doing task 2")

        todos.items[0].mark_in_progress()
        todos.items[1].mark_in_progress()

        # Validation should catch this
        assert len([t for t in todos.items if t.status == TodoStatus.IN_PROGRESS]) <= 1

    def test_progress_tracking(self):
        """Progress should update correctly."""
        todos = TodoList("conv1")
        todos.add("Task 1", "Doing 1")
        todos.add("Task 2", "Doing 2")
        todos.add("Task 3", "Doing 3")

        assert todos.progress == (0, 3)

        todos.items[0].mark_completed()
        assert todos.progress == (1, 3)

        todos.items[1].mark_completed()
        todos.items[2].mark_completed()
        assert todos.progress == (3, 3)


class TestTodoTool:
    async def test_update_todos(self):
        """Tool should update todo list."""
        tool = TodoWriteTool(todo_list, storage)

        result = await tool.execute([
            {"content": "Fix bug", "active_form": "Fixing bug", "status": "in_progress"},
            {"content": "Add tests", "active_form": "Adding tests", "status": "pending"},
        ])

        assert "1/2" in result
        assert len(todo_list.items) == 2

    async def test_reject_multiple_in_progress(self):
        """Should reject multiple in_progress items."""
        tool = TodoWriteTool(todo_list, storage)

        result = await tool.execute([
            {"content": "Task 1", "active_form": "Doing 1", "status": "in_progress"},
            {"content": "Task 2", "active_form": "Doing 2", "status": "in_progress"},
        ])

        assert "Error" in result
```

---

## Rollout Plan

1. **Phase 1**: Implement TodoList data model and storage
2. **Phase 2**: Add TodoWriteTool to MCP server
3. **Phase 3**: Add UI rendering (TodoPanel, progress indicator)
4. **Phase 4**: Add system prompt guidance for todo usage
5. **Phase 5**: Add `/todos` commands
6. **Phase 6**: Add todo persistence across sessions

---

## Future Enhancements

- **Subtasks**: Nested todo items for complex steps
- **Time tracking**: How long each task took
- **Dependencies**: Tasks that block other tasks
- **Priority levels**: High/medium/low priority markers
- **Templates**: Pre-defined todo lists for common operations
- **Export**: Export task history for reporting
