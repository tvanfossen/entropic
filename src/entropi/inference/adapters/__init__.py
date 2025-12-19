"""Model adapters for different chat formats."""

from entropi.inference.adapters.base import ChatAdapter
from entropi.inference.adapters.qwen import QwenAdapter

__all__ = [
    "ChatAdapter",
    "QwenAdapter",
]
