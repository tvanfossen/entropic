"""
Configuration schema using Pydantic.

Principles:
- All config values have sensible defaults
- Validation happens at load time
- Immutable after loading (frozen=True where appropriate)
"""

from pathlib import Path
from typing import Any, Literal

from pydantic import BaseModel, Field, field_validator
from pydantic_settings import BaseSettings, SettingsConfigDict


class ModelConfig(BaseModel):
    """Configuration for a single model."""

    path: Path
    adapter: str = "qwen2"  # Adapter name: qwen2, qwen3, generic
    context_length: int = Field(default=16384, ge=512, le=131072)
    gpu_layers: int = Field(default=-1)  # -1 = all layers
    temperature: float = Field(default=0.7, ge=0.0, le=2.0)
    top_p: float = Field(default=0.9, ge=0.0, le=1.0)
    top_k: int = Field(default=40, ge=0)
    repeat_penalty: float = Field(default=1.1, ge=1.0, le=2.0)

    @field_validator("path")
    @classmethod
    def validate_path(cls, v: Path) -> Path:
        """Expand user path."""
        return Path(v).expanduser()


class ModelsConfig(BaseModel):
    """Configuration for all models."""

    # Task-specialized model slots
    thinking: ModelConfig | None = None  # Deep reasoning (e.g., Qwen3-14B)
    normal: ModelConfig | None = None  # General reasoning (e.g., Qwen3-8B)
    code: ModelConfig | None = None  # Code generation (e.g., Qwen2.5-Coder-7B)
    simple: ModelConfig | None = None  # Simple responses - can share model with normal
    router: ModelConfig | None = None  # Classification only (small model, e.g., 0.5B)

    # Default model to use if routing disabled
    default: Literal["thinking", "normal", "code", "simple"] = "normal"


class RoutingConfig(BaseModel):
    """Configuration for model routing.

    Task classification is handled by the ROUTER model with GBNF grammar
    constraint - no keyword heuristics needed. This provides more accurate
    and maintainable routing.

    The ROUTER model is used ONLY for classification. Response generation
    is handled by the dedicated tier for each task type:
    - SIMPLE tasks -> simple model
    - CODE tasks -> code model
    - REASONING tasks -> normal model (or thinking if forced)
    - COMPLEX tasks -> thinking model
    """

    enabled: bool = True
    fallback_model: Literal["thinking", "normal", "code", "simple"] = "normal"


class ThinkingConfig(BaseModel):
    """Configuration for thinking mode."""

    enabled: bool = False  # Default to OFF (use normal reasoning model)
    auto_enable_keywords: list[str] = Field(
        default_factory=lambda: [
            "architect",
            "design",
            "think through",
            "complex",
            "deeply",
        ]
    )
    swap_timeout_seconds: int = Field(default=10, ge=1, le=60)


class QualityRulesConfig(BaseModel):
    """Code quality enforcement rules."""

    # Complexity
    max_cognitive_complexity: int = Field(default=15, ge=1, le=50)
    max_cyclomatic_complexity: int = Field(default=10, ge=1, le=30)

    # Size
    max_function_lines: int = Field(default=50, ge=10, le=200)
    max_file_lines: int = Field(default=500, ge=100, le=2000)
    max_parameters: int = Field(default=5, ge=2, le=10)
    max_returns_per_function: int = Field(default=3, ge=1, le=10)

    # Structure
    require_type_hints: bool = True
    require_docstrings: bool = True
    require_return_type: bool = True

    # Style
    docstring_style: Literal["google", "numpy", "sphinx"] = "google"
    enforce_snake_case_functions: bool = True
    enforce_pascal_case_classes: bool = True


class QualityConfig(BaseModel):
    """Quality enforcement configuration."""

    enabled: bool = True
    max_regeneration_attempts: int = Field(default=3, ge=1, le=5)
    rules: QualityRulesConfig = Field(default_factory=QualityRulesConfig)

    # Per-language overrides
    language_overrides: dict[str, QualityRulesConfig] = Field(default_factory=dict)


class PermissionsConfig(BaseModel):
    """Tool permission configuration.

    Permissions are built dynamically per-project:
    - No hardcoded defaults - empty allow/deny lists initially
    - Unknown tools prompt the user for approval
    - "Always allow" saves to local config, "Yes" is one-time approval
    """

    # Explicitly allowed tools/patterns (glob patterns)
    # Built up dynamically when user selects "Always allow"
    allow: list[str] = Field(default_factory=list)

    # Explicitly denied tools/patterns (glob patterns)
    # Built up dynamically when user selects "Always deny"
    deny: list[str] = Field(default_factory=list)

    # Auto-approve all tool calls (skip confirmation prompts)
    auto_approve: bool = False


