"""Tests for PromptManager — central prompt loading, validation, caching."""

from pathlib import Path

import pytest
from entropic.prompts.manager import PromptManager


def _write_prompt(path: Path, prompt_type: str, version: int = 1, **extra: str) -> None:
    """Write a prompt file with valid frontmatter."""
    frontmatter_lines = [f"type: {prompt_type}", f"version: {version}"]
    for k, v in extra.items():
        frontmatter_lines.append(f"{k}: {v}")
    fm = "\n".join(frontmatter_lines)
    path.write_text(f"---\n{fm}\n---\n\n# Body of {prompt_type}\n")


def _write_identity(path: Path, name: str, version: int = 1) -> None:
    """Write an identity prompt file."""
    path.write_text(
        f"---\ntype: identity\nversion: {version}\nname: {name}\n"
        f"focus:\n  - testing {name}\n---\n\n# {name.title()} Identity\n"
    )


class TestPromptManagerDefaults:
    """Test default (bundled) prompt loading."""

    def test_bundled_constitution_loads(self) -> None:
        pm = PromptManager(quiet=True)
        body = pm.get_constitution()
        assert body is not None
        assert "Core Principles" in body

    def test_app_context_disabled_by_default(self) -> None:
        pm = PromptManager(quiet=True)
        assert pm.get_app_context() is None
        assert not pm.has_app_context

    def test_bundled_identities_load(self) -> None:
        pm = PromptManager(
            tier_identities={"normal": None, "thinking": None},
            quiet=True,
        )
        assert pm.get_identity("normal") is not None
        assert pm.get_identity("thinking") is not None
        assert "Normal" in (pm.get_identity("normal") or "")

    def test_missing_bundled_identity_warns(self) -> None:
        pm = PromptManager(
            tier_identities={"nonexistent_tier": None},
            quiet=True,
        )
        assert pm.get_identity("nonexistent_tier") is None


class TestPromptManagerDisable:
    """Test disabling prompts via False."""

    def test_constitution_disabled(self) -> None:
        pm = PromptManager(constitution=False, quiet=True)
        assert pm.get_constitution() is None
        assert not pm.has_constitution

    def test_app_context_disabled(self) -> None:
        pm = PromptManager(app_context=False, quiet=True)
        assert pm.get_app_context() is None

    def test_identity_disabled(self) -> None:
        pm = PromptManager(
            tier_identities={"normal": False},
            quiet=True,
        )
        assert pm.get_identity("normal") is None


class TestPromptManagerCustomPaths:
    """Test custom prompt file loading."""

    def test_custom_constitution(self, tmp_path: Path) -> None:
        path = tmp_path / "my_constitution.md"
        _write_prompt(path, "constitution")
        pm = PromptManager(constitution=path, quiet=True)
        assert pm.has_constitution
        assert "Body of constitution" in (pm.get_constitution() or "")

    def test_custom_app_context(self, tmp_path: Path) -> None:
        path = tmp_path / "my_app.md"
        _write_prompt(path, "app_context")
        pm = PromptManager(app_context=path, quiet=True)
        assert pm.has_app_context
        assert "Body of app_context" in (pm.get_app_context() or "")

    def test_custom_identity(self, tmp_path: Path) -> None:
        path = tmp_path / "identity_custom.md"
        _write_identity(path, "custom")
        pm = PromptManager(
            tier_identities={"custom": path},
            quiet=True,
        )
        assert "Custom Identity" in (pm.get_identity("custom") or "")

    def test_missing_custom_file_raises(self, tmp_path: Path) -> None:
        with pytest.raises(FileNotFoundError):
            PromptManager(constitution=tmp_path / "nope.md", quiet=True)

    def test_type_mismatch_raises(self, tmp_path: Path) -> None:
        path = tmp_path / "wrong_type.md"
        _write_identity(path, "wrong")
        with pytest.raises(ValueError, match="type 'identity' but was loaded as 'constitution'"):
            PromptManager(constitution=path, quiet=True)


