"""
Shared types for the agentic loop engine.

Dataclasses, enums, and callback containers used across engine
subsystems (ToolExecutor, ResponseGenerator, ContextManager).
"""

from __future__ import annotations

import threading
from collections.abc import Callable
from dataclasses import dataclass, field
from enum import Enum, auto
from typing import TYPE_CHECKING, Any

from entropic.core.queue import MessageSource

if TYPE_CHECKING:
    from entropic.core.base import ToolCall
    from entropic.core.compaction import CompactionResult
    from entropic.inference.orchestrator import RoutingResult


class AgentState(Enum):
    """Agent execution states."""

    IDLE = auto()
    PLANNING = auto()
    EXECUTING = auto()
    WAITING_TOOL = auto()
    VERIFYING = auto()
    DELEGATING = auto()  # Waiting for child delegation to complete
    COMPLETE = auto()
    ERROR = auto()
    INTERRUPTED = auto()
    PAUSED = auto()  # Generation paused, awaiting user input


class InterruptMode(Enum):
    """How to handle generation interrupt."""

    CANCEL = "cancel"  # Discard partial response, stop
    PAUSE = "pause"  # Keep partial response, await input
    INJECT = "inject"  # Keep partial, inject context, continue


class ToolApproval(Enum):
    """Tool approval responses from user."""

    DENY = "deny"  # Deny this once
    ALLOW = "allow"  # Allow this once
    ALWAYS_DENY = "always_deny"  # Deny and save to config
    ALWAYS_ALLOW = "always_allow"  # Allow and save to config


@dataclass
class LoopConfig:
    """Configuration for the agentic loop."""

    max_iterations: int = 15
    max_consecutive_errors: int = 3
    max_tool_calls_per_turn: int = 10
    idle_timeout_seconds: int = 300
    require_plan_for_complex: bool = True
    stream_output: bool = True
    auto_approve_tools: bool = False


@dataclass
class LoopMetrics:
    """Metrics collected during loop execution."""

    iterations: int = 0
    tool_calls: int = 0
    tokens_used: int = 0
    errors: int = 0
    start_time: float = 0.0
    end_time: float = 0.0

    @property
    def duration_ms(self) -> int:
        """Get duration in milliseconds."""
        return int((self.end_time - self.start_time) * 1000)


@dataclass
class LoopContext:
    """Context maintained during loop execution."""

    messages: list[Any] = field(default_factory=list)  # list[Message]
    pending_tool_calls: list[Any] = field(default_factory=list)  # list[ToolCall]
    state: AgentState = AgentState.IDLE
    metrics: LoopMetrics = field(default_factory=LoopMetrics)
    consecutive_errors: int = 0
    # Track recent tool calls to prevent duplicates (key: "name:args_hash")
    recent_tool_calls: dict[str, str] = field(default_factory=dict)
    # Track consecutive duplicate attempts to detect stuck model
    consecutive_duplicate_attempts: int = 0
    # Count of tool calls that actually executed this iteration
    # (not blocked, not denied, not duplicate). Reset each iteration.
    effective_tool_calls: int = 0
    # Flag indicating we have tool results that should be presented
    has_pending_tool_results: bool = False
    # Lock model tier for the entire loop to prevent mid-task switching
    locked_tier: Any = None  # ModelTier or None
    # External task tracking (for MCP integration)
    task_id: str | None = None  # Associated task ID if from external source
    source: str = MessageSource.HUMAN  # Message source (human, claude-code)
    # Stored for system prompt rebuild on tier change
    all_tools: list[dict[str, Any]] = field(default_factory=list)
    base_system: str = ""
    # General-purpose metadata for runtime state (e.g., warning tracking)
    metadata: dict[str, Any] = field(default_factory=dict)
    # Delegation: 0 = root context, 1+ = child delegation depth
    delegation_depth: int = 0
    # Delegation: parent conversation ID (if this is a child context)
    parent_conversation_id: str | None = None
    # Delegation: child conversation IDs spawned from this context
    child_conversation_ids: list[str] = field(default_factory=list)


@dataclass
class InterruptContext:
    """Context for interrupted/paused generation."""

    partial_content: str = ""
    partial_tool_calls: list[Any] = field(default_factory=list)  # list[ToolCall]
    injection: str | None = None
    mode: InterruptMode = InterruptMode.PAUSE


@dataclass
class EngineCallbacks:
    """Callback configuration for engine events.

    Shared mutable container — all subsystems hold a reference to the
    same instance. Engine.set_callbacks() updates fields in place.
    """

    on_state_change: Callable[[AgentState], None] | None = None
    on_tool_call: Callable[[ToolCall], Any] | None = None
    on_stream_chunk: Callable[[str], None] | None = None
    on_tool_start: Callable[[ToolCall], None] | None = None
    on_tool_complete: Callable[[ToolCall, str, float], None] | None = None
    on_presenter_notify: Callable[[str, dict[str, Any]], None] | None = None
    on_compaction: Callable[[CompactionResult], None] | None = None
    on_pause_prompt: Callable[[str], Any] | None = None
    on_tool_record: Callable[[str, ToolCall, str, str | None, float], None] | None = None
    on_tier_selected: Callable[[str], None] | None = None
    on_routing_complete: Callable[[RoutingResult], None] | None = None
    on_delegation_start: Callable[[str, str, str], None] | None = None
    """(child_conv_id, target_tier, task)"""
    on_delegation_complete: Callable[[str, str, str, bool], None] | None = None
    """(child_conv_id, tier, summary, success)"""
    error_sanitizer: Callable[[str], str] | None = None
    repo_init: Callable[[Any], bool] | None = None
    """Initialize a VCS repo at a Path for worktree isolation.

    Called when the project directory has no ``.git``.  Receives the
    project directory (Path).  Returns True if init succeeded and
    worktrees should be enabled, False to skip worktree isolation.

    Default (None): ``git init`` the directory.
    Override to use a different VCS, add initial commits, or disable.
    """


@dataclass
class GenerationEvents:
    """Threading events passed to ResponseGenerator for interrupt/pause signaling."""

    interrupt: threading.Event
    """Hard interrupt — breaks out of the streaming/generation loop."""

    pause: threading.Event
    """Pause signal — pauses generation and prompts for user injection."""
