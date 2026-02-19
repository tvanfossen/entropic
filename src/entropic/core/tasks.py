"""
Task manager for async task lifecycle.

Manages tasks submitted via MCP, tracking their state through
queued → in_progress → completed/cancelled/failed.
"""

from __future__ import annotations

import time
import uuid
from collections.abc import Callable
from dataclasses import dataclass, field
from enum import Enum, auto
from typing import Any

from entropic.core.logging import get_logger

logger = get_logger("core.tasks")


class TaskStatus(Enum):
    """Task lifecycle states."""

    QUEUED = auto()
    IN_PROGRESS = auto()
    COMPLETED = auto()
    CANCELLED = auto()
    FAILED = auto()
    PREEMPTED = auto()  # Paused due to higher-priority input


@dataclass
class ToolCallRecord:
    """Record of a tool call made during task execution."""

    tool: str
    arguments: dict[str, Any]
    status: str  # "success", "error", "pending"
    result: str | None = None
    duration_ms: float = 0.0

    # File-specific fields
    path: str | None = None
    lines: int | None = None
    exit_code: int | None = None

    def to_dict(self) -> dict[str, Any]:
        """Convert to dict for MCP response."""
        d: dict[str, Any] = {
            "tool": self.tool,
            "status": self.status,
        }
        if self.path:
            d["path"] = self.path
        if self.lines is not None:
            d["lines"] = self.lines
        if self.exit_code is not None:
            d["exit_code"] = self.exit_code
        if self.result and self.status == "error":
            d["error"] = self.result
        return d


@dataclass
class Task:
    """A task submitted via MCP."""

    id: str
    message: str
    source: str  # "claude-code"
    status: TaskStatus = TaskStatus.QUEUED
    created_at: float = field(default_factory=time.time)
    started_at: float | None = None
    completed_at: float | None = None

    # Context provided by caller
    context: list[dict[str, Any]] | None = None

    # Results
    response: str = ""
    tool_calls: list[ToolCallRecord] = field(default_factory=list)
    error: str | None = None

    # Token usage
    input_tokens: int = 0
    output_tokens: int = 0

    # Preemption state
    partial_response: str = ""
    will_resume: bool = True

    # Queue position (for status reporting)
    queue_position: int = 0

    @property
    def elapsed_seconds(self) -> float:
        """Get elapsed time since task started."""
        if self.started_at is None:
            return 0.0
        end = self.completed_at or time.time()
        return end - self.started_at

    def to_queued_response(self) -> dict[str, Any]:
        """Response for newly queued task."""
        return {
            "task_id": self.id,
            "status": "queued",
            "queue_position": self.queue_position,
        }

    def to_poll_response(self) -> dict[str, Any]:
        """Response for poll_task."""
        base: dict[str, Any] = {
            "task_id": self.id,
            "status": self.status.name.lower(),
        }

        if self.status == TaskStatus.IN_PROGRESS:
            base["elapsed_seconds"] = int(self.elapsed_seconds)
            if self.tool_calls:
                last_tool = self.tool_calls[-1]
                base["progress"] = f"Executing tool: {last_tool.tool}"

        elif self.status == TaskStatus.COMPLETED:
            base["response"] = self.response
            base["tool_calls"] = [tc.to_dict() for tc in self.tool_calls]
            base["token_usage"] = {
                "input": self.input_tokens,
                "output": self.output_tokens,
            }

        elif self.status == TaskStatus.PREEMPTED:
            base["partial_response"] = self.partial_response
            base["will_resume"] = self.will_resume
            base["queue_position"] = self.queue_position

        elif self.status == TaskStatus.FAILED:
            base["error"] = self.error

        elif self.status == TaskStatus.CANCELLED:
            base["partial_response"] = self.partial_response

        return base

    def to_completed_notification(self) -> dict[str, Any]:
        """Notification payload for task_completed."""
        return {
            "task_id": self.id,
            "status": "completed",
            "response": self.response,
            "tool_calls": [tc.to_dict() for tc in self.tool_calls],
            "token_usage": {
                "input": self.input_tokens,
                "output": self.output_tokens,
            },
        }

    def to_preempted_notification(self, reason: str = "human_input") -> dict[str, Any]:
        """Notification payload for task_preempted."""
        return {
            "task_id": self.id,
            "reason": reason,
            "partial_response": self.partial_response,
            "will_resume": self.will_resume,
        }


