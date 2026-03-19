"""
Entropic - Local AI inference engine with multi-tier model orchestration.

Public API for library consumers:
  pip install entropic-engine          # Core inference engine
  pip install entropic-tui             # Terminal UI (separate package)
"""

from entropic.config.loader import ConfigLoader, save_permission, validate_config
from entropic.config.schema import (
    CompactionConfig,
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
from entropic.core.headless_presenter import HeadlessPresenter
from entropic.core.logging import setup_display_logger, setup_logging, setup_model_logger
from entropic.core.presenter import Presenter, StatusInfo
from entropic.core.tool_validation import ToolValidationError
from entropic.inference.adapters import ChatAdapter, get_adapter, register_adapter
from entropic.inference.orchestrator import BackendFactory, ModelOrchestrator, RoutingResult
from entropic.mcp.manager import ServerManager
from entropic.mcp.provider import InProcessProvider
from entropic.mcp.servers.base import BaseMCPServer, ServerResponse, load_tool_definition
from entropic.mcp.tools import BaseTool, ToolRegistry
from entropic.prompts import (
    AppContextFrontmatter,
    ConstitutionFrontmatter,
    IdentityFrontmatter,
    PromptFrontmatter,
    TierIdentity,
    load_tier_identity,
    parse_prompt_file,
)
from entropic.prompts.manager import PromptManager

try:
    from importlib.metadata import version as _get_version

    __version__ = _get_version("entropic-engine")
except Exception:
    __version__ = "0.0.0"  # fallback for editable installs without metadata
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
    "setup_display_logger",
    "setup_logging",
    "setup_model_logger",
    # Config
    "ConfigLoader",
    "save_permission",
    "validate_config",
    "CompactionConfig",
    "HeadlessPresenter",
    "Presenter",
    "StatusInfo",
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
    "AppContextFrontmatter",
    "ConstitutionFrontmatter",
    "IdentityFrontmatter",
    "PromptFrontmatter",
    "TierIdentity",
    "load_tier_identity",
    "parse_prompt_file",
    "PromptManager",
]
