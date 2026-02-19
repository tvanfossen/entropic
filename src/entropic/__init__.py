"""
Entropi - Local AI inference engine with multi-tier model orchestration.

Public API for library consumers. Install extras for additional features:
  pip install entropic-engine          # Core inference engine
  pip install entropic-engine[tui]     # Terminal UI application
  pip install entropic-engine[voice]   # Voice interface
"""

from entropic.config.loader import ConfigLoader, save_permission, validate_config
from entropic.config.schema import (
    CompactionConfig,
    EntropyConfig,
    GenerationConfig,
    LibraryConfig,
    ModelConfig,
    ModelsConfig,
    RoutingConfig,
    TierConfig,
)
from entropic.core.base import (
    GenerationResult,
    Message,
    ModelBackend,
    ModelTier,
    ToolCall,
    ToolProvider,
    ToolResult,
)
from entropic.core.engine import AgentEngine, AgentState, EngineCallbacks, LoopConfig
from entropic.core.logging import setup_logging, setup_model_logger
from entropic.core.tool_validation import ToolValidationError
from entropic.inference.adapters import ChatAdapter, get_adapter, register_adapter
from entropic.inference.orchestrator import BackendFactory, ModelOrchestrator, RoutingResult
from entropic.mcp.manager import ServerManager
from entropic.mcp.provider import InProcessProvider
from entropic.mcp.servers.base import BaseMCPServer, ServerResponse, load_tool_definition
from entropic.mcp.tools import BaseTool, ToolRegistry
from entropic.prompts import TierIdentity, load_tier_identity

__version__ = "1.0.0"
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
    "save_permission",
    "validate_config",
    "CompactionConfig",
    "EntropyConfig",
    "GenerationConfig",
    "LibraryConfig",
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
