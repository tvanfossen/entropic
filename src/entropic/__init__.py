"""
Entropic - Local AI inference engine with multi-tier model orchestration.

Public API for library consumers:
  pip install entropic-engine          # Core inference engine
  pip install entropic-tui             # Terminal UI (separate package)
"""

import warnings

# === Public API (stable, documented) ===
# These symbols form the behavioral specification for the v2.0.0 C API.
from entropic.config.loader import ConfigLoader, save_permission, validate_config
from entropic.config.schema import (
    CompactionConfig,
    GenerationConfig,
    LibraryConfig,
    MCPConfig,
    ModelConfig,
    ModelsConfig,
    PermissionsConfig,
    RoutingConfig,
    TierConfig,
)
from entropic.core.base import (
    GenerationResult,
    Message,
    ModelBackend,
    ModelTier,
    ToolCall,
    ToolResult,
)
from entropic.core.engine import AgentEngine, AgentState, EngineCallbacks, LoopConfig
from entropic.core.headless_presenter import HeadlessPresenter
from entropic.core.presenter import Presenter, StatusInfo

# === Extension Points (stable, for custom backends/tools) ===
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


# === Deprecation shim (removed at v1.8.0) ===


def __getattr__(name: str) -> type:
    """Lazy import with deprecation warning for removed symbols.

    @brief Emit DeprecationWarning for EntropyConfig imports.
    @version 1
    """
    if name == "EntropyConfig":
        warnings.warn(
            "EntropyConfig has moved to the entropic-tui package. "
            "Use LibraryConfig for engine-only usage. "
            "This shim will be removed in v1.8.0.",
            DeprecationWarning,
            stacklevel=2,
        )
        return LibraryConfig
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")


__all__ = [
    # === Public API (stable, documented) ===
    # Config
    "ConfigLoader",
    "save_permission",
    "validate_config",
    "CompactionConfig",
    "GenerationConfig",
    "LibraryConfig",
    "MCPConfig",
    "ModelConfig",
    "ModelsConfig",
    "PermissionsConfig",
    "RoutingConfig",
    "TierConfig",
    # Engine
    "AgentEngine",
    "AgentState",
    "EngineCallbacks",
    "LoopConfig",
    # Types
    "GenerationResult",
    "Message",
    "ModelBackend",
    "ModelTier",
    "ToolCall",
    "ToolResult",
    # Orchestrator
    "BackendFactory",
    "ModelOrchestrator",
    "RoutingResult",
    # Presenter
    "HeadlessPresenter",
    "Presenter",
    "StatusInfo",
    # Prompts
    "AppContextFrontmatter",
    "ConstitutionFrontmatter",
    "IdentityFrontmatter",
    "PromptFrontmatter",
    "PromptManager",
    "TierIdentity",
    "load_tier_identity",
    "parse_prompt_file",
    # === Extension Points (stable, for custom backends/tools) ===
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
    "load_tool_definition",
]
