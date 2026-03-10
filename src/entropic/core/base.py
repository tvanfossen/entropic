"""
Base abstract classes for extensibility.

All major components inherit from these bases to ensure
consistent interfaces and enable dependency injection.
"""

from abc import ABC, abstractmethod
from collections.abc import AsyncIterator, Sequence
from dataclasses import dataclass, field
from enum import Enum
from typing import TYPE_CHECKING, Any, Protocol, runtime_checkable

if TYPE_CHECKING:
    from entropic.config.schema import ModelConfig
    from entropic.inference.adapters.base import ChatAdapter


class ModelTier:
    """Base class for model tiers. Subclass to add domain-specific metadata.

    The engine requires every tier to declare focus points for router
    classification. This ensures the classification prompt and grammar
    are always auto-generated correctly.
    """

    def __init__(self, name: str, *, focus: Sequence[str], examples: Sequence[str] = ()) -> None:
        if not focus:
            raise ValueError(f"ModelTier '{name}' requires at least one focus point")
        self._name = name
        self._focus = tuple(focus)
        self._examples = tuple(examples)

    @property
    def name(self) -> str:
        """Tier name (e.g. 'lead', 'eng', 'arch')."""
        return self._name

    @property
    def focus(self) -> tuple[str, ...]:
        """Capability descriptions used to build classification prompt."""
        return self._focus

    @property
    def examples(self) -> tuple[str, ...]:
        """Few-shot examples for classification (from identity frontmatter)."""
        return self._examples

    def __eq__(self, other: object) -> bool:
        if isinstance(other, ModelTier):
            return self._name == other._name
        if isinstance(other, str):
            return self._name == other
        return NotImplemented

    def __hash__(self) -> int:
        return hash(self._name)

    def __str__(self) -> str:
        return self._name

    def __repr__(self) -> str:
        return f"ModelTier({self._name!r})"


@dataclass
class Message:
    """A message in a conversation."""

    role: str  # "user", "assistant", "system", "tool"
    content: str
    tool_calls: list[dict[str, Any]] = field(default_factory=list)
    tool_results: list[dict[str, Any]] = field(default_factory=list)
    metadata: dict[str, Any] = field(default_factory=dict)


@dataclass
class ToolCall:
    """A tool call request."""

    id: str
    name: str
    arguments: dict[str, Any]


@dataclass
class ToolResult:
    """Result of a tool execution."""

    call_id: str
    name: str
    result: str
    is_error: bool = False
    duration_ms: int = 0
    directives: list[Any] = field(default_factory=list)


@dataclass
class GenerationResult:
    """Result of a generation."""

    content: str
    raw_content: str = ""
    tool_calls: list[ToolCall] = field(default_factory=list)
    finish_reason: str = "stop"
    token_count: int = 0
    generation_time_ms: int = 0
    logprobs: list[dict[str, Any]] | None = None  # Per-token log-probabilities
    # Engine timing — populated by orchestrator during generate()
    routing_ms: float = 0.0
    swap_ms: float = 0.0
    total_ms: float = 0.0


class ModelState(Enum):
    """Three-state model lifecycle.

    COLD  — on disk only, no RAM consumed.
    WARM  — mmap'd + mlock'd in system RAM, n_gpu_layers=0.
            Inference possible but slow (~0.5–2 tok/s).
    ACTIVE — N layers on GPU, full inference speed.

    Subsystem initialization:
        COLD → WARM: warm()          ~1–2s (demand paging, one-time)
        WARM → ACTIVE: activate()    ~1–3s (PCIe transfer, no disk I/O)
        ACTIVE → WARM: deactivate()  ~0.5–1s (no PCIe transfer needed)
        WARM/ACTIVE → COLD: unload() negligible
    """

    COLD = "cold"
    WARM = "warm"
    ACTIVE = "active"


