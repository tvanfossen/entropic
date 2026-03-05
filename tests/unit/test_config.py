"""Tests for configuration system."""

import tempfile
from pathlib import Path

import pytest
import yaml
from entropic.config.loader import (
    ConfigLoader,
    _normalize_config,
    deep_merge,
    load_yaml_config,
    validate_config,
)
from entropic.config.schema import (
    CompactionConfig,
    EntropyConfig,
    LibraryConfig,
    ModelConfig,
    TierConfig,
)


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


class TestLibraryConfig:
    """Tests for LibraryConfig (library-only subset)."""

    def test_default_construction(self) -> None:
        """LibraryConfig constructs with sensible defaults."""
        config = LibraryConfig()
        assert config.log_level == "INFO"
        assert config.routing.enabled is True
        assert config.models.default == "conversational"

    def test_excludes_tui_fields(self) -> None:
        """LibraryConfig does not expose TUI-specific fields."""
        config = LibraryConfig()
        assert not hasattr(config, "quality")
        assert not hasattr(config, "ui")
        assert not hasattr(config, "storage")
        assert not hasattr(config, "lsp")
        assert not hasattr(config, "voice")
        assert not hasattr(config, "commands_dir")

    def test_entropy_config_inherits(self) -> None:
        """EntropyConfig is a subclass of LibraryConfig."""
        assert issubclass(EntropyConfig, LibraryConfig)
        config = EntropyConfig()
        assert isinstance(config, LibraryConfig)
        # Has library fields
        assert config.log_level == "INFO"
        # Also has TUI fields
        assert config.quality.enabled is True

    def test_inject_model_context_default_true(self) -> None:
        """inject_model_context defaults to True."""
        config = LibraryConfig()
        assert config.inject_model_context is True

    def test_inject_model_context_disabled(self) -> None:
        """inject_model_context can be set to False."""
        config = LibraryConfig(inject_model_context=False)
        assert config.inject_model_context is False

    def test_routing_validator_runs(self) -> None:
        """Cross-validation runs on LibraryConfig, not just EntropyConfig."""
        with pytest.raises(ValueError, match="fallback_tier"):
            LibraryConfig(
                models={"tiers": {"fast": {"path": "/m"}}, "default": "fast"},
                routing={"enabled": False, "fallback_tier": "nonexistent"},
            )


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

    def test_allowed_tools_valid_format(self) -> None:
        """Test allowed_tools accepts fully-qualified names."""
        config = ModelConfig(path=Path("/test"), allowed_tools=["server.tool", "entropic.handoff"])
        assert config.allowed_tools == ["server.tool", "entropic.handoff"]

    def test_allowed_tools_none_default(self) -> None:
        """Test allowed_tools defaults to None (all tools visible)."""
        config = ModelConfig(path=Path("/test"))
        assert config.allowed_tools is None

    def test_allowed_tools_rejects_unqualified(self) -> None:
        """Test allowed_tools rejects entries without server.tool format."""
        with pytest.raises(ValueError, match="allowed_tools entry 'badname'"):
            ModelConfig(path=Path("/test"), allowed_tools=["badname"])

    def test_default_routing_config(self) -> None:
        """Test default routing configuration."""
        config = EntropyConfig()
        assert config.routing.enabled is True
        assert config.routing.fallback_tier == "conversational"

    def test_default_quality_rules(self) -> None:
        """Test default quality rules."""
        config = EntropyConfig()
        assert config.quality.rules.max_cognitive_complexity == 15
        assert config.quality.rules.require_type_hints is True
        assert config.quality.rules.docstring_style == "google"


def _tier(path: str = "/test.gguf") -> TierConfig:
    """Create a minimal TierConfig for testing."""
    return TierConfig(path=Path(path))


class TestTierConfigFields:
    """Tests for TierConfig auto_chain and enable_thinking fields."""

    def test_auto_chain_defaults_false(self) -> None:
        """auto_chain defaults to False."""
        tc = _tier()
        assert tc.auto_chain is False

    def test_auto_chain_true(self) -> None:
        """auto_chain=True round-trips through config."""
        tc = TierConfig(path=Path("/test.gguf"), auto_chain=True)
        assert tc.auto_chain is True

    def test_full_config_with_auto_chain(self) -> None:
        """EntropyConfig with auto_chain tier parses correctly."""
        config = EntropyConfig(
            models={
                "tiers": {
                    "thinker": {
                        "path": "/test.gguf",
                        "auto_chain": True,
                    },
                    "executor": {
                        "path": "/test.gguf",
                    },
                },
                "default": "thinker",
            },
            routing={"enabled": False, "fallback_tier": "thinker"},
        )
        assert config.models.tiers["thinker"].auto_chain is True
        assert config.models.tiers["executor"].auto_chain is False


