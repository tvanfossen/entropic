# Implementation 01: Foundation

> Configuration system, base classes, and project skeleton

**Prerequisites:** Pre-work checklist complete
**Estimated Time:** 3-5 hours with Claude Code
**Checkpoint:** Basic `entropi --version` command works

---

## Objectives

1. Create Pydantic configuration schema
2. Implement configuration loader with hierarchy
3. Create base abstract classes for extensibility
4. Set up CLI entry point
5. Implement logging infrastructure
6. Create application orchestrator skeleton

---

## 1. Configuration Schema

### File: `src/entropi/config/schema.py`

```python
"""
Configuration schema using Pydantic.

Principles:
- All config values have sensible defaults
- Validation happens at load time
- Immutable after loading (frozen=True where appropriate)
"""
from pathlib import Path
from typing import Literal

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
    prompt: list[str] = Field(
        default_factory=lambda: ["bash.execute:*", "filesystem.write_file:*"]
    )


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
    external_servers: dict[str, dict] = Field(default_factory=dict)

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
```

---

## 2. Configuration Loader

### File: `src/entropi/config/loader.py`

```python
"""
Configuration loader with hierarchy support.

Hierarchy (lowest to highest priority):
1. Defaults (built into schema)
2. Global config (~/.entropi/config.yaml)
3. Project config (.entropi/config.yaml)
4. Local config (.entropi/config.local.yaml) - gitignored
5. Environment variables (ENTROPI_*)
6. CLI arguments
"""
import os
from pathlib import Path
from typing import Any

import yaml

from entropi.config.schema import EntropyConfig


def deep_merge(base: dict, override: dict) -> dict:
    """
    Deep merge two dictionaries.

    Args:
        base: Base dictionary
        override: Override dictionary (takes precedence)

    Returns:
        Merged dictionary
    """
    result = base.copy()

    for key, value in override.items():
        if key in result and isinstance(result[key], dict) and isinstance(value, dict):
            result[key] = deep_merge(result[key], value)
        else:
            result[key] = value

    return result


def load_yaml_config(path: Path) -> dict[str, Any]:
    """
    Load configuration from YAML file.

    Args:
        path: Path to YAML file

    Returns:
        Configuration dictionary (empty if file doesn't exist)
    """
    if not path.exists():
        return {}

    with open(path) as f:
        content = yaml.safe_load(f)
        return content if content else {}


def find_project_root() -> Path | None:
    """
    Find project root by looking for .entropi directory or .git.

    Returns:
        Project root path or None if not in a project
    """
    current = Path.cwd()

    while current != current.parent:
        if (current / ".entropi").is_dir() or (current / ".git").is_dir():
            return current
        current = current.parent

    return None


class ConfigLoader:
    """Configuration loader with hierarchy support."""

    def __init__(
        self,
        global_config_dir: Path | None = None,
        project_root: Path | None = None,
    ) -> None:
        """
        Initialize configuration loader.

        Args:
            global_config_dir: Global config directory (default: ~/.entropi)
            project_root: Project root (auto-detected if None)
        """
        self.global_config_dir = (
            global_config_dir or Path("~/.entropi")
        ).expanduser()
        self.project_root = project_root or find_project_root()

    def load(self, cli_overrides: dict[str, Any] | None = None) -> EntropyConfig:
        """
        Load configuration with full hierarchy.

        Args:
            cli_overrides: CLI argument overrides

        Returns:
            Merged configuration
        """
        # Start with empty dict (defaults come from Pydantic)
        config: dict[str, Any] = {}

        # Layer 1: Global config
        global_config_path = self.global_config_dir / "config.yaml"
        config = deep_merge(config, load_yaml_config(global_config_path))

        # Layer 2: Project config
        if self.project_root:
            project_config_path = self.project_root / ".entropi" / "config.yaml"
            config = deep_merge(config, load_yaml_config(project_config_path))

            # Layer 3: Local config (gitignored)
            local_config_path = self.project_root / ".entropi" / "config.local.yaml"
            config = deep_merge(config, load_yaml_config(local_config_path))

        # Layer 4: CLI overrides
        if cli_overrides:
            config = deep_merge(config, cli_overrides)

        # Create config (Pydantic handles env vars via SettingsConfigDict)
        return EntropyConfig(**config)

    def ensure_directories(self, config: EntropyConfig) -> None:
        """
        Ensure all required directories exist.

        Args:
            config: Configuration to use
        """
        directories = [
            config.config_dir,
            config.prompts_dir,
            config.commands_dir,
            config.storage.database_path.parent,
        ]

        for directory in directories:
            directory.mkdir(parents=True, exist_ok=True)


# Global config instance (lazy loaded)
_config: EntropyConfig | None = None


def get_config() -> EntropyConfig:
    """Get global configuration instance."""
    global _config
    if _config is None:
        loader = ConfigLoader()
        _config = loader.load()
        loader.ensure_directories(_config)
    return _config


def reload_config(cli_overrides: dict[str, Any] | None = None) -> EntropyConfig:
    """Reload configuration."""
    global _config
    loader = ConfigLoader()
    _config = loader.load(cli_overrides)
    loader.ensure_directories(_config)
    return _config
```