class UIConfig(BaseModel):
    """Terminal UI configuration."""

    theme: Literal["dark", "light", "auto"] = "dark"
    stream_output: bool = True
    show_token_count: bool = True
    show_timing: bool = True
    max_output_lines: int = Field(default=100, ge=10, le=1000)


class StorageConfig(BaseModel):
    """Storage configuration."""

    database_path: Path = Field(default_factory=lambda: Path.home() / ".entropi" / "history.db")
    max_conversations: int = Field(default=1000, ge=10)
    auto_save: bool = True

    @field_validator("database_path")
    @classmethod
    def validate_database_path(cls, v: Path) -> Path:
        """Expand user path."""
        return Path(v).expanduser()


class MCPConfig(BaseModel):
    """MCP server configuration."""

    # Built-in servers to enable
    enable_filesystem: bool = True
    enable_bash: bool = True
    enable_git: bool = True
    enable_diagnostics: bool = True  # LSP diagnostics tool

    # External MCP servers (from .mcp.json)
    external_servers: dict[str, dict[str, Any]] = Field(default_factory=dict)

    # Server timeout
    server_timeout_seconds: int = Field(default=30, ge=5, le=300)


class CompactionConfig(BaseModel):
    """Auto-compaction configuration."""

    enabled: bool = True
    threshold_percent: float = Field(default=0.90, ge=0.5, le=0.99)
    preserve_recent_turns: int = Field(default=2, ge=1, le=10)
    summary_max_tokens: int = Field(default=1500, ge=500, le=4000)
    notify_user: bool = True
    save_full_history: bool = True


class GenerationConfig(BaseModel):
    """Generation parameters configuration."""

    max_tokens: int = Field(default=4096, ge=64, le=32768)
    default_temperature: float = Field(default=0.7, ge=0.0, le=2.0)
    default_top_p: float = Field(default=0.9, ge=0.0, le=1.0)


class LSPServerConfig(BaseModel):
    """Configuration for a single LSP server."""

    command: str
    args: list[str] = Field(default_factory=list)
    extensions: list[str] = Field(default_factory=list)


class LSPConfig(BaseModel):
    """LSP integration configuration.

    Currently supports Python (pyright) and C (clangd).
    """

    enabled: bool = True

    # Per-language enable/disable
    python_enabled: bool = True
    c_enabled: bool = True

    # Custom server overrides (optional)
    servers: dict[str, LSPServerConfig] = Field(default_factory=dict)


class EntropyConfig(BaseSettings):
    """Root configuration for Entropi."""

    model_config = SettingsConfigDict(
        env_prefix="ENTROPI_",
        env_nested_delimiter="__",
        extra="ignore",
    )

    # Sub-configurations
    models: ModelsConfig = Field(default_factory=ModelsConfig)
    routing: RoutingConfig = Field(default_factory=RoutingConfig)
    thinking: ThinkingConfig = Field(default_factory=ThinkingConfig)
    generation: GenerationConfig = Field(default_factory=GenerationConfig)
    quality: QualityConfig = Field(default_factory=QualityConfig)
    permissions: PermissionsConfig = Field(default_factory=PermissionsConfig)
    ui: UIConfig = Field(default_factory=UIConfig)
    storage: StorageConfig = Field(default_factory=StorageConfig)
    mcp: MCPConfig = Field(default_factory=MCPConfig)
    compaction: CompactionConfig = Field(default_factory=CompactionConfig)
    lsp: LSPConfig = Field(default_factory=LSPConfig)

    # Logging
    log_level: Literal["DEBUG", "INFO", "WARNING", "ERROR"] = "INFO"
    log_file: Path | None = None

    # Paths - use default_factory to ensure Path.home() is used
    config_dir: Path = Field(default_factory=lambda: Path.home() / ".entropi")
    prompts_dir: Path = Field(default_factory=lambda: Path.home() / ".entropi" / "prompts")
    commands_dir: Path = Field(default_factory=lambda: Path.home() / ".entropi" / "commands")

    @field_validator("config_dir", "prompts_dir", "commands_dir")
    @classmethod
    def validate_paths(cls, v: Path) -> Path:
        """Expand user paths if ~ is used."""
        return Path(v).expanduser()
