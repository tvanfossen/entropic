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

    def test_empty_override(self) -> None:
        """Test merging with empty override."""
        base = {"a": 1, "b": 2}
        override: dict = {}
        result = deep_merge(base, override)
        assert result == {"a": 1, "b": 2}

    def test_empty_base(self) -> None:
        """Test merging with empty base."""
        base: dict = {}
        override = {"a": 1}
        result = deep_merge(base, override)
        assert result == {"a": 1}


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

    def test_load_empty_file(self) -> None:
        """Test loading empty YAML file returns empty dict."""
        with tempfile.NamedTemporaryFile(mode="w", suffix=".yaml", delete=False) as f:
            f.write("")
            f.flush()
            result = load_yaml_config(Path(f.name))
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

    def test_validation_context_length_min(self) -> None:
        """Test minimum context length validation."""
        with pytest.raises(ValueError):
            ModelConfig(path=Path("/test"), context_length=100)  # Below minimum of 512

    def test_validation_context_length_max(self) -> None:
        """Test maximum context length validation."""
        with pytest.raises(ValueError):
            ModelConfig(path=Path("/test"), context_length=200000)  # Above maximum

    def test_validation_temperature(self) -> None:
        """Test temperature validation."""
        with pytest.raises(ValueError):
            ModelConfig(path=Path("/test"), temperature=3.0)  # Above maximum of 2.0

    def test_default_routing_config(self) -> None:
        """Test default routing configuration."""
        config = EntropyConfig()
        assert config.routing.enabled is True
        assert config.routing.fallback_tier == "normal"

    def test_default_quality_rules(self) -> None:
        """Test default quality rules."""
        config = EntropyConfig()
        assert config.quality.rules.max_cognitive_complexity == 15
        assert config.quality.rules.require_type_hints is True
        assert config.quality.rules.docstring_style == "google"


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
            (entropi_dir / "config.yaml").write_text(yaml.dump({"routing": {"enabled": False}}))

            loader = ConfigLoader(
                global_config_dir=global_dir,
                project_root=project_dir,
            )
            config = loader.load()

            # Global log_level preserved
            assert config.log_level == "DEBUG"
            # Project routing overrides global
            assert config.routing.enabled is False

    def test_cli_overrides(self) -> None:
        """Test CLI overrides take precedence."""
        loader = ConfigLoader(
            global_config_dir=Path("/nonexistent"),
            project_root=None,
        )
        config = loader.load(cli_overrides={"log_level": "ERROR"})
        assert config.log_level == "ERROR"

    def test_ensure_directories(self) -> None:
        """Test directory creation."""
        with tempfile.TemporaryDirectory() as tmpdir:
            tmpdir = Path(tmpdir)

            config = EntropyConfig(
                config_dir=tmpdir / "config",
            )

            loader = ConfigLoader()
            loader.ensure_directories(config)

            # Only config_dir is created by ensure_directories
            assert (tmpdir / "config").exists()
