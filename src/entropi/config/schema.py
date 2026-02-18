"""
Configuration schema using Pydantic.

Principles:
- All config values have sensible defaults
- Validation happens at load time
- Immutable after loading (frozen=True where appropriate)
"""

from pathlib import Path
from typing import Any, Literal

from pydantic import BaseModel, Field, field_validator, model_validator
from pydantic_settings import BaseSettings, SettingsConfigDict


class ModelConfig(BaseModel):
    """Configuration for a single model."""

    path: Path
    adapter: str = "qwen2"  # Adapter name: qwen2, qwen3, generic
    context_length: int = Field(default=16384, ge=512, le=131072)
    max_output_tokens: int = Field(default=4096, ge=1, le=32768)
    gpu_layers: int = Field(default=-1)  # -1 = all layers
    temperature: float = Field(default=0.7, ge=0.0, le=2.0)
    top_p: float = Field(default=0.9, ge=0.0, le=1.0)
    top_k: int = Field(default=40, ge=0)
    repeat_penalty: float = Field(default=1.1, ge=1.0, le=2.0)
    allowed_tools: list[str] | None = None  # None = all tools allowed

    @field_validator("path")
    @classmethod
    def validate_path(cls, v: Path) -> Path:
        """Expand user path."""
        return Path(v).expanduser()


class TierConfig(ModelConfig):
    """Model configuration for a specific tier.

    Extends ModelConfig with optional focus points. Focus can also come
    from ModelTier instances or identity file frontmatter — config is
    one of three resolution paths.
    """

    focus: list[str] = Field(default_factory=list)


class ModelsConfig(BaseModel):
    """Configuration for all models.

    Tiers are defined as a dict mapping tier name to TierConfig.
    Router is separate (not a tier — it classifies, doesn't generate).
    """

    tiers: dict[str, TierConfig] = Field(default_factory=dict)
    router: ModelConfig | None = None
    default: str = "normal"

    @model_validator(mode="after")
    def validate_default_tier(self) -> "ModelsConfig":
        """Ensure default tier exists in tiers dict."""
        if self.tiers and self.default not in self.tiers:
            raise ValueError(f"Default tier '{self.default}' not in tiers: {list(self.tiers)}")
        return self


class RoutingConfig(BaseModel):
    """Configuration for model routing.

    Task classification is handled by the ROUTER model. The classification
    prompt and grammar can be auto-generated from tier definitions or
    explicitly configured.
    """

    enabled: bool = True
    fallback_tier: str = "normal"
    classification_prompt: str | None = None  # None = auto-generate from tier focus
    tier_map: dict[str, str] = Field(default_factory=dict)  # Empty = auto-derive
    handoff_rules: dict[str, list[str]] = Field(default_factory=dict)  # Empty = all-to-all
    use_grammar: bool = False  # Opt-in GBNF constraint


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


class ExternalMCPConfig(BaseModel):
    """Configuration for external MCP server (Claude Code integration)."""

    enabled: bool = False
    socket_path: Path = Field(default_factory=lambda: Path.home() / ".entropi" / "mcp.sock")
    # Rate limiting: requests per minute
    rate_limit: int = Field(default=10, ge=1, le=100)

    @field_validator("socket_path")
    @classmethod
    def validate_socket_path(cls, v: Path) -> Path:
        """Expand user path."""
        return Path(v).expanduser()


class FilesystemConfig(BaseModel):
    """Filesystem server configuration."""

    # Proactive diagnostics on edit/write
    diagnostics_on_edit: bool = True
    fail_on_errors: bool = True  # Rollback edit if it introduces errors
    diagnostics_timeout: float = Field(default=1.0, ge=0.1, le=5.0)
    # Max file size for read_file (bytes). None = derive from model context.
    # Explicit value overrides dynamic calculation.
    max_read_bytes: int | None = Field(default=None, ge=1_000, le=500_000)
    # Percentage of model context window for single file read (dynamic gate)
    max_read_context_pct: float = Field(default=0.25, ge=0.05, le=0.75)


