"""
Configuration loader with hierarchy support.

Hierarchy (lowest to highest priority):
1. Defaults (built into schema)
2. Global config (~/.entropi/config.yaml)
3. Project config (.entropi/config.yaml) - auto-created from global
4. Local config (.entropi/config.local.yaml) - gitignored
5. Environment variables (ENTROPI_*)
6. CLI arguments

Installation flow:
1. Package includes default_config.yaml in entropi/data/
2. On first run, if ~/.entropi/config.yaml doesn't exist, copy from package
3. On first run in a project, copy from ~/.entropi/config.yaml to .entropi/config.yaml

Per-project architecture:
- Each project gets its own .entropi/ folder
- Auto-created on first run with config copied from global
- Database is per-project for conversation history
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
        with importlib.resources.files("entropi.data").joinpath("default_config.yaml") as p:
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
        self.global_config_dir = (global_config_dir or Path.home() / ".entropi")
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

        Auto-creates from global config if not present.
        """
        project_entropi_dir = self.project_root / ".entropi"
        project_config_path = project_entropi_dir / "config.yaml"
        local_config_path = project_entropi_dir / "config.local.yaml"
        global_config_path = self.global_config_dir / "config.yaml"

        # Create .entropi directory if it doesn't exist
        if not project_entropi_dir.exists():
            project_entropi_dir.mkdir(parents=True, exist_ok=True)

            # Create .gitignore to ignore db, logs, and local config
            gitignore_path = project_entropi_dir / ".gitignore"
            gitignore_path.write_text("*.db\n*.log\nconfig.local.yaml\n")

            # Create default ENTROPI.md
            entropi_md_path = project_entropi_dir / "ENTROPI.md"
            entropi_md_path.write_text(
                "# ENTROPI.md\n\n"
                "This file provides project context to Entropi, your local AI coding assistant.\n\n"
                "Edit this file to describe your project, its structure, coding standards, "
                "and any other context that would help Entropi assist you more effectively.\n"
            )

        # Copy global config as template if project config doesn't exist
        if not project_config_path.exists():
            try:
                if global_config_path.exists():
                    shutil.copy(global_config_path, project_config_path)
            except PermissionError:
                # Global config not accessible - copy from package default
                default_config = get_default_config_path()
                if default_config.exists():
                    shutil.copy(default_config, project_config_path)

        # Create local config template if it doesn't exist
        if not local_config_path.exists():
            self._create_local_config_template(local_config_path)

    def _create_local_config_template(self, path: Path) -> None:
        """
        Create a template config.local.yaml with common override fields.

        This file is gitignored and intended for personal/machine-specific settings.
        """
        template = """\
# Entropi Local Configuration
# This file is gitignored - use for personal/machine-specific settings
# Overrides values from config.yaml

# Permissions - control tool execution
permissions:
  # Auto-approve all tool calls (skip confirmation prompts)
  auto_approve: false

  # Tools to allow (glob patterns supported)
  # allow:
  #   - "filesystem.*"
  #   - "git.*"
  #   - "bash.execute:pytest *"

  # Tools to deny (glob patterns supported)
  # deny:
  #   - "bash.execute:rm -rf *"

  # Tools that require confirmation (glob patterns)
  # prompt:
  #   - "bash.execute:*"
  #   - "filesystem.write_file:*"

# UI preferences
ui:
  theme: dark  # dark, light, auto
  # stream_output: true
  # show_token_count: true
  # show_timing: true

# Logging (useful for debugging)
# log_level: INFO  # DEBUG, INFO, WARNING, ERROR

# Model overrides (if you have different local paths)
# models:
#   default: normal  # thinking, normal, code, micro
#   normal:
#     path: ~/models/gguf/your-model.gguf
#     context_length: 16384
#     gpu_layers: -1
"""
        path.write_text(template)

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

        # Layer 1: Global config (for defaults/templates)
        try:
            global_config_path = self.global_config_dir / "config.yaml"
            global_config = _migrate_config(load_yaml_config(global_config_path))
            config = deep_merge(config, global_config)
        except PermissionError:
            # Global config not accessible (e.g., in container)
            pass

        # Layer 2: Project config (primary source)
        project_config_path = self.project_root / ".entropi" / "config.yaml"
        project_config = _migrate_config(load_yaml_config(project_config_path))
        config = deep_merge(config, project_config)

        # Layer 3: Local config (gitignored, for personal overrides)
        local_config_path = self.project_root / ".entropi" / "config.local.yaml"
        local_config = _migrate_config(load_yaml_config(local_config_path))
        config = deep_merge(config, local_config)

        # Layer 4: CLI overrides
        if cli_overrides:
            config = deep_merge(config, cli_overrides)

        # Override config_dir to use project-level .entropi
        config["config_dir"] = str(self.project_root / ".entropi")

        # Layer 5: Environment variable overrides (for debugging)
        # These take highest priority after CLI args
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
    Save a permission pattern to the local project config.

    Args:
        pattern: Permission pattern (e.g., "bash.execute:python -m venv *")
        allow: True to add to allow list, False to add to deny list
    """
    # Find project root
    project_root = find_project_root() or Path.cwd()
    config_path = project_root / ".entropi" / "config.yaml"

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
