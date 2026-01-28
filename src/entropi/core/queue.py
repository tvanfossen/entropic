"""
Message queue with priority support for external MCP integration.

Enables human and Claude Code to share the same conversation context
with human input always taking priority.
"""

from __future__ import annotations

import asyncio
import time
import uuid
from dataclasses import dataclass, field
from enum import IntEnum
from typing import Any, Callable

from entropi.core.logging import get_logger

logger = get_logger("core.queue")


class MessagePriority(IntEnum):
    """Message priority levels.

    Lower values = higher priority.
    Human input always preempts Claude Code.
    """

    HUMAN = 0  # Highest priority
    SYSTEM = 1  # Internal system messages
    CLAUDE_CODE = 2  # External agent (Claude Code)


class MessageSource(str):
    """Message source identifiers."""

    HUMAN = "human"
    CLAUDE_CODE = "claude-code"
    SYSTEM = "system"


@dataclass
class QueuedMessage:
    """A message waiting in the queue."""

    id: str
    content: str
    source: str  # MessageSource value
    priority: MessagePriority
    timestamp: float
    context: list[dict[str, Any]] | None = None  # Optional file context
    hint: str | None = None  # Task hint (code_generation, etc.)
    callback: Callable[[Any], None] | None = None  # Called with result

    def __lt__(self, other: QueuedMessage) -> bool:
        """Priority queue ordering: lower priority value first, then by timestamp."""
        if self.priority != other.priority:
            return self.priority < other.priority
        return self.timestamp < other.timestamp


@dataclass
class PreemptionState:
    """State saved when a task is preempted."""

    message_id: str
    partial_content: str
    partial_tool_calls: list[dict[str, Any]] = field(default_factory=list)
    preempted_at: float = 0.0

    def __post_init__(self) -> None:
        if self.preempted_at == 0.0:
            self.preempted_at = time.time()


