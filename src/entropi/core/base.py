"""
Base abstract classes for extensibility.

All major components inherit from these bases to ensure
consistent interfaces and enable dependency injection.
"""

from abc import ABC, abstractmethod
from collections.abc import AsyncIterator
from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Any, Protocol, runtime_checkable

if TYPE_CHECKING:
    from entropi.config.schema import ModelConfig
    from entropi.inference.adapters.base import ChatAdapter


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
    tool_calls: list[ToolCall] = field(default_factory=list)
    finish_reason: str = "stop"
    token_count: int = 0
    generation_time_ms: int = 0


class ModelBackend(ABC):
    """Abstract base class for model backends."""

    # Subclasses must define these attributes
    config: "ModelConfig"
    _adapter: "ChatAdapter"

    @abstractmethod
    async def load(self) -> None:
        """Load the model into memory."""
        pass

    @abstractmethod
    async def unload(self) -> None:
        """Unload the model from memory."""
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

    @property
    @abstractmethod
    def is_loaded(self) -> bool:
        """Check if model is loaded."""
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
