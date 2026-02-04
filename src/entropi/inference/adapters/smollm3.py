"""
SmolLM3 adapter.

SmolLM3 uses ChatML format with the same tool call conventions as Qwen3:
- <tool_call>{"name": "...", "arguments": {...}}</tool_call> for tool calls

SmolLM3 is a smaller model optimized for quick responses. It uses system.handoff
to transfer complex tasks to other tiers - no special detection needed here.
"""

from entropi.core.logging import get_logger
from entropi.inference.adapters.base import register_adapter
from entropi.inference.adapters.qwen3 import Qwen3Adapter

logger = get_logger("adapter.smollm3")


class SmolLM3Adapter(Qwen3Adapter):
    """
    Adapter for SmolLM3 models.

    Inherits from Qwen3Adapter since SmolLM3 uses the same ChatML format
    and tool call conventions. SmolLM3 may not use <think> blocks as
    extensively, but the parsing handles their absence gracefully.
    """

    pass  # All behavior inherited from Qwen3Adapter


# Register adapter
register_adapter("smollm3", SmolLM3Adapter)
