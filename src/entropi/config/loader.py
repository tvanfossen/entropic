"""
Configuration loader with hierarchy support.

Hierarchy (lowest to highest priority):
1. Defaults (built into schema)
2. Global config (~/.entropi/config.yaml) — user defaults for all projects
3. Project config (.entropi/config.local.yaml) — source of truth per project
4. Environment variables (ENTROPI_*)
5. CLI arguments

Seeding flow:
1. Package default_config.yaml → ~/.entropi/config.yaml (on first global run)
2. ~/.entropi/config.yaml → .entropi/config.local.yaml (on first project run)

Per-project architecture:
- Each project gets its own .entropi/ folder
- Auto-created on first run with config seeded from global
- Database is per-project for conversation history
- User decides whether to commit config.local.yaml (not gitignored)
"""

import importlib.resources
import logging
import os
import shutil
from pathlib import Path
from typing import Any

import yaml

from entropi.config.schema import EntropyConfig

logger = logging.getLogger(__name__)


def get_default_config_path() -> Path:
    """Get the path to the default config file bundled with the package."""
    # Use importlib.resources for Python 3.9+ compatible package data access
    try:
        resource = importlib.resources.files("entropi.data").joinpath("default_config.yaml")
        with importlib.resources.as_file(resource) as p:
            return Path(p)
    except (TypeError, FileNotFoundError):
        # Fallback for editable installs
        return Path(__file__).parent.parent / "data" / "default_config.yaml"


_SLOT_RENAMES: dict[str, str] = {
    "primary": "thinking",
    "workhorse": "normal",
    "fast": "code",
}


def _migrate_model_slots(models: dict[str, Any]) -> None:
    """Rename legacy model slot names (primary→thinking, etc.)."""
    for old_name, new_name in _SLOT_RENAMES.items():
        if old_name in models and new_name not in models:
            models[new_name] = models.pop(old_name)


def _migrate_routing(data: dict[str, Any]) -> None:
    """Migrate routing config: fallback_model→fallback_tier, default slot names."""
    routing = data.get("routing", {})

    # fallback_model → fallback_tier
    fallback = routing.pop("fallback_model", None)
    if fallback and "fallback_tier" not in routing:
        routing["fallback_tier"] = _SLOT_RENAMES.get(fallback, fallback)

    # Migrate default model name
    models = data.get("models", {})
    default = models.get("default")
    if default and default in _SLOT_RENAMES:
        models["default"] = _SLOT_RENAMES[default]


def _migrate_named_slots_to_tiers(models: dict[str, Any]) -> None:
    """Convert named-slot model config (thinking, normal, ...) to tiers dict."""
    if "tiers" in models:
        return
    tiers: dict[str, Any] = {}
    for name in ["thinking", "normal", "code", "simple"]:
        if name in models:
            tiers[name] = models.pop(name)
    if tiers:
        models["tiers"] = tiers


def _migrate_config(data: dict[str, Any]) -> dict[str, Any]:
    """Migrate old config format to current schema.

    Args:
        data: Configuration dictionary

    Returns:
        Migrated configuration dictionary
    """
    if "models" not in data:
        return data

    _migrate_model_slots(data["models"])
    _migrate_routing(data)
    _migrate_named_slots_to_tiers(data["models"])

    return data


