"""
Configuration schema using Pydantic.

Principles:
- All config values have sensible defaults
- Validation happens at load time
- Immutable after loading (frozen=True where appropriate)
"""

from pathlib import Path
from typing import Annotated, Any, Literal

from pydantic import BaseModel, BeforeValidator, Field, field_validator, model_validator
from pydantic_settings import BaseSettings, SettingsConfigDict


def _expand_path(v: Any) -> Path:
    """Expand ~ in path values."""
    return Path(v).expanduser()


def _expand_optional_path(v: Any) -> Path | None:
    """Expand ~ in optional path values (None passes through)."""
    if v is None:
        return None
    return Path(v).expanduser()


def _expand_tri_state_path(v: Any) -> Path | Literal[False] | None:
    """Expand ~ in tri-state path values (None and False pass through)."""
    if v is None or v is False:
        return v
    return Path(v).expanduser()


ExpandedPath = Annotated[Path, BeforeValidator(_expand_path)]
OptionalExpandedPath = Annotated[Path | None, BeforeValidator(_expand_optional_path)]
TriStatePath = Annotated[Path | Literal[False] | None, BeforeValidator(_expand_tri_state_path)]

_BUNDLED_MODELS_FILE = Path(__file__).parent.parent / "data" / "bundled_models.yaml"
_bundled_models: dict[str, Any] | None = None


def _load_bundled_models() -> dict[str, Any]:
    """Load bundled model registry (cached)."""
    global _bundled_models  # noqa: PLW0603
    if _bundled_models is None:
        import yaml

        _bundled_models = yaml.safe_load(_BUNDLED_MODELS_FILE.read_text())
    return _bundled_models


def resolve_model_path(model: str) -> Path:
    """Resolve a model field to a file path.

    If ``model`` matches a key in bundled_models.yaml, resolves to
    ``~/models/gguf/{name}.gguf``. Otherwise treats it as a direct path.
    """
    bundled = _load_bundled_models()
    if model in bundled:
        name = bundled[model]["name"]
        return Path(f"~/models/gguf/{name}.gguf").expanduser()
    return Path(model).expanduser()


class ModelConfig(BaseModel):
    """Configuration for a single model.

    Load-time hardware params only. Inference behavior (temperature,
    max_output_tokens, etc.) belongs in identity frontmatter.

    The ``path`` field accepts either a direct path to a GGUF file OR
    a key from ``bundled_models.yaml`` (e.g. ``primary``). Keys are
    resolved to ``~/models/gguf/{name}.gguf`` at load time.

    Attributes:
        allowed_tools: Tool visibility filter using fully-qualified names
            in ``{server_name}.{tool_name}`` format. ``None`` (default)
            means all registered tools are visible to this model/tier.
    """

    path: ExpandedPath
    adapter: str = "qwen2"  # Adapter name: qwen2, qwen3, qwen35, falcon, generic
    context_length: int = Field(default=16384, ge=512, le=131072)
    gpu_layers: int = Field(default=-1)  # -1 = all layers
    keep_warm: bool = False  # Use WARM state: pre-warm at startup, deactivate (not unload) on swap
    use_mlock: bool = True  # Lock model pages in RAM (prevents OS swap; reduces activate latency)
    logits_all: bool = False  # Compute logits for all positions (required for logprobs)
    allowed_tools: list[str] | None = None

    @field_validator("path", mode="before")
    @classmethod
    def resolve_bundled_model(cls, v: Any) -> Any:
        """Resolve bundled model keys to paths before expansion.

        If the value matches a key in bundled_models.yaml, replaces it
        with ~/models/gguf/{name}.gguf. Otherwise passes through for
        normal path expansion.
        """
        if isinstance(v, str):
            bundled = _load_bundled_models()
            if v in bundled:
                return f"~/models/gguf/{bundled[v]['name']}.gguf"
        return v

    @field_validator("allowed_tools")
    @classmethod
    def validate_allowed_tools(cls, v: list[str] | None) -> list[str] | None:
        """Validate allowed_tools entries are fully-qualified '{server}.{tool}' names."""
        if v is None:
            return v
        for entry in v:
            if "." not in entry:
                raise ValueError(
                    f"allowed_tools entry '{entry}' must use '{{server_name}}.{{tool_name}}' format"
                )
        return v


class TierConfig(ModelConfig):
    """Model configuration for a specific tier.

    Extends ModelConfig with identity resolution. Inference behavior
    (temperature, max_output_tokens, etc.) lives in identity frontmatter.

    Identity prompt resolution:
        absent/None  → bundled default (ships with entropic-engine)
        False        → disabled entirely
        path string  → custom file (must exist, validated at load)
    """

    identity: TriStatePath = None
    grammar: OptionalExpandedPath = None
    auto_chain: bool | None = None  # None = defer to identity frontmatter
    routable: bool | None = None  # None = defer to identity frontmatter


class ModelsConfig(BaseModel):
    """Configuration for all models.

    Tiers are defined as a dict mapping tier name to TierConfig.
    Router is separate (not a tier — it classifies, doesn't generate).
    """

    tiers: dict[str, TierConfig] = Field(default_factory=dict)
    router: ModelConfig | None = None
    default: str = "lead"

    @model_validator(mode="after")
    def validate_default_tier(self) -> "ModelsConfig":
        """Ensure default tier exists in tiers dict."""
        if self.tiers and self.default not in self.tiers:
            raise ValueError(f"Default tier '{self.default}' not in tiers: {list(self.tiers)}")
        return self