class ModelBackend(ABC):
    """Abstract base class for model backends."""

    # Subclasses must define these attributes
    config: "ModelConfig"
    _adapter: "ChatAdapter"

    @property
    @abstractmethod
    def state(self) -> ModelState:
        """Current lifecycle state of the model."""
        ...

    @abstractmethod
    async def warm(self) -> None:
        """Load model into CPU RAM only (n_gpu_layers=0, mmap+mlock).

        COLD → WARM. No-op if already WARM or ACTIVE.
        """
        ...

    @abstractmethod
    async def activate(self, gpu_layers: int = -1) -> None:
        """Promote model to GPU (WARM/COLD → ACTIVE).

        If COLD, warms first. No-op if already ACTIVE.

        Args:
            gpu_layers: Number of layers to offload to GPU. -1 = all layers.
        """
        ...

    @abstractmethod
    async def deactivate(self) -> None:
        """Release GPU layers, return to WARM (ACTIVE → WARM).

        No-op if not ACTIVE.
        """
        ...

    @abstractmethod
    async def load(self) -> None:
        """Load the model. Convenience wrapper: warm() + activate()."""
        pass

    @abstractmethod
    async def unload(self) -> None:
        """Unload the model fully (→ COLD). Releases all RAM and VRAM."""
        pass

    @abstractmethod
    async def generate(
        self,
        messages: list[Message],
        max_tokens: int = 4096,
        stop: list[str] | None = None,
        **kwargs: Any,
    ) -> GenerationResult:
        """Generate a response."""
        pass

    @abstractmethod
    def generate_stream(
        self,
        messages: list[Message],
        max_tokens: int = 4096,
        stop: list[str] | None = None,
        **kwargs: Any,
    ) -> AsyncIterator[str]:
        """Generate a streaming response (async generator)."""
        ...

    @abstractmethod
    def count_tokens(self, text: str) -> int:
        """Count tokens in text."""
        pass

    @property
    @abstractmethod
    def context_length(self) -> int:
        """Get the model's context length."""
        pass

    @abstractmethod
    async def complete(
        self,
        prompt: str,
        max_tokens: int = 16,
        grammar: str | None = None,
        **kwargs: Any,
    ) -> "GenerationResult":
        """Raw text completion (no chat template).

        Used for classification and other tasks where the model should
        continue a prompt directly without chat scaffolding.
        """
        pass

    @property
    def is_loaded(self) -> bool:
        """Returns True when model is ACTIVE (backward compatibility)."""
        return self.state == ModelState.ACTIVE

    @property
    @abstractmethod
    def last_finish_reason(self) -> str:
        """Get the finish_reason from the last generation."""
        pass

    @property
    def adapter(self) -> "ChatAdapter":
        """Get the adapter for this backend."""
        return self._adapter


@runtime_checkable
class ToolProvider(Protocol):
    """Structural interface for tool providers.

    Both MCPClient (subprocess JSON-RPC) and InProcessProvider
    (direct in-process execution) implement this protocol,
    letting ServerManager treat them uniformly.
    """

    name: str

    @property
    def is_connected(self) -> bool: ...

    async def connect(self) -> None: ...

    async def disconnect(self) -> None: ...

    async def list_tools(self) -> list[dict[str, Any]]: ...

    async def execute(
        self,
        tool_name: str,
        arguments: dict[str, Any],
    ) -> ToolResult: ...


class StorageBackend(ABC):
    """Abstract base class for storage backends."""

    @abstractmethod
    async def initialize(self) -> None:
        """Initialize storage."""
        pass

    @abstractmethod
    async def close(self) -> None:
        """Close storage."""
        pass

    @abstractmethod
    async def save_conversation(
        self,
        conversation_id: str,
        messages: list[Message],
        metadata: dict[str, Any] | None = None,
    ) -> None:
        """Save a conversation."""
        pass

    @abstractmethod
    async def load_conversation(
        self,
        conversation_id: str,
    ) -> tuple[list[Message], dict[str, Any]]:
        """Load a conversation."""
        pass

    @abstractmethod
    async def list_conversations(
        self,
        limit: int = 100,
        offset: int = 0,
    ) -> list[dict[str, Any]]:
        """List conversations."""
        pass

    @abstractmethod
    async def search_conversations(
        self,
        query: str,
        limit: int = 10,
    ) -> list[dict[str, Any]]:
        """Search conversations."""
        pass