class TaskManager:
    """
    Manages async task lifecycle.

    Features:
    - Task state tracking
    - Tool call result collection
    - Progress reporting
    - Cancellation support
    """

    def __init__(self, max_tasks: int = 1000) -> None:
        """
        Initialize task manager.

        Args:
            max_tasks: Maximum number of tasks to retain
        """
        self._tasks: dict[str, Task] = {}
        self._max_tasks = max_tasks

        # Callbacks for notifications
        self._on_progress: Callable[[Task, str, int], None] | None = None
        self._on_completed: Callable[[Task], None] | None = None
        self._on_preempted: Callable[[Task, str], None] | None = None

    def set_callbacks(
        self,
        on_progress: Callable[[Task, str, int], None] | None = None,
        on_completed: Callable[[Task], None] | None = None,
        on_preempted: Callable[[Task, str], None] | None = None,
    ) -> None:
        """
        Set notification callbacks.

        Args:
            on_progress: Called with (task, progress_message, percent)
            on_completed: Called when task completes
            on_preempted: Called when task is preempted with (task, reason)
        """
        self._on_progress = on_progress
        self._on_completed = on_completed
        self._on_preempted = on_preempted

    def create_task(
        self,
        message: str,
        source: str = "claude-code",
        context: list[dict[str, Any]] | None = None,
    ) -> Task:
        """
        Create a new task.

        Args:
            message: Task message/prompt
            source: Task source
            context: Optional file context

        Returns:
            Created task
        """
        # Prune old tasks if needed
        self._prune_if_needed()

        task = Task(
            id=str(uuid.uuid4()),
            message=message,
            source=source,
            context=context,
        )

        self._tasks[task.id] = task
        logger.debug(f"Created task {task.id[:8]}: {message[:50]}...")

        return task

    def get_task(self, task_id: str) -> Task | None:
        """Get a task by ID."""
        return self._tasks.get(task_id)

    def start_task(self, task_id: str) -> bool:
        """
        Mark a task as started.

        Args:
            task_id: Task ID

        Returns:
            True if task was started
        """
        task = self._tasks.get(task_id)
        if not task:
            return False

        if task.status != TaskStatus.QUEUED:
            logger.warning(f"Cannot start task {task_id[:8]} in state {task.status}")
            return False

        task.status = TaskStatus.IN_PROGRESS
        task.started_at = time.time()
        logger.debug(f"Started task {task_id[:8]}")

        return True

    def complete_task(
        self,
        task_id: str,
        response: str,
        input_tokens: int = 0,
        output_tokens: int = 0,
    ) -> bool:
        """
        Mark a task as completed.

        Args:
            task_id: Task ID
            response: Final response
            input_tokens: Token count
            output_tokens: Token count

        Returns:
            True if task was completed
        """
        task = self._tasks.get(task_id)
        if not task:
            return False

        task.status = TaskStatus.COMPLETED
        task.completed_at = time.time()
        task.response = response
        task.input_tokens = input_tokens
        task.output_tokens = output_tokens

        logger.info(
            f"Completed task {task_id[:8]} in {task.elapsed_seconds:.1f}s "
            f"({len(task.tool_calls)} tool calls)"
        )

        if self._on_completed:
            self._on_completed(task)

        return True

    def fail_task(self, task_id: str, error: str) -> bool:
        """
        Mark a task as failed.

        Args:
            task_id: Task ID
            error: Error message

        Returns:
            True if task was marked failed
        """
        task = self._tasks.get(task_id)
        if not task:
            return False

        task.status = TaskStatus.FAILED
        task.completed_at = time.time()
        task.error = error

        logger.error(f"Task {task_id[:8]} failed: {error}")

        return True

    def cancel_task(self, task_id: str, reason: str | None = None) -> bool:
        """
        Cancel a task.

        Args:
            task_id: Task ID
            reason: Optional reason for cancellation

        Returns:
            True if task was cancelled
        """
        task = self._tasks.get(task_id)
        if not task:
            return False

        if task.status in (TaskStatus.COMPLETED, TaskStatus.FAILED, TaskStatus.CANCELLED):
            logger.warning(f"Cannot cancel task {task_id[:8]} in state {task.status}")
            return False

        task.status = TaskStatus.CANCELLED
        task.completed_at = time.time()
        task.error = reason

        logger.info(f"Cancelled task {task_id[:8]}: {reason}")

        return True

    def preempt_task(
        self,
        task_id: str,
        partial_response: str = "",
        reason: str = "human_input",
        will_resume: bool = True,
    ) -> bool:
        """
        Mark a task as preempted.

        Args:
            task_id: Task ID
            partial_response: Partial response so far
            reason: Reason for preemption
            will_resume: Whether task will resume

        Returns:
            True if task was preempted
        """
        task = self._tasks.get(task_id)
        if not task:
            return False

        if task.status != TaskStatus.IN_PROGRESS:
            return False

        task.status = TaskStatus.PREEMPTED
        task.partial_response = partial_response
        task.will_resume = will_resume

        logger.info(f"Preempted task {task_id[:8]}: {reason}")

        if self._on_preempted:
            self._on_preempted(task, reason)

        return True

    def resume_task(self, task_id: str) -> bool:
        """
        Resume a preempted task.

        Args:
            task_id: Task ID

        Returns:
            True if task was resumed
        """
        task = self._tasks.get(task_id)
        if not task:
            return False

        if task.status != TaskStatus.PREEMPTED:
            return False

        task.status = TaskStatus.IN_PROGRESS
        logger.debug(f"Resumed task {task_id[:8]}")

        return True

    def add_tool_call(self, task_id: str, record: ToolCallRecord) -> None:
        """
        Record a tool call for a task.

        Args:
            task_id: Task ID
            record: Tool call record
        """
        task = self._tasks.get(task_id)
        if not task:
            return

        task.tool_calls.append(record)

        # Calculate progress
        percent = min(95, len(task.tool_calls) * 10)  # Rough estimate

        if self._on_progress:
            progress = f"Executing: {record.tool}"
            self._on_progress(task, progress, percent)

    def update_tool_call(
        self,
        task_id: str,
        tool: str,
        status: str,
        result: str | None = None,
        duration_ms: float = 0.0,
    ) -> None:
        """
        Update the most recent matching tool call.

        Args:
            task_id: Task ID
            tool: Tool name to match
            status: New status
            result: Result or error
            duration_ms: Duration in milliseconds
        """
        task = self._tasks.get(task_id)
        if not task:
            return

        # Find the most recent pending call for this tool
        for tc in reversed(task.tool_calls):
            if tc.tool == tool and tc.status == "pending":
                tc.status = status
                tc.result = result
                tc.duration_ms = duration_ms
                break

    def report_progress(self, task_id: str, message: str, percent: int) -> None:
        """
        Report task progress.

        Args:
            task_id: Task ID
            message: Progress message
            percent: Completion percentage (0-100)
        """
        task = self._tasks.get(task_id)
        if not task:
            return

        if self._on_progress:
            self._on_progress(task, message, percent)

    def update_queue_positions(self, positions: dict[str, int]) -> None:
        """
        Update queue positions for tasks.

        Args:
            positions: Mapping of task_id -> queue_position
        """
        for task_id, position in positions.items():
            task = self._tasks.get(task_id)
            if task:
                task.queue_position = position

    def get_active_tasks(self) -> list[Task]:
        """Get all active (queued or in_progress) tasks."""
        return [
            task
            for task in self._tasks.values()
            if task.status in (TaskStatus.QUEUED, TaskStatus.IN_PROGRESS, TaskStatus.PREEMPTED)
        ]

    def get_queued_count(self) -> int:
        """Get count of queued tasks."""
        return sum(1 for t in self._tasks.values() if t.status == TaskStatus.QUEUED)

    def _prune_if_needed(self) -> None:
        """Remove old completed tasks if over limit."""
        if len(self._tasks) < self._max_tasks:
            return

        # Find completed tasks sorted by completion time
        completed = [
            t
            for t in self._tasks.values()
            if t.status in (TaskStatus.COMPLETED, TaskStatus.FAILED, TaskStatus.CANCELLED)
        ]
        completed.sort(key=lambda t: t.completed_at or 0)

        # Remove oldest completed tasks
        to_remove = len(self._tasks) - self._max_tasks + 100  # Leave some buffer
        for task in completed[:to_remove]:
            del self._tasks[task.id]

        logger.debug(f"Pruned {to_remove} old tasks")