---

## 3. Base Abstract Classes

### File: `src/entropi/core/base.py`

```python
"""
Base abstract classes for extensibility.

All major components inherit from these bases to ensure
consistent interfaces and enable dependency injection.
"""
from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from typing import Any, AsyncIterator


@dataclass
class Message:
    """A message in a conversation."""

    role: str  # "user", "assistant", "system", "tool"
    content: str
    tool_calls: list[dict[str, Any]] = field(default_factory=list)
    tool_results: list[dict[str, Any]] = field(default_factory=list)
    metadata: dict[str, Any] = field(default_factory=dict)


@dataclass
class ToolCall:
    """A tool call request."""

    id: str
    name: str
    arguments: dict[str, Any]


@dataclass
class ToolResult:
    """Result of a tool execution."""

    call_id: str
    name: str
    result: str
    is_error: bool = False
    duration_ms: int = 0


@dataclass
class GenerationResult:
    """Result of a generation."""

    content: str
    tool_calls: list[ToolCall] = field(default_factory=list)
    finish_reason: str = "stop"
    token_count: int = 0
    generation_time_ms: int = 0


class ModelBackend(ABC):
    """Abstract base class for model backends."""

    @abstractmethod
    async def load(self) -> None:
        """Load the model into memory."""
        pass

    @abstractmethod
    async def unload(self) -> None:
        """Unload the model from memory."""
        pass

    @abstractmethod
    async def generate(
        self,
        messages: list[Message],
        max_tokens: int = 4096,
        stop: list[str] | None = None,
        **kwargs: Any,
    ) -> GenerationResult:
        """Generate a response."""
        pass

    @abstractmethod
    async def generate_stream(
        self,
        messages: list[Message],
        max_tokens: int = 4096,
        stop: list[str] | None = None,
        **kwargs: Any,
    ) -> AsyncIterator[str]:
        """Generate a streaming response."""
        pass

    @abstractmethod
    def count_tokens(self, text: str) -> int:
        """Count tokens in text."""
        pass

    @property
    @abstractmethod
    def context_length(self) -> int:
        """Get the model's context length."""
        pass

    @property
    @abstractmethod
    def is_loaded(self) -> bool:
        """Check if model is loaded."""
        pass


class ToolProvider(ABC):
    """Abstract base class for tool providers (MCP servers)."""

    @property
    @abstractmethod
    def name(self) -> str:
        """Get provider name."""
        pass

    @abstractmethod
    async def initialize(self) -> None:
        """Initialize the provider."""
        pass

    @abstractmethod
    async def shutdown(self) -> None:
        """Shutdown the provider."""
        pass

    @abstractmethod
    async def list_tools(self) -> list[dict[str, Any]]:
        """List available tools."""
        pass

    @abstractmethod
    async def execute_tool(
        self,
        name: str,
        arguments: dict[str, Any],
    ) -> ToolResult:
        """Execute a tool."""
        pass


class StorageBackend(ABC):
    """Abstract base class for storage backends."""

    @abstractmethod
    async def initialize(self) -> None:
        """Initialize storage."""
        pass

    @abstractmethod
    async def close(self) -> None:
        """Close storage."""
        pass

    @abstractmethod
    async def save_conversation(
        self,
        conversation_id: str,
        messages: list[Message],
        metadata: dict[str, Any] | None = None,
    ) -> None:
        """Save a conversation."""
        pass

    @abstractmethod
    async def load_conversation(
        self,
        conversation_id: str,
    ) -> tuple[list[Message], dict[str, Any]]:
        """Load a conversation."""
        pass

    @abstractmethod
    async def list_conversations(
        self,
        limit: int = 100,
        offset: int = 0,
    ) -> list[dict[str, Any]]:
        """List conversations."""
        pass

    @abstractmethod
    async def search_conversations(
        self,
        query: str,
        limit: int = 10,
    ) -> list[dict[str, Any]]:
        """Search conversations."""
        pass
```