class RoutingConfig(BaseModel):
    """Configuration for model routing.

    Task classification is handled by the ROUTER model. The classification
    prompt is auto-generated from tier definitions or explicitly configured.
    """

    enabled: bool = False
    fallback_tier: str = "lead"
    classification_prompt: str | None = None  # None = auto-generate from tier focus
    tier_map: dict[str, str] = Field(default_factory=dict)  # Empty = auto-derive
    handoff_rules: dict[str, list[str]] = Field(default_factory=dict)  # Empty = all-to-all


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


class ExternalMCPConfig(BaseModel):
    """Configuration for external MCP server (Claude Code integration)."""

    enabled: bool = False
    # When None, socket path is derived from project_dir at runtime:
    #   ~/.entropic/socks/{first8(sha256(abs(project_dir)))}.sock
    # Set explicitly to override with a fixed path.
    socket_path: OptionalExpandedPath = None
    # Rate limiting: requests per minute
    rate_limit: int = Field(default=10, ge=1, le=100)


class FilesystemConfig(BaseModel):
    """Filesystem server configuration."""

    # Proactive diagnostics on edit/write
    diagnostics_on_edit: bool = True
    fail_on_errors: bool = True  # Rollback edit if it introduces errors
    diagnostics_timeout: float = Field(default=1.0, ge=0.1, le=5.0)
    # Allow file operations outside workspace root (../../ references)
    # The permissions system still governs approval — this only lifts the
    # path containment check.
    allow_outside_root: bool = False
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
    enable_web: bool = True

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

    Used by the diagnostics MCP server for proactive code analysis.
    Currently supports Python (pyright) and C (clangd).
    """

    enabled: bool = True

    # Per-language enable/disable
    python_enabled: bool = True
    c_enabled: bool = True

    # Custom server overrides (optional)
    servers: dict[str, LSPServerConfig] = Field(default_factory=dict)


def _validate_fallback_tier(fallback: str, tier_names: set[str]) -> None:
    """Raise if fallback_tier is not a defined tier."""
    if fallback not in tier_names:
        raise ValueError(
            f"routing.fallback_tier '{fallback}' not in models.tiers: {sorted(tier_names)}"
        )


def _validate_tier_map(tier_map: dict[str, str], tier_names: set[str]) -> None:
    """Raise if any tier_map value references an undefined tier."""
    for key, tier in tier_map.items():
        if tier not in tier_names:
            raise ValueError(
                f"routing.tier_map['{key}'] = '{tier}' not in models.tiers: {sorted(tier_names)}"
            )


def _validate_handoff_rules(rules: dict[str, list[str]], tier_names: set[str]) -> None:
    """Raise if any handoff_rules key or value references an undefined tier."""
    for source, targets in rules.items():
        if source not in tier_names:
            raise ValueError(
                f"routing.handoff_rules key '{source}' not in models.tiers: {sorted(tier_names)}"
            )
        for target in targets:
            if target not in tier_names:
                raise ValueError(
                    f"routing.handoff_rules['{source}'] contains '{target}' "
                    f"not in models.tiers: {sorted(tier_names)}"
                )


def _warn_auto_chain_without_targets(
    tiers: dict[str, TierConfig], handoff_rules: dict[str, list[str]]
) -> None:
    """Warn if a tier has auto_chain=True but no handoff_rules entry."""
    import warnings

    for name, tier_cfg in tiers.items():
        if tier_cfg.auto_chain and name not in handoff_rules:
            warnings.warn(
                f"Tier '{name}' has auto_chain=true but no handoff_rules entry. "
                f"Auto-chain will have no targets at runtime.",
                stacklevel=3,
            )


class LibraryConfig(BaseSettings):
    """Configuration for the entropic inference engine.

    Contains all fields needed for inference: models, routing,
    generation, MCP, compaction, and prompts. TUI-specific fields
    live in the ``entropic-tui`` package.
    """

    model_config = SettingsConfigDict(
        env_prefix="ENTROPIC_",
        env_nested_delimiter="__",
        extra="ignore",
    )

    # Core inference configuration
    models: ModelsConfig = Field(default_factory=ModelsConfig)
    routing: RoutingConfig = Field(default_factory=RoutingConfig)
    generation: GenerationConfig = Field(default_factory=GenerationConfig)
    permissions: PermissionsConfig = Field(default_factory=PermissionsConfig)
    mcp: MCPConfig = Field(default_factory=MCPConfig)
    compaction: CompactionConfig = Field(default_factory=CompactionConfig)
    lsp: LSPConfig = Field(default_factory=LSPConfig)

    # Logging
    log_level: Literal["DEBUG", "INFO", "WARNING", "ERROR"] = "INFO"

    # Prompt handling — new per-type fields (Phase 2)
    # constitution: None = bundled, False = disabled, Path = custom
    constitution: TriStatePath = None
    # app_context: None = disabled, False = disabled, Path = custom
    app_context: TriStatePath = None

    # Auto-inject model config (tier, model file, adapter) into system prompt
    inject_model_context: bool = True

    # VRAM management
    vram_reserve_mb: int = Field(default=512, ge=0, le=65536)  # Reserved VRAM headroom (MB)

    # Paths
    config_dir: ExpandedPath = Field(default_factory=lambda: Path.home() / ".entropic")

    @model_validator(mode="after")
    def validate_routing_references(self) -> "LibraryConfig":
        """Cross-validate routing tier references against defined tiers."""
        tier_names = set(self.models.tiers)
        if not tier_names:
            return self

        if self.routing.enabled and self.models.router is None:
            raise ValueError("routing.enabled is true but models.router is not configured")

        _validate_fallback_tier(self.routing.fallback_tier, tier_names)
        _validate_tier_map(self.routing.tier_map, tier_names)
        _validate_handoff_rules(self.routing.handoff_rules, tier_names)
        _warn_auto_chain_without_targets(self.models.tiers, self.routing.handoff_rules)
        return self
