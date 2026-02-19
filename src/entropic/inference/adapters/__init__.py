"""Model adapters for different chat formats."""

from entropic.inference.adapters.base import (
    ChatAdapter,
    GenericAdapter,
    get_adapter,
    list_adapters,
    register_adapter,
)

# Import to trigger registration
from entropic.inference.adapters.falcon import FalconAdapter
from entropic.inference.adapters.qwen2 import Qwen2Adapter
from entropic.inference.adapters.qwen3 import Qwen3Adapter
from entropic.inference.adapters.router import RouterAdapter
from entropic.inference.adapters.smollm3 import SmolLM3Adapter

__all__ = [
    "ChatAdapter",
    "FalconAdapter",
    "GenericAdapter",
    "Qwen2Adapter",
    "Qwen3Adapter",
    "RouterAdapter",
    "SmolLM3Adapter",
    "get_adapter",
    "list_adapters",
    "register_adapter",
]