---

## 4. Logging Infrastructure

### File: `src/entropi/core/logging.py`

```python
"""
Logging infrastructure.

Provides consistent logging across all components with
optional file output and rich console formatting.
"""
import logging
import sys
from pathlib import Path

from rich.console import Console
from rich.logging import RichHandler

from entropi.config.schema import EntropyConfig


def setup_logging(config: EntropyConfig) -> logging.Logger:
    """
    Set up logging infrastructure.

    Args:
        config: Application configuration

    Returns:
        Root logger
    """
    # Create logger
    logger = logging.getLogger("entropi")
    logger.setLevel(config.log_level)

    # Clear existing handlers
    logger.handlers.clear()

    # Console handler with Rich formatting
    console_handler = RichHandler(
        console=Console(stderr=True),
        show_time=True,
        show_path=False,
        rich_tracebacks=True,
    )
    console_handler.setLevel(config.log_level)
    console_format = logging.Formatter("%(message)s")
    console_handler.setFormatter(console_format)
    logger.addHandler(console_handler)

    # File handler (if configured)
    if config.log_file:
        log_path = Path(config.log_file).expanduser()
        log_path.parent.mkdir(parents=True, exist_ok=True)

        file_handler = logging.FileHandler(log_path)
        file_handler.setLevel(config.log_level)
        file_format = logging.Formatter(
            "%(asctime)s - %(name)s - %(levelname)s - %(message)s"
        )
        file_handler.setFormatter(file_format)
        logger.addHandler(file_handler)

    return logger


def get_logger(name: str) -> logging.Logger:
    """
    Get a logger for a specific component.

    Args:
        name: Component name (will be prefixed with 'entropi.')

    Returns:
        Logger instance
    """
    return logging.getLogger(f"entropi.{name}")
```

---

## 5. CLI Entry Point

### File: `src/entropi/cli.py`

