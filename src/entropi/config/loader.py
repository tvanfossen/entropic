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
import os
import shutil
from pathlib import Path
from typing import Any

import yaml

from entropi.config.schema import EntropyConfig


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


def _migrate_config(data: dict[str, Any]) -> dict[str, Any]:
    """
    Migrate old config format to new task-specialized format.

    Handles migration of model slot names:
    - primary -> thinking
    - workhorse -> normal
    - fast -> code
    - micro -> micro (unchanged)

    Args:
        data: Configuration dictionary

    Returns:
        Migrated configuration dictionary
    """
    if "models" not in data:
        return data

    models = data["models"]
    migrations = [
        ("primary", "thinking"),
        ("workhorse", "normal"),
        ("fast", "code"),
    ]

    for old_name, new_name in migrations:
        if old_name in models and new_name not in models:
            models[new_name] = models.pop(old_name)

    # Migrate routing fallback_model if needed
    if "routing" in data:
        routing = data["routing"]
        fallback = routing.get("fallback_model")
        fallback_migrations = {
            "primary": "normal",
            "workhorse": "normal",
            "fast": "code",
        }
        if fallback in fallback_migrations:
            routing["fallback_model"] = fallback_migrations[fallback]

        # Migrate default model if specified
        if "default" in data.get("models", {}):
            default = data["models"]["default"]
            default_migrations = {
                "primary": "normal",
                "workhorse": "normal",
                "fast": "code",
            }
            if default in default_migrations:
                data["models"]["default"] = default_migrations[default]

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
            project_root: Project root (auto-detected if None, or use cwd)
        """
        self.global_config_dir = global_config_dir or Path.home() / ".entropi"
        # Use detected project root, or fall back to current directory
        self.project_root = project_root or find_project_root() or Path.cwd()

    def _ensure_global_config(self) -> None:
        """
        Ensure global ~/.entropi/ directory and config exist.

        Copies from package default if not present.
        """
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
        Ensure project .entropi/ directory and config exist.

        Seeding: global config.yaml → .entropi/config.local.yaml
        Migration: existing config.yaml → config.local.yaml
        """
        project_entropi_dir = self.project_root / ".entropi"
        local_config_path = project_entropi_dir / "config.local.yaml"
        legacy_config_path = project_entropi_dir / "config.yaml"
        global_config_path = self.global_config_dir / "config.yaml"

        # Create .entropi directory if it doesn't exist
        if not project_entropi_dir.exists():
            project_entropi_dir.mkdir(parents=True, exist_ok=True)

            # Create .gitignore for transient files only
            gitignore_path = project_entropi_dir / ".gitignore"
            gitignore_path.write_text("*.db\n*.log\n")

            # Create default ENTROPI.md
            entropi_md_path = project_entropi_dir / "ENTROPI.md"
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

        # Migration: if legacy config.yaml exists but
        # config.local.yaml doesn't, adopt the legacy file
        if legacy_config_path.exists() and not local_config_path.exists():
            shutil.copy(legacy_config_path, local_config_path)

        # Seed from global config if config.local.yaml doesn't exist
        if not local_config_path.exists():
            self._seed_project_config(
                local_config_path,
                global_config_path,
            )

    def _seed_project_config(
        self,
        target: Path,
        global_config: Path,
    ) -> None:
        """Seed project config.local.yaml from global config."""
        try:
            if global_config.exists():
                shutil.copy(global_config, target)
                return
        except PermissionError:
            pass

        # Fallback to package default
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
        # Ensure global ~/.entropi/ exists with default config
        try:
            self._ensure_global_config()
        except PermissionError:
            # Can't write to home directory (e.g., container)
            pass

        # Ensure project .entropi/ exists with config from global
        self._ensure_project_config()

        # Start with empty dict (defaults come from Pydantic)
        config: dict[str, Any] = {}

        # Layer 1: Global config (user defaults for all projects)
        try:
            global_path = self.global_config_dir / "config.yaml"
            global_config = _migrate_config(
                load_yaml_config(global_path),
            )
            config = deep_merge(config, global_config)
        except PermissionError:
            pass

        # Layer 2: Project config (source of truth)
        local_path = self.project_root / ".entropi" / "config.local.yaml"
        local_config = _migrate_config(
            load_yaml_config(local_path),
        )
        config = deep_merge(config, local_config)

        # Layer 3: CLI overrides
        if cli_overrides:
            config = deep_merge(config, cli_overrides)

        # Override config_dir to use project-level .entropi
        config["config_dir"] = str(self.project_root / ".entropi")

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
