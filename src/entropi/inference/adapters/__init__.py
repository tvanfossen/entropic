"""Model adapters for different chat formats."""

from entropi.inference.adapters.base import (
    ChatAdapter,
    GenericAdapter,
    get_adapter,
    list_adapters,
    register_adapter,
)

# Import to trigger registration
from entropi.inference.adapters.falcon import FalconAdapter
from entropi.inference.adapters.qwen2 import Qwen2Adapter
from entropi.inference.adapters.qwen3 import Qwen3Adapter

__all__ = [
    "ChatAdapter",
    "FalconAdapter",
    "GenericAdapter",
    "Qwen2Adapter",
    "Qwen3Adapter",
    "get_adapter",
    "list_adapters",
    "register_adapter",
]
