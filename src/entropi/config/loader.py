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

from pathlib import Path
from typing import Any

import yaml

from entropi.config.schema import EntropyConfig


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
            project_root: Project root (auto-detected if None)
        """
        self.global_config_dir = (global_config_dir or Path("~/.entropi")).expanduser()
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
