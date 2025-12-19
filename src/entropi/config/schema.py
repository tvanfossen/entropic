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

    primary: ModelConfig | None = None
    workhorse: ModelConfig | None = None
    fast: ModelConfig | None = None
    micro: ModelConfig | None = None

    # Default model to use if routing disabled
    default: Literal["primary", "workhorse", "fast", "micro"] = "primary"


class RoutingConfig(BaseModel):
    """Configuration for model routing."""

    enabled: bool = True
    use_heuristics: bool = True
    fallback_model: Literal["primary", "workhorse", "fast", "micro"] = "primary"

    # Thresholds for heuristic routing
    simple_query_max_tokens: int = 50
    complex_keywords: list[str] = Field(
        default_factory=lambda: [
            "implement",
            "refactor",
            "design",
            "architect",
            "review",
            "analyze",
        ]
    )


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
    """Tool permission configuration."""

    allow: list[str] = Field(
        default_factory=lambda: ["filesystem.*", "git.*", "bash.execute:pytest *"]
    )
    deny: list[str] = Field(default_factory=lambda: ["bash.execute:rm -rf *"])

    # Prompt before executing these patterns
    prompt: list[str] = Field(default_factory=lambda: ["bash.execute:*", "filesystem.write_file:*"])


class UIConfig(BaseModel):
    """Terminal UI configuration."""

    theme: Literal["dark", "light", "auto"] = "dark"
    stream_output: bool = True
    show_token_count: bool = True
    show_timing: bool = True
    max_output_lines: int = Field(default=100, ge=10, le=1000)


class StorageConfig(BaseModel):
    """Storage configuration."""

    database_path: Path = Field(default=Path("~/.entropi/history.db"))
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

    # External MCP servers (from .mcp.json)
    external_servers: dict[str, dict[str, Any]] = Field(default_factory=dict)

    # Server timeout
    server_timeout_seconds: int = Field(default=30, ge=5, le=300)


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
    quality: QualityConfig = Field(default_factory=QualityConfig)
    permissions: PermissionsConfig = Field(default_factory=PermissionsConfig)
    ui: UIConfig = Field(default_factory=UIConfig)
    storage: StorageConfig = Field(default_factory=StorageConfig)
    mcp: MCPConfig = Field(default_factory=MCPConfig)

    # Logging
    log_level: Literal["DEBUG", "INFO", "WARNING", "ERROR"] = "INFO"
    log_file: Path | None = None

    # Paths
    config_dir: Path = Field(default=Path("~/.entropi"))
    prompts_dir: Path = Field(default=Path("~/.entropi/prompts"))
    commands_dir: Path = Field(default=Path("~/.entropi/commands"))

    @field_validator("config_dir", "prompts_dir", "commands_dir")
    @classmethod
    def validate_paths(cls, v: Path) -> Path:
        """Expand user paths."""
        return Path(v).expanduser()
