"""
Abstract backend interface and common utilities.

This module defines the interface that all backends must implement
and provides common utilities for token counting, prompt formatting, etc.
"""

from dataclasses import dataclass, field

# Re-export from core.base for convenience
from entropi.core.base import (
    GenerationResult,
    Message,
    ModelBackend,
    ToolCall,
)


@dataclass
class GenerationConfig:
    """Configuration for a single generation."""

    max_tokens: int = 4096
    temperature: float = 0.7
    top_p: float = 0.9
    top_k: int = 40
    repeat_penalty: float = 1.1
    stop: list[str] = field(default_factory=list)
    stream: bool = False


@dataclass
class TokenUsage:
    """Token usage statistics."""

    prompt_tokens: int = 0
    completion_tokens: int = 0

    @property
    def total_tokens(self) -> int:
        """Total tokens used."""
        return self.prompt_tokens + self.completion_tokens


__all__ = [
    "GenerationConfig",
    "GenerationResult",
    "Message",
    "ModelBackend",
    "TokenUsage",
    "ToolCall",
]