class MCPConfig(BaseModel):
    """MCP server configuration."""

    # Built-in servers to enable
    enable_filesystem: bool = True
    enable_bash: bool = True
    enable_git: bool = True
    enable_diagnostics: bool = True  # LSP diagnostics tool

    # Filesystem server config
    filesystem: FilesystemConfig = Field(default_factory=FilesystemConfig)

    # External MCP servers (from .mcp.json)
    external_servers: dict[str, dict[str, Any]] = Field(default_factory=dict)

    # External MCP server for Claude Code integration
    external: ExternalMCPConfig = Field(default_factory=ExternalMCPConfig)

    # Server timeout
    server_timeout_seconds: int = Field(default=30, ge=5, le=300)


class CompactionConfig(BaseModel):
    """Auto-compaction configuration."""

    enabled: bool = True
    threshold_percent: float = Field(default=0.75, ge=0.5, le=0.99)
    preserve_recent_turns: int = Field(default=2, ge=1, le=10)
    summary_max_tokens: int = Field(default=1500, ge=500, le=4000)
    notify_user: bool = True
    save_full_history: bool = True
    tool_result_ttl: int = Field(default=10, ge=1, le=20)
    warning_threshold_percent: float = Field(default=0.6, ge=0.3, le=0.9)

    @model_validator(mode="after")
    def validate_threshold_ordering(self) -> "CompactionConfig":
        """Warning threshold must be below compaction threshold."""
        if self.warning_threshold_percent >= self.threshold_percent:
            raise ValueError(
                f"compaction.warning_threshold_percent "
                f"({self.warning_threshold_percent}) must be less than "
                f"threshold_percent ({self.threshold_percent})"
            )
        return self


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


# === Voice Interface Configuration ===


class PersonaPlexModelConfig(BaseModel):
    """Configuration for PersonaPlex model source."""

    hf_repo: str = "nvidia/personaplex-7b-v1"


class PersonaPlexRuntimeConfig(BaseModel):
    """Runtime configuration for PersonaPlex."""

    device: Literal["cuda", "cpu"] = "cuda"
    quantization: Literal["int8", "fp16", "none"] = "int8"
    context_window: int = Field(default=500, ge=50, le=3000)  # LM context in tokens


class PersonaPlexSamplingConfig(BaseModel):
    """Sampling parameters for PersonaPlex generation."""

    text_temperature: float = Field(default=0.8, ge=0.0, le=2.0)
    audio_temperature: float = Field(default=0.8, ge=0.0, le=2.0)
    top_k: int = Field(default=250, ge=1, le=1000)


class VoicePromptConfig(BaseModel):
    """Voice prompt file configuration."""

    prompt_dir: Path = Field(default_factory=lambda: Path.home() / ".entropi" / "voices")
    # Voice name from PersonaPlex (NATF0-3, NATM0-3, VARF0-4, VARM0-4)
    voice_name: str = "NATF2"
    thinking_audio: str = "thinking_moment.wav"

    @field_validator("prompt_dir")
    @classmethod
    def validate_prompt_dir(cls, v: Path) -> Path:
        """Expand user path."""
        return Path(v).expanduser()


class VoiceConversationConfig(BaseModel):
    """Voice conversation window configuration."""

    window_duration: float = Field(default=15.0, ge=5.0, le=60.0)
    initial_prompt: str = "You are a helpful coding assistant."


class VoiceServerConfig(BaseModel):
    """Configuration for voice server subprocess."""

    host: str = "127.0.0.1"
    port: int = Field(default=8765, ge=1024, le=65535)
    auto_start: bool = True
    startup_timeout_seconds: int = Field(
        default=600, ge=10, le=1800
    )  # 10 min default for model loading