class MessageQueue:
    """
    Priority message queue for coordinating human and Claude Code input.

    Features:
    - Priority ordering (human > system > claude-code)
    - Preemption support for pausing lower-priority tasks
    - Async callbacks for task completion
    - Queue depth monitoring
    """

    def __init__(self, max_size: int = 100) -> None:
        """
        Initialize message queue.

        Args:
            max_size: Maximum queue size
        """
        self._queue: list[QueuedMessage] = []
        self._max_size = max_size
        self._lock = asyncio.Lock()
        self._not_empty = asyncio.Event()

        # Preemption state
        self._preempted: dict[str, PreemptionState] = {}
        self._preemption_event = asyncio.Event()

        # Currently processing message
        self._current: QueuedMessage | None = None

        # Callbacks
        self._on_preempt: Callable[[str, PreemptionState], None] | None = None
        self._on_resume: Callable[[str], None] | None = None

    def set_callbacks(
        self,
        on_preempt: Callable[[str, PreemptionState], None] | None = None,
        on_resume: Callable[[str], None] | None = None,
    ) -> None:
        """Set callbacks for preemption events."""
        self._on_preempt = on_preempt
        self._on_resume = on_resume

    async def put(
        self,
        content: str,
        source: str,
        priority: MessagePriority | None = None,
        context: list[dict[str, Any]] | None = None,
        hint: str | None = None,
        callback: Callable[[Any], None] | None = None,
    ) -> str:
        """
        Add a message to the queue.

        Args:
            content: Message content
            source: Message source (human, claude-code, system)
            priority: Optional explicit priority (defaults based on source)
            context: Optional file context to include
            hint: Optional task hint
            callback: Optional callback for when task completes

        Returns:
            Message ID
        """
        # Determine priority from source if not specified
        if priority is None:
            priority = self._priority_for_source(source)

        message = QueuedMessage(
            id=str(uuid.uuid4()),
            content=content,
            source=source,
            priority=priority,
            timestamp=time.time(),
            context=context,
            hint=hint,
            callback=callback,
        )

        async with self._lock:
            if len(self._queue) >= self._max_size:
                raise QueueFullError(f"Queue full (max {self._max_size})")

            # Insert maintaining priority order
            self._queue.append(message)
            self._queue.sort()

            logger.debug(
                f"Queued message {message.id[:8]} from {source} "
                f"(priority={priority.name}, depth={len(self._queue)})"
            )

            # Check if we should preempt current task
            if self._current and priority < self._current.priority:
                logger.info(
                    f"Preempting {self._current.source} task for {source} input"
                )
                self._preemption_event.set()

            self._not_empty.set()

        return message.id

    async def get(self) -> QueuedMessage:
        """
        Get the next message from the queue.

        Blocks until a message is available.

        Returns:
            Next highest-priority message
        """
        while True:
            await self._not_empty.wait()

            async with self._lock:
                if self._queue:
                    message = self._queue.pop(0)
                    self._current = message

                    if not self._queue:
                        self._not_empty.clear()

                    logger.debug(
                        f"Dequeued message {message.id[:8]} from {message.source}"
                    )
                    return message

                self._not_empty.clear()

    async def get_nowait(self) -> QueuedMessage | None:
        """
        Get the next message without blocking.

        Returns:
            Next message or None if queue is empty
        """
        async with self._lock:
            if self._queue:
                message = self._queue.pop(0)
                self._current = message

                if not self._queue:
                    self._not_empty.clear()

                return message
        return None

    def mark_complete(self, message_id: str, result: Any = None) -> None:
        """
        Mark a message as complete.

        Args:
            message_id: Message ID
            result: Result to pass to callback
        """
        if self._current and self._current.id == message_id:
            if self._current.callback:
                try:
                    self._current.callback(result)
                except Exception as e:
                    logger.error(f"Error in message callback: {e}")
            self._current = None
            logger.debug(f"Completed message {message_id[:8]}")

        # Remove from preempted if present
        self._preempted.pop(message_id, None)

    def preempt_current(self, partial_content: str = "", partial_tool_calls: list | None = None) -> PreemptionState | None:
        """
        Preempt the currently processing message.

        Args:
            partial_content: Partial response content
            partial_tool_calls: Partial tool calls

        Returns:
            PreemptionState if a message was preempted, None otherwise
        """
        if not self._current:
            return None

        state = PreemptionState(
            message_id=self._current.id,
            partial_content=partial_content,
            partial_tool_calls=partial_tool_calls or [],
        )

        self._preempted[self._current.id] = state

        if self._on_preempt:
            self._on_preempt(self._current.id, state)

        logger.info(f"Preempted message {self._current.id[:8]}")

        # Don't clear _current - the message handler will re-queue after preemption
        return state

    async def requeue_preempted(self, message_id: str) -> bool:
        """
        Re-queue a preempted message to continue processing.

        Args:
            message_id: ID of preempted message

        Returns:
            True if message was re-queued
        """
        state = self._preempted.get(message_id)
        if not state:
            return False

        # Find original message (might need to store it)
        # For now, we rely on the caller to re-queue
        if self._on_resume:
            self._on_resume(message_id)

        return True

    def get_preemption_state(self, message_id: str) -> PreemptionState | None:
        """Get preemption state for a message."""
        return self._preempted.get(message_id)

    def clear_preemption(self) -> None:
        """Clear preemption event flag."""
        self._preemption_event.clear()

    async def wait_for_preemption(self, timeout: float | None = None) -> bool:
        """
        Wait for a preemption event.

        Args:
            timeout: Optional timeout in seconds

        Returns:
            True if preemption occurred, False if timeout
        """
        try:
            await asyncio.wait_for(self._preemption_event.wait(), timeout)
            return True
        except asyncio.TimeoutError:
            return False

    @property
    def is_preemption_requested(self) -> bool:
        """Check if preemption has been requested."""
        return self._preemption_event.is_set()

    def _priority_for_source(self, source: str) -> MessagePriority:
        """Get default priority for a message source."""
        if source == MessageSource.HUMAN:
            return MessagePriority.HUMAN
        elif source == MessageSource.SYSTEM:
            return MessagePriority.SYSTEM
        else:
            return MessagePriority.CLAUDE_CODE

    @property
    def depth(self) -> int:
        """Get current queue depth."""
        return len(self._queue)

    @property
    def is_empty(self) -> bool:
        """Check if queue is empty."""
        return len(self._queue) == 0

    @property
    def current_message(self) -> QueuedMessage | None:
        """Get the currently processing message."""
        return self._current

    def get_queue_status(self) -> dict[str, Any]:
        """Get queue status for MCP status endpoint."""
        return {
            "depth": self.depth,
            "current": {
                "id": self._current.id,
                "source": self._current.source,
                "priority": self._current.priority.name,
            } if self._current else None,
            "preempted_count": len(self._preempted),
        }


class QueueFullError(Exception):
    """Raised when queue is full."""
    pass