class TestAssembledPrompt:
    """Test get_assembled_prompt assembly order."""

    def test_full_assembly(self, tmp_path: Path) -> None:
        const_path = tmp_path / "constitution.md"
        const_path.write_text("---\ntype: constitution\nversion: 1\n---\n\nCONSTITUTION_BODY")

        app_path = tmp_path / "app.md"
        app_path.write_text("---\ntype: app_context\nversion: 1\n---\n\nAPP_BODY")

        id_path = tmp_path / "id.md"
        id_path.write_text(
            "---\ntype: identity\nversion: 1\nname: test\n"
            "focus:\n  - testing\n---\n\nIDENTITY_BODY"
        )

        pm = PromptManager(
            constitution=const_path,
            app_context=app_path,
            tier_identities={"test": id_path},
            quiet=True,
        )
        assembled = pm.get_assembled_prompt("test")

        # Order: constitution, identity, app_context
        const_pos = assembled.index("CONSTITUTION_BODY")
        id_pos = assembled.index("IDENTITY_BODY")
        app_pos = assembled.index("APP_BODY")
        assert const_pos < id_pos < app_pos

    def test_assembly_with_disabled_parts(self) -> None:
        pm = PromptManager(
            constitution=False,
            app_context=False,
            tier_identities={"normal": None},
            quiet=True,
        )
        assembled = pm.get_assembled_prompt("normal")
        assert "Normal" in assembled
        assert "Constitution" not in assembled

    def test_assembly_empty_when_all_disabled(self) -> None:
        pm = PromptManager(
            constitution=False,
            app_context=False,
            quiet=True,
        )
        assert pm.get_assembled_prompt("anything") == ""


class TestIdentityFrontmatter:
    """Test identity frontmatter access."""

    def test_get_identity_frontmatter(self) -> None:
        pm = PromptManager(
            tier_identities={"thinking": None},
            quiet=True,
        )
        fm = pm.get_identity_frontmatter("thinking")
        assert fm is not None
        assert fm.name == "thinking"
        assert len(fm.focus) > 0

    def test_frontmatter_none_for_missing_tier(self) -> None:
        pm = PromptManager(quiet=True)
        assert pm.get_identity_frontmatter("nonexistent") is None


class TestTerminalOutput:
    """Test prompt loading summary output."""

    def test_quiet_suppresses_output(self, capsys: pytest.CaptureFixture[str]) -> None:
        PromptManager(quiet=True)
        assert capsys.readouterr().out == ""

    def test_default_prints_summary(self, capsys: pytest.CaptureFixture[str]) -> None:
        PromptManager(
            tier_identities={"normal": None},
            quiet=False,
        )
        output = capsys.readouterr().out
        assert "Prompts:" in output
        assert "constitution" in output
        assert "bundled" in output
        assert "normal:" in output

    def test_disabled_shows_disabled(self, capsys: pytest.CaptureFixture[str]) -> None:
        PromptManager(constitution=False, quiet=False)
        output = capsys.readouterr().out
        assert "disabled" in output

    def test_custom_shows_path(self, tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
        path = tmp_path / "constitution.md"
        _write_prompt(path, "constitution")
        PromptManager(constitution=path, quiet=False)
        output = capsys.readouterr().out
        assert str(path) in output


class TestFromConfig:
    """Test PromptManager.from_config factory."""

    def test_from_entropy_config_defaults(self) -> None:
        from entropic.config.schema import EntropyConfig

        config = EntropyConfig()
        pm = PromptManager.from_config(config, quiet=True)
        assert pm.has_constitution
        assert not pm.has_app_context

    def test_from_config_with_tiers(self) -> None:
        from entropic.config.schema import EntropyConfig

        config = EntropyConfig(
            models={
                "tiers": {
                    "normal": {"path": "/tmp/model.gguf"},
                    "thinking": {"path": "/tmp/model.gguf"},
                },
                "default": "normal",
            },
            routing={"enabled": False, "fallback_tier": "normal"},
        )
        pm = PromptManager.from_config(config, quiet=True)
        assert pm.get_identity("normal") is not None
        assert pm.get_identity("thinking") is not None

    def test_from_config_constitution_disabled(self) -> None:
        from entropic.config.schema import EntropyConfig

        config = EntropyConfig(constitution=False)
        pm = PromptManager.from_config(config, quiet=True)
        assert not pm.has_constitution
