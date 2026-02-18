"""
Entropi - Local AI inference engine with multi-tier model orchestration.

Public API for library consumers. Install extras for additional features:
  pip install entropi          # Core inference engine
  pip install entropi[tui]     # Terminal UI application
  pip install entropi[voice]   # Voice interface
"""

from entropi.config.loader import ConfigLoader
from entropi.config.schema import (
    CompactionConfig,
    EntropyConfig,
    GenerationConfig,
    ModelConfig,
    ModelsConfig,
    RoutingConfig,
    TierConfig,
)
from entropi.core.base import (
    GenerationResult,
    Message,
    ModelBackend,
    ModelTier,
    ToolCall,
    ToolProvider,
    ToolResult,
)
from entropi.core.engine import AgentEngine, AgentState, EngineCallbacks, LoopConfig
from entropi.core.logging import setup_logging, setup_model_logger
from entropi.core.tool_validation import ToolValidationError
from entropi.inference.adapters import ChatAdapter, get_adapter, register_adapter
from entropi.inference.orchestrator import BackendFactory, ModelOrchestrator, RoutingResult
from entropi.mcp.manager import ServerManager
from entropi.mcp.provider import InProcessProvider
from entropi.mcp.servers.base import BaseMCPServer, ServerResponse, load_tool_definition
from entropi.mcp.tools import BaseTool, ToolRegistry
from entropi.prompts import TierIdentity, load_tier_identity

__version__ = "0.1.0"
__author__ = "Tristan VanFossen"

__all__ = [
    # Core types
    "GenerationResult",
    "Message",
    "ModelBackend",
    "ModelTier",
    "ToolCall",
    "ToolProvider",
    "ToolResult",
    # Engine
    "AgentEngine",
    "AgentState",
    "EngineCallbacks",
    "LoopConfig",
    # Logging
    "setup_logging",
    "setup_model_logger",
    # Config
    "ConfigLoader",
    "CompactionConfig",
    "EntropyConfig",
    "GenerationConfig",
    "ModelConfig",
    "ModelsConfig",
    "RoutingConfig",
    "TierConfig",
    # Orchestrator
    "BackendFactory",
    "ModelOrchestrator",
    "RoutingResult",
    # Adapters
    "ChatAdapter",
    "get_adapter",
    "register_adapter",
    # MCP
    "BaseMCPServer",
    "BaseTool",
    "InProcessProvider",
    "ServerManager",
    "ServerResponse",
    "ToolRegistry",
    "ToolValidationError",
    "load_tool_definition",
    # Prompts
    "TierIdentity",
    "load_tier_identity",
]