```python
"""
CLI entry point for Entropi.

Handles command-line arguments and initializes the application.
"""
import asyncio
import sys
from pathlib import Path

import click

from entropi import __version__
from entropi.config.loader import reload_config
from entropi.core.logging import setup_logging


@click.group(invoke_without_command=True)
@click.version_option(version=__version__, prog_name="entropi")
@click.option(
    "--config",
    "-c",
    type=click.Path(exists=True, path_type=Path),
    help="Path to configuration file",
)
@click.option(
    "--model",
    "-m",
    type=click.Choice(["primary", "workhorse", "fast", "micro"]),
    help="Model to use",
)
@click.option(
    "--log-level",
    "-l",
    type=click.Choice(["DEBUG", "INFO", "WARNING", "ERROR"]),
    help="Logging level",
)
@click.option(
    "--project",
    "-p",
    type=click.Path(exists=True, file_okay=False, path_type=Path),
    help="Project directory",
)
@click.pass_context
def main(
    ctx: click.Context,
    config: Path | None,
    model: str | None,
    log_level: str | None,
    project: Path | None,
) -> None:
    """
    Entropi - Local AI Coding Assistant

    Run without arguments to start interactive mode.
    """
    # Build CLI overrides
    cli_overrides: dict = {}

    if model:
        cli_overrides["routing"] = {"default": model}

    if log_level:
        cli_overrides["log_level"] = log_level

    # Load configuration
    app_config = reload_config(cli_overrides)

    # Setup logging
    logger = setup_logging(app_config)

    # Store in context for subcommands
    ctx.ensure_object(dict)
    ctx.obj["config"] = app_config
    ctx.obj["logger"] = logger
    ctx.obj["project"] = project or Path.cwd()

    # If no subcommand, start interactive mode
    if ctx.invoked_subcommand is None:
        from entropi.app import Application

        app = Application(config=app_config, project_dir=ctx.obj["project"])
        asyncio.run(app.run())


@main.command()
@click.pass_context
def status(ctx: click.Context) -> None:
    """Show system and model status."""
    from rich.console import Console
    from rich.table import Table

    console = Console()
    config = ctx.obj["config"]

    table = Table(title="Entropi Status")
    table.add_column("Component", style="cyan")
    table.add_column("Status", style="green")

    # Models
    if config.models.primary:
        table.add_row("Primary Model", str(config.models.primary.path))
    if config.models.fast:
        table.add_row("Fast Model", str(config.models.fast.path))
    if config.models.micro:
        table.add_row("Micro Model", str(config.models.micro.path))

    # Settings
    table.add_row("Routing Enabled", str(config.routing.enabled))
    table.add_row("Quality Enforcement", str(config.quality.enabled))
    table.add_row("Log Level", config.log_level)

    console.print(table)


@main.command()
@click.argument("message", required=False)
@click.option("--no-stream", is_flag=True, help="Disable streaming output")
@click.pass_context
def ask(ctx: click.Context, message: str | None, no_stream: bool) -> None:
    """
    Send a single message and get a response.

    If MESSAGE is not provided, reads from stdin.
    """
    if message is None:
        if sys.stdin.isatty():
            click.echo("Error: No message provided", err=True)
            sys.exit(1)
        message = sys.stdin.read().strip()

    from entropi.app import Application

    config = ctx.obj["config"]
    app = Application(config=config, project_dir=ctx.obj["project"])

    asyncio.run(app.single_turn(message, stream=not no_stream))


@main.command()
@click.pass_context
def init(ctx: click.Context) -> None:
    """Initialize Entropi in the current directory."""
    project_dir = ctx.obj["project"]
    entropi_dir = project_dir / ".entropi"

    if entropi_dir.exists():
        click.echo(f"Entropi already initialized in {project_dir}")
        return

    # Create directories
    entropi_dir.mkdir(parents=True)
    (entropi_dir / "commands").mkdir()

    # Create default config
    default_config = """# Entropi Project Configuration
# See ~/.entropi/config.yaml for global settings

quality:
  enabled: true
  rules:
    max_cognitive_complexity: 15
    require_type_hints: true

permissions:
  allow:
    - "filesystem.*"
    - "git.*"
"""
    (entropi_dir / "config.yaml").write_text(default_config)

    # Create ENTROPI.md template
    entropi_md = """# Project Context

## About
Describe your project here.

## Structure
- `src/` - Source code
- `tests/` - Test files

## Commands
```bash
# Add common commands here
```

## Standards
- Add coding standards here
"""
    if not (project_dir / "ENTROPI.md").exists():
        (project_dir / "ENTROPI.md").write_text(entropi_md)

    click.echo(f"Initialized Entropi in {project_dir}")
    click.echo("Created:")
    click.echo(f"  - {entropi_dir}/config.yaml")
    click.echo(f"  - {entropi_dir}/commands/")
    click.echo(f"  - {project_dir}/ENTROPI.md")


if __name__ == "__main__":
    main()
```