class TestTierConfigGrammar:
    """Tests for TierConfig grammar field."""

    def test_grammar_none_default(self) -> None:
        """TierConfig without grammar defaults to None."""
        tc = _tier()
        assert tc.grammar is None

    def test_grammar_path_coercion(self) -> None:
        """String grammar path is coerced to Path."""
        tc = TierConfig(path=Path("/test.gguf"), grammar="data/grammars/chess.gbnf")
        assert isinstance(tc.grammar, Path)
        assert tc.grammar == Path("data/grammars/chess.gbnf")

    def test_grammar_expanduser(self) -> None:
        """Grammar path with ~ is expanded."""
        tc = TierConfig(path=Path("/test.gguf"), grammar="~/grammars/foo.gbnf")
        assert isinstance(tc.grammar, Path)
        assert "~" not in str(tc.grammar)


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


class TestRoutingConfigNoGrammar:
    """Tests confirming use_grammar was removed from RoutingConfig."""

    def test_use_grammar_not_a_field(self) -> None:
        """RoutingConfig no longer has use_grammar as a model field."""
        from entropic.config.schema import RoutingConfig

        config = RoutingConfig()
        assert "use_grammar" not in config.model_fields

    def test_use_grammar_not_in_schema(self) -> None:
        """use_grammar does not appear in RoutingConfig JSON schema."""
        from entropic.config.schema import RoutingConfig

        schema = RoutingConfig.model_json_schema()
        assert "use_grammar" not in schema.get("properties", {})


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
            entropic_dir = project_dir / ".entropic"
            entropic_dir.mkdir(parents=True)
            (entropic_dir / "config.yaml").write_text(yaml.dump({"routing": {"enabled": False}}))

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


class TestValidateConfig:
    """Tests for standalone validate_config() function."""

    def test_valid_config_returns_empty(self) -> None:
        """Valid config dict returns no errors."""
        data = {
            "models": {"tiers": {"normal": _tier()}, "default": "normal"},
            "routing": {"enabled": False, "fallback_tier": "normal"},
        }
        assert validate_config(data) == []

    def test_invalid_config_returns_errors(self) -> None:
        """Invalid config returns list of error messages."""
        data = {"log_level": "BOGUS"}
        errors = validate_config(data)
        assert len(errors) > 0
        assert any("DEBUG" in e or "INFO" in e for e in errors)

    def test_cross_validator_caught(self) -> None:
        """Cross-validators (e.g. routing references) produce errors."""
        data = {
            "models": {"tiers": {"normal": _tier()}, "default": "bogus_tier"},
            "routing": {"enabled": False, "fallback_tier": "normal"},
        }
        errors = validate_config(data)
        assert len(errors) > 0

    def test_yaml_file_path(self, tmp_path: Path) -> None:
        """Accepts a Path to a YAML file."""
        config_file = tmp_path / "test.yaml"
        config_file.write_text(
            yaml.dump(
                {
                    "models": {
                        "tiers": {"normal": {"path": "/m.gguf"}},
                        "default": "normal",
                    },
                    "routing": {"enabled": False, "fallback_tier": "normal"},
                }
            )
        )
        assert validate_config(config_file) == []

    def test_missing_yaml_file(self, tmp_path: Path) -> None:
        """Non-existent YAML file returns empty errors (empty dict is valid defaults)."""
        missing = tmp_path / "nope.yaml"
        assert validate_config(missing) == []

    def test_flat_tiers_normalized(self) -> None:
        """Flat tier config (tiers as top-level keys) is normalized before validation."""
        data = {
            "models": {"mytier": {"path": "/m.gguf"}, "default": "mytier"},
            "routing": {"enabled": False, "fallback_tier": "mytier"},
        }
        assert validate_config(data) == []

    def test_normalize_config_flat_to_nested(self) -> None:
        """_normalize_config moves flat tiers into models.tiers dict."""
        data = {
            "models": {
                "fast": {"path": "/fast.gguf"},
                "slow": {"path": "/slow.gguf"},
                "default": "fast",
            },
        }
        result = _normalize_config(data)
        assert "tiers" in result["models"]
        assert "fast" in result["models"]["tiers"]
        assert "slow" in result["models"]["tiers"]
        # Reserved keys stay at top level
        assert result["models"]["default"] == "fast"