def deep_merge(base: dict[str, Any], override: dict[str, Any]) -> dict[str, Any]:
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
        if isinstance(content, dict):
            return content
        return {}


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
    """Configuration loader with hierarchy support.

    Defaults are tuned for the entropi TUI. Consumer applications override
    ``app_dir_name`` and ``default_config_path`` to get their own config
    directory with their own defaults — no subclassing needed::

        loader = ConfigLoader(
            project_root=Path("."),
            app_dir_name=".myapp",
            default_config_path=Path("data/default_config.yaml"),
            global_config_dir=None,   # skip global ~/.entropi layer
        )
        config = loader.load()
    """

    _SENTINEL = object()

    def __init__(
        self,
        global_config_dir: Path | None | object = _SENTINEL,
        project_root: Path | None = None,
        *,
        app_dir_name: str = ".entropi",
        default_config_path: Path | None = None,
    ) -> None:
        """
        Initialize configuration loader.

        Args:
            global_config_dir: Global config directory. Defaults to
                ``~/.entropi`` for TUI use. Pass ``None`` to disable
                global config entirely (recommended for consumer apps).
            project_root: Project root (auto-detected if None, or use cwd).
            app_dir_name: Name of the app's config directory created inside
                project_root (default: ``.entropi``). Consumer apps should
                use their own name (e.g. ``.pychess``).
            default_config_path: Path to a default config YAML to seed from
                when no config exists. ``None`` uses the entropi package
                default.
        """
        if global_config_dir is self._SENTINEL:
            self.global_config_dir: Path | None = Path.home() / ".entropi"
        else:
            self.global_config_dir = global_config_dir  # type: ignore[assignment]
        # Use detected project root, or fall back to current directory
        self.project_root = project_root or find_project_root() or Path.cwd()
        self.app_dir_name = app_dir_name
        self.default_config_path = default_config_path

    @property
    def _app_dir(self) -> Path:
        """Project-level app config directory."""
        return self.project_root / self.app_dir_name

    def _ensure_global_config(self) -> None:
        """
        Ensure global config directory and config exist.

        Skipped when ``global_config_dir`` is ``None`` (consumer apps).
        Copies from package default if not present.
        """
        if self.global_config_dir is None:
            return

        global_config_path = self.global_config_dir / "config.yaml"

        # Create global directory if it doesn't exist
        if not self.global_config_dir.exists():
            self.global_config_dir.mkdir(parents=True, exist_ok=True)

        # Copy default config from package if global config doesn't exist
        if not global_config_path.exists():
            default_config = get_default_config_path()
            if default_config.exists():
                shutil.copy(default_config, global_config_path)

    def _ensure_project_config(self) -> None:
        """
        Ensure project app directory and config exist.

        Creates the app directory (e.g. ``.entropi/`` or ``.pychess/``),
        seeds ``config.local.yaml`` from defaults on first run.
        """
        app_dir = self._app_dir
        local_config_path = app_dir / "config.local.yaml"
        legacy_config_path = app_dir / "config.yaml"

        is_new_dir = not app_dir.exists()

        # Create app directory if it doesn't exist
        if is_new_dir:
            app_dir.mkdir(parents=True, exist_ok=True)

            # Create .gitignore for transient files only
            gitignore_path = app_dir / ".gitignore"
            gitignore_path.write_text("*.db\n*.log\n")

            # Create ENTROPI.md only for the entropi TUI
            if self.app_dir_name == ".entropi":
                self._create_project_context_md(app_dir)

        # Migration: if legacy config.yaml exists but
        # config.local.yaml doesn't, adopt the legacy file
        if legacy_config_path.exists() and not local_config_path.exists():
            shutil.copy(legacy_config_path, local_config_path)

        # Seed config.local.yaml if it doesn't exist
        if not local_config_path.exists():
            self._seed_project_config(local_config_path)

    @staticmethod
    def _create_project_context_md(app_dir: Path) -> None:
        """Create default ENTROPI.md for TUI projects."""
        entropi_md_path = app_dir / "ENTROPI.md"
        entropi_md_path.write_text(
            "# Project Context\n\n"
            "This file provides context to Entropi."
            " Edit it to describe your project.\n\n"
            "## Overview\n\n"
            "<!-- Brief description of what this project does -->\n\n"
            "## Tech Stack\n\n"
            "<!-- Languages, frameworks, key dependencies -->\n\n"
            "## Structure\n\n"
            "<!-- Key directories and their purpose -->\n\n"
            "## Conventions\n\n"
            "<!-- Coding standards, naming conventions,"
            " patterns to follow -->\n"
        )

    def _seed_project_config(self, target: Path) -> None:
        """Seed project config.local.yaml.

        Priority: custom default_config_path > global config > package default.
        """
        # 1. Consumer-provided default config
        if self.default_config_path:
            if self.default_config_path.exists():
                shutil.copy(self.default_config_path, target)
                return
            logger.warning(
                "default_config_path %s does not exist, falling back",
                self.default_config_path,
            )

        # 2. Global config (if enabled)
        if self.global_config_dir is not None:
            global_config = self.global_config_dir / "config.yaml"
            try:
                if global_config.exists():
                    shutil.copy(global_config, target)
                    return
            except PermissionError:
                pass

        # 3. Package default
        default_config = get_default_config_path()
        if default_config.exists():
            shutil.copy(default_config, target)

    def load(self, cli_overrides: dict[str, Any] | None = None) -> EntropyConfig:
        """
        Load configuration with full hierarchy.

        Args:
            cli_overrides: CLI argument overrides

        Returns:
            Merged configuration
        """
        # Ensure global config exists (skipped when global_config_dir=None)
        try:
            self._ensure_global_config()
        except PermissionError:
            # Can't write to home directory (e.g., container)
            pass

        # Ensure project app dir exists with seeded config
        self._ensure_project_config()

        # Start with empty dict (defaults come from Pydantic)
        config: dict[str, Any] = {}

        # Layer 1: Global config (skipped for consumer apps)
        if self.global_config_dir is not None:
            try:
                global_path = self.global_config_dir / "config.yaml"
                global_config = _migrate_config(
                    load_yaml_config(global_path),
                )
                config = deep_merge(config, global_config)
            except PermissionError:
                pass

        # Layer 2: Project config (source of truth)
        local_path = self._app_dir / "config.local.yaml"
        local_config = _migrate_config(
            load_yaml_config(local_path),
        )
        config = deep_merge(config, local_config)

        # Layer 3: CLI overrides
        if cli_overrides:
            config = deep_merge(config, cli_overrides)

        # Set config_dir to the project-level app directory
        config["config_dir"] = str(self._app_dir)

        # Layer 4: Environment variable overrides (for debugging)
        env_overrides = {
            "ENTROPI_LOG_LEVEL": "log_level",
        }
        for env_var, config_key in env_overrides.items():
            if env_var in os.environ:
                config[config_key] = os.environ[env_var]

        # Create config
        return EntropyConfig(**config)

    def ensure_directories(self, config: EntropyConfig) -> None:
        """
        Ensure all required directories exist.

        Args:
            config: Configuration to use
        """
        # Only create project-level directories
        directories = [
            config.config_dir,
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


def save_permission(pattern: str, allow: bool) -> None:
    """
    Save a permission pattern to the project config.

    Args:
        pattern: Permission pattern (e.g., "bash.execute:pytest *")
        allow: True to add to allow list, False to deny list
    """
    project_root = find_project_root() or Path.cwd()
    config_path = project_root / ".entropi" / "config.local.yaml"

    # Load existing config
    config = load_yaml_config(config_path)

    # Ensure permissions section exists
    if "permissions" not in config:
        config["permissions"] = {}

    permissions = config["permissions"]
    list_key = "allow" if allow else "deny"

    if list_key not in permissions:
        permissions[list_key] = []

    # Add pattern if not already present
    if pattern not in permissions[list_key]:
        permissions[list_key].append(pattern)

        # Write back to file
        with open(config_path, "w") as f:
            yaml.dump(config, f, default_flow_style=False, sort_keys=False)

    # Reload global config to pick up changes
    global _config
    if _config is not None:
        loader = ConfigLoader()
        _config = loader.load()