---

## 6. Package Initialization

### File: `src/entropi/__init__.py`

```python
"""
Entropi - Local AI Coding Assistant

A terminal-based coding assistant powered by Qwen models.
"""

__version__ = "0.1.0"
__author__ = "Your Name"
```

### File: `src/entropi/__main__.py`

```python
"""
Allow running as `python -m entropi`.
"""
from entropi.cli import main

if __name__ == "__main__":
    main()
```

---

## 7. Application Orchestrator Skeleton

### File: `src/entropi/app.py`

```python
"""
Application orchestrator.

Coordinates all components and manages the application lifecycle.
This is a skeleton - full implementation comes in later phases.
"""
from pathlib import Path

from rich.console import Console

from entropi.config.schema import EntropyConfig
from entropi.core.logging import get_logger


class Application:
    """Main application orchestrator."""

    def __init__(
        self,
        config: EntropyConfig,
        project_dir: Path | None = None,
    ) -> None:
        """
        Initialize application.

        Args:
            config: Application configuration
            project_dir: Project directory
        """
        self.config = config
        self.project_dir = project_dir or Path.cwd()
        self.logger = get_logger("app")
        self.console = Console()

        # Components (initialized lazily)
        self._model_orchestrator = None
        self._mcp_client = None
        self._storage = None
        self._ui = None

    async def initialize(self) -> None:
        """Initialize all components."""
        self.logger.info("Initializing Entropi...")

        # TODO: Initialize components in later phases
        # - Model orchestrator
        # - MCP client
        # - Storage backend
        # - Terminal UI

        self.logger.info("Entropi initialized")

    async def shutdown(self) -> None:
        """Shutdown all components."""
        self.logger.info("Shutting down...")

        # TODO: Shutdown components in reverse order

        self.logger.info("Shutdown complete")

    async def run(self) -> None:
        """Run the interactive application."""
        try:
            await self.initialize()

            # Placeholder - will be replaced with actual UI
            self.console.print("[bold green]Entropi[/bold green] initialized!")
            self.console.print(f"Project: {self.project_dir}")
            self.console.print(f"Config: {self.config.config_dir}")
            self.console.print("\n[yellow]Interactive mode not yet implemented.[/yellow]")
            self.console.print("Use 'entropi ask \"your question\"' for now.")

        except KeyboardInterrupt:
            self.console.print("\n[yellow]Interrupted[/yellow]")
        finally:
            await self.shutdown()

    async def single_turn(self, message: str, stream: bool = True) -> None:
        """
        Process a single message and exit.

        Args:
            message: User message
            stream: Whether to stream output
        """
        try:
            await self.initialize()

            # Placeholder - will be replaced with actual generation
            self.console.print(f"[dim]You: {message}[/dim]")
            self.console.print("\n[yellow]Generation not yet implemented.[/yellow]")

        finally:
            await self.shutdown()
```

---

## 8. Tests

### File: `tests/unit/test_config.py`

