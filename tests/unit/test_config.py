"""Tests for configuration system."""

import tempfile
from pathlib import Path

import pytest
import yaml
from entropi.config.loader import ConfigLoader, deep_merge, load_yaml_config
from entropi.config.schema import CompactionConfig, EntropyConfig, ModelConfig, TierConfig


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


def _tier(path: str = "/test.gguf") -> TierConfig:
    """Create a minimal TierConfig for testing."""
    return TierConfig(path=Path(path))


class TestRoutingCrossValidation:
    """Tests for cross-validation between routing and models config."""

    def test_valid_fallback_tier(self) -> None:
        """Config with fallback_tier matching a defined tier passes."""
        config = EntropyConfig(
            models={"tiers": {"normal": _tier()}, "default": "normal"},
            routing={"enabled": False, "fallback_tier": "normal"},
        )
        assert config.routing.fallback_tier == "normal"

    def test_invalid_fallback_tier(self) -> None:
        """Config with fallback_tier not in tiers raises."""
        with pytest.raises(ValueError, match="routing.fallback_tier 'missing'"):
            EntropyConfig(
                models={"tiers": {"normal": _tier()}, "default": "normal"},
                routing={"enabled": False, "fallback_tier": "missing"},
            )

    def test_no_tiers_skips_validation(self) -> None:
        """Config with no tiers defined skips routing validation."""
        config = EntropyConfig(routing={"fallback_tier": "anything"})
        assert config.routing.fallback_tier == "anything"

    def test_valid_tier_map(self) -> None:
        """Config with tier_map values matching defined tiers passes."""
        config = EntropyConfig(
            models={"tiers": {"a": _tier(), "b": _tier()}, "default": "a"},
            routing={"enabled": False, "fallback_tier": "a", "tier_map": {"1": "a", "2": "b"}},
        )
        assert config.routing.tier_map == {"1": "a", "2": "b"}

    def test_invalid_tier_map_value(self) -> None:
        """Config with tier_map value not in tiers raises."""
        with pytest.raises(ValueError, match="routing.tier_map"):
            EntropyConfig(
                models={"tiers": {"a": _tier()}, "default": "a"},
                routing={"enabled": False, "fallback_tier": "a", "tier_map": {"1": "bogus"}},
            )

    def test_valid_handoff_rules(self) -> None:
        """Config with handoff_rules referencing defined tiers passes."""
        config = EntropyConfig(
            models={"tiers": {"a": _tier(), "b": _tier()}, "default": "a"},
            routing={
                "enabled": False,
                "fallback_tier": "a",
                "handoff_rules": {"a": ["b"], "b": ["a"]},
            },
        )
        assert config.routing.handoff_rules == {"a": ["b"], "b": ["a"]}

    def test_invalid_handoff_rules_key(self) -> None:
        """Config with handoff_rules key not in tiers raises."""
        with pytest.raises(ValueError, match="routing.handoff_rules key 'bogus'"):
            EntropyConfig(
                models={"tiers": {"a": _tier()}, "default": "a"},
                routing={
                    "enabled": False,
                    "fallback_tier": "a",
                    "handoff_rules": {"bogus": ["a"]},
                },
            )

    def test_invalid_handoff_rules_target(self) -> None:
        """Config with handoff_rules target not in tiers raises."""
        with pytest.raises(ValueError, match="contains 'bogus'"):
            EntropyConfig(
                models={"tiers": {"a": _tier()}, "default": "a"},
                routing={
                    "enabled": False,
                    "fallback_tier": "a",
                    "handoff_rules": {"a": ["bogus"]},
                },
            )

    def test_empty_tier_map_and_handoff_rules_valid(self) -> None:
        """Empty tier_map and handoff_rules are valid (auto-derived)."""
        config = EntropyConfig(
            models={"tiers": {"normal": _tier()}, "default": "normal"},
            routing={"enabled": False, "fallback_tier": "normal"},
        )
        assert config.routing.tier_map == {}
        assert config.routing.handoff_rules == {}

    def test_routing_enabled_without_router_raises(self) -> None:
        """Routing enabled but no router configured raises."""
        with pytest.raises(ValueError, match="models.router is not configured"):
            EntropyConfig(
                models={"tiers": {"normal": _tier()}, "default": "normal"},
                routing={"enabled": True, "fallback_tier": "normal"},
            )

    def test_routing_enabled_with_router_passes(self) -> None:
        """Routing enabled with router configured passes."""
        config = EntropyConfig(
            models={
                "tiers": {"normal": _tier()},
                "default": "normal",
                "router": {"path": "/router.gguf"},
            },
            routing={"enabled": True, "fallback_tier": "normal"},
        )
        assert config.routing.enabled is True
        assert config.models.router is not None


class TestCompactionThresholdValidation:
    """Tests for compaction threshold ordering validation."""

    def test_valid_thresholds(self) -> None:
        """Warning threshold below compaction threshold passes."""
        config = CompactionConfig(warning_threshold_percent=0.6, threshold_percent=0.75)
        assert config.warning_threshold_percent < config.threshold_percent

    def test_warning_equals_threshold_raises(self) -> None:
        """Warning threshold equal to compaction threshold raises."""
        with pytest.raises(ValueError, match="warning_threshold_percent"):
            CompactionConfig(warning_threshold_percent=0.75, threshold_percent=0.75)

    def test_warning_above_threshold_raises(self) -> None:
        """Warning threshold above compaction threshold raises."""
        with pytest.raises(ValueError, match="warning_threshold_percent"):
            CompactionConfig(warning_threshold_percent=0.8, threshold_percent=0.75)


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