class SecondaryModelConfig(BaseModel):
    """Configuration for secondary LLM used in context compaction."""

    model_path: Path = Field(
        default_factory=lambda: Path.home() / "models" / "gguf" / "Qwen3-0.6B-Q8_0.gguf"
    )
    max_tokens: int = Field(default=300, ge=50, le=1000)
    temperature: float = Field(default=0.3, ge=0.0, le=2.0)

    @field_validator("model_path")
    @classmethod
    def validate_model_path(cls, v: Path) -> Path:
        """Expand user path."""
        return Path(v).expanduser()


class VoiceConfig(BaseModel):
    """Root voice interface configuration."""

    enabled: bool = False

    # Sub-configurations
    model: PersonaPlexModelConfig = Field(default_factory=PersonaPlexModelConfig)
    runtime: PersonaPlexRuntimeConfig = Field(default_factory=PersonaPlexRuntimeConfig)
    sampling: PersonaPlexSamplingConfig = Field(default_factory=PersonaPlexSamplingConfig)
    voice_prompt: VoicePromptConfig = Field(default_factory=VoicePromptConfig)
    conversation: VoiceConversationConfig = Field(default_factory=VoiceConversationConfig)
    secondary_model: SecondaryModelConfig = Field(default_factory=SecondaryModelConfig)
    server: VoiceServerConfig = Field(default_factory=VoiceServerConfig)


def _validate_fallback_tier(fallback: str, tier_names: set[str]) -> None:
    """Raise if fallback_tier is not a defined tier."""
    if fallback not in tier_names:
        raise ValueError(
            f"routing.fallback_tier '{fallback}' " f"not in models.tiers: {sorted(tier_names)}"
        )


def _validate_tier_map(tier_map: dict[str, str], tier_names: set[str]) -> None:
    """Raise if any tier_map value references an undefined tier."""
    for key, tier in tier_map.items():
        if tier not in tier_names:
            raise ValueError(
                f"routing.tier_map['{key}'] = '{tier}' "
                f"not in models.tiers: {sorted(tier_names)}"
            )


def _validate_handoff_rules(rules: dict[str, list[str]], tier_names: set[str]) -> None:
    """Raise if any handoff_rules key or value references an undefined tier."""
    for source, targets in rules.items():
        if source not in tier_names:
            raise ValueError(
                f"routing.handoff_rules key '{source}' "
                f"not in models.tiers: {sorted(tier_names)}"
            )
        for target in targets:
            if target not in tier_names:
                raise ValueError(
                    f"routing.handoff_rules['{source}'] contains '{target}' "
                    f"not in models.tiers: {sorted(tier_names)}"
                )


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
    voice: VoiceConfig = Field(default_factory=VoiceConfig)

    # Logging
    log_level: Literal["DEBUG", "INFO", "WARNING", "ERROR"] = "INFO"
    log_file: Path | None = None

    # Prompt handling
    use_bundled_prompts: bool = True  # False = raise on missing prompt, no bundled fallback

    # Paths - use default_factory to ensure Path.home() is used
    config_dir: Path = Field(default_factory=lambda: Path.home() / ".entropi")
    prompts_dir: Path = Field(default_factory=lambda: Path.home() / ".entropi" / "prompts")
    commands_dir: Path = Field(default_factory=lambda: Path.home() / ".entropi" / "commands")

    @field_validator("config_dir", "prompts_dir", "commands_dir")
    @classmethod
    def validate_paths(cls, v: Path) -> Path:
        """Expand user paths if ~ is used."""
        return Path(v).expanduser()

    @model_validator(mode="after")
    def validate_routing_references(self) -> "EntropyConfig":
        """Cross-validate routing tier references against defined tiers."""
        tier_names = set(self.models.tiers)
        if not tier_names:
            return self

        if self.routing.enabled and self.models.router is None:
            raise ValueError("routing.enabled is true but models.router is not configured")

        _validate_fallback_tier(self.routing.fallback_tier, tier_names)
        _validate_tier_map(self.routing.tier_map, tier_names)
        _validate_handoff_rules(self.routing.handoff_rules, tier_names)
        return self