```python
"""Tests for configuration system."""
import tempfile
from pathlib import Path

import pytest
import yaml

from entropi.config.loader import ConfigLoader, deep_merge, load_yaml_config
from entropi.config.schema import EntropyConfig, ModelConfig


class TestDeepMerge:
    """Tests for deep_merge function."""

    def test_simple_merge(self) -> None:
        """Test merging flat dictionaries."""
        base = {"a": 1, "b": 2}
        override = {"b": 3, "c": 4}
        result = deep_merge(base, override)
        assert result == {"a": 1, "b": 3, "c": 4}

    def test_nested_merge(self) -> None:
        """Test merging nested dictionaries."""
        base = {"outer": {"a": 1, "b": 2}}
        override = {"outer": {"b": 3, "c": 4}}
        result = deep_merge(base, override)
        assert result == {"outer": {"a": 1, "b": 3, "c": 4}}

    def test_base_unchanged(self) -> None:
        """Test that base dictionary is not modified."""
        base = {"a": 1}
        override = {"b": 2}
        deep_merge(base, override)
        assert base == {"a": 1}


class TestLoadYamlConfig:
    """Tests for load_yaml_config function."""

    def test_load_existing_file(self) -> None:
        """Test loading existing YAML file."""
        with tempfile.NamedTemporaryFile(mode="w", suffix=".yaml", delete=False) as f:
            yaml.dump({"key": "value"}, f)
            f.flush()
            result = load_yaml_config(Path(f.name))
            assert result == {"key": "value"}

    def test_load_nonexistent_file(self) -> None:
        """Test loading non-existent file returns empty dict."""
        result = load_yaml_config(Path("/nonexistent/path.yaml"))
        assert result == {}


class TestEntropyConfig:
    """Tests for EntropyConfig schema."""

    def test_default_values(self) -> None:
        """Test default configuration values."""
        config = EntropyConfig()
        assert config.log_level == "INFO"
        assert config.routing.enabled is True
        assert config.quality.enabled is True

    def test_model_config_path_expansion(self) -> None:
        """Test that model paths are expanded."""
        config = ModelConfig(path=Path("~/models/test.gguf"))
        assert not str(config.path).startswith("~")

    def test_validation(self) -> None:
        """Test configuration validation."""
        # Invalid context length should raise
        with pytest.raises(ValueError):
            ModelConfig(path=Path("/test"), context_length=100)  # Below minimum


class TestConfigLoader:
    """Tests for ConfigLoader."""

    def test_load_defaults(self) -> None:
        """Test loading with no config files."""
        loader = ConfigLoader(
            global_config_dir=Path("/nonexistent"),
            project_root=None,
        )
        config = loader.load()
        assert isinstance(config, EntropyConfig)

    def test_hierarchy(self) -> None:
        """Test configuration hierarchy."""
        with tempfile.TemporaryDirectory() as tmpdir:
            tmpdir = Path(tmpdir)

            # Create global config
            global_dir = tmpdir / "global"
            global_dir.mkdir()
            (global_dir / "config.yaml").write_text(
                yaml.dump({"log_level": "DEBUG", "routing": {"enabled": True}})
            )

            # Create project config
            project_dir = tmpdir / "project"
            entropi_dir = project_dir / ".entropi"
            entropi_dir.mkdir(parents=True)
            (entropi_dir / "config.yaml").write_text(
                yaml.dump({"routing": {"enabled": False}})
            )

            loader = ConfigLoader(
                global_config_dir=global_dir,
                project_root=project_dir,
            )
            config = loader.load()

            # Global log_level preserved
            assert config.log_level == "DEBUG"
            # Project routing overrides global
            assert config.routing.enabled is False
```

---

## Checkpoint: Verification

After implementing this phase, verify:

```bash
# Activate environment
source ~/.venvs/entropi/bin/activate
cd ~/projects/entropi

# Install in development mode
pip install -e ".[dev]"

# Run tests
pytest tests/unit/test_config.py -v

# Check version
entropi --version
# Should output: entropi, version 0.1.0

# Check status
entropi status

# Initialize in a test directory
cd /tmp
mkdir test_project && cd test_project
entropi init
ls -la .entropi/
cat ENTROPI.md

# Run pre-commit
cd ~/projects/entropi
pre-commit run --all-files
```

**Success Criteria:**
- [ ] `entropi --version` outputs version
- [ ] `entropi status` shows configuration
- [ ] `entropi init` creates project structure
- [ ] All tests pass
- [ ] Pre-commit passes

---

## Next Phase

Proceed to **Implementation 02: Inference Engine** to implement model loading and generation.
