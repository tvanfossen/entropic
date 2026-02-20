"""Tests for prompt loading and classification prompt building."""

import tempfile
from pathlib import Path

import pytest
from entropic.inference.adapters.base import GenericAdapter
from entropic.prompts import (
    AppContextFrontmatter,
    ConstitutionFrontmatter,
    IdentityFrontmatter,
    _resolve_identity_path,
    get_tier_identity_prompt,
    load_tier_identity,
    parse_prompt_file,
)
from pydantic import ValidationError

TIERS = ["simple", "code", "normal", "thinking"]


class TestParsePromptFile:
    """Tests for parse_prompt_file with typed frontmatter validation."""

    def test_parses_constitution(self, tmp_path: Path) -> None:
        path = tmp_path / "constitution.md"
        path.write_text("---\ntype: constitution\nversion: 1\n---\n\n# Body\n")
        fm, body = parse_prompt_file(path, "constitution")
        assert isinstance(fm, ConstitutionFrontmatter)
        assert fm.version == 1
        assert "# Body" in body

    def test_parses_app_context(self, tmp_path: Path) -> None:
        path = tmp_path / "app_context.md"
        path.write_text("---\ntype: app_context\nversion: 1\n---\n\n# Context\n")
        fm, body = parse_prompt_file(path, "app_context")
        assert isinstance(fm, AppContextFrontmatter)
        assert "# Context" in body

    def test_parses_identity(self, tmp_path: Path) -> None:
        path = tmp_path / "identity_test.md"
        path.write_text(
            "---\ntype: identity\nversion: 1\nname: test\n"
            "focus:\n  - item one\n  - item two\n---\n\n# Body\n"
        )
        fm, body = parse_prompt_file(path, "identity")
        assert isinstance(fm, IdentityFrontmatter)
        assert fm.name == "test"
        assert fm.focus == ["item one", "item two"]
        assert "# Body" in body

    def test_raises_on_missing_frontmatter(self, tmp_path: Path) -> None:
        path = tmp_path / "no_fm.md"
        path.write_text("# No frontmatter\n")
        with pytest.raises(ValueError, match="missing YAML frontmatter"):
            parse_prompt_file(path, "constitution")

    def test_raises_on_malformed_frontmatter(self, tmp_path: Path) -> None:
        path = tmp_path / "bad.md"
        path.write_text("---\njust a string\n")
        with pytest.raises(ValueError, match="malformed frontmatter"):
            parse_prompt_file(path, "constitution")

    def test_raises_on_type_mismatch(self, tmp_path: Path) -> None:
        path = tmp_path / "wrong.md"
        path.write_text("---\ntype: identity\nversion: 1\nname: x\nfocus:\n  - y\n---\n\nBody\n")
        with pytest.raises(ValueError, match="type 'identity' but was loaded as 'constitution'"):
            parse_prompt_file(path, "constitution")

    def test_raises_on_invalid_version(self, tmp_path: Path) -> None:
        path = tmp_path / "bad_ver.md"
        path.write_text("---\ntype: constitution\nversion: 0\n---\n\nBody\n")
        with pytest.raises(ValidationError):
            parse_prompt_file(path, "constitution")

    def test_raises_on_missing_version(self, tmp_path: Path) -> None:
        path = tmp_path / "no_ver.md"
        path.write_text("---\ntype: constitution\n---\n\nBody\n")
        with pytest.raises(ValidationError):
            parse_prompt_file(path, "constitution")

    def test_identity_requires_focus(self, tmp_path: Path) -> None:
        path = tmp_path / "no_focus.md"
        path.write_text("---\ntype: identity\nversion: 1\nname: x\nfocus: []\n---\n\nBody\n")
        with pytest.raises(ValidationError):
            parse_prompt_file(path, "identity")

    def test_identity_examples_optional(self, tmp_path: Path) -> None:
        path = tmp_path / "no_ex.md"
        path.write_text("---\ntype: identity\nversion: 1\nname: x\nfocus:\n  - y\n---\n\nBody\n")
        fm, _body = parse_prompt_file(path, "identity")
        assert isinstance(fm, IdentityFrontmatter)
        assert fm.examples == []

    def test_non_mapping_frontmatter_raises(self, tmp_path: Path) -> None:
        path = tmp_path / "list_fm.md"
        path.write_text("---\n- item1\n- item2\n---\n\nBody\n")
        with pytest.raises(ValueError, match="not a YAML mapping"):
            parse_prompt_file(path, "constitution")


class TestLoadTierIdentity:
    """Tests for load_tier_identity with YAML frontmatter."""

    def test_parses_frontmatter_and_body(self) -> None:
        content = (
            "---\ntype: identity\nversion: 1\nname: test\n"
            "focus:\n  - item one\n  - item two\n---\n\n# Body\n"
        )
        with tempfile.NamedTemporaryFile(mode="w", suffix=".md", delete=False) as f:
            f.write(content)
            f.flush()
            identity, body = load_tier_identity(Path(f.name))
        assert identity.name == "test"
        assert identity.focus == ["item one", "item two"]
        assert "# Body" in body

    def test_raises_when_frontmatter_missing(self) -> None:
        content = "# No frontmatter\n"
        with tempfile.NamedTemporaryFile(mode="w", suffix=".md", delete=False) as f:
            f.write(content)
            f.flush()
            with pytest.raises(ValueError, match="missing YAML frontmatter"):
                load_tier_identity(Path(f.name))

    def test_raises_when_focus_empty(self) -> None:
        content = "---\ntype: identity\nversion: 1\nname: test\nfocus: []\n---\n\n# Body\n"
        with tempfile.NamedTemporaryFile(mode="w", suffix=".md", delete=False) as f:
            f.write(content)
            f.flush()
            with pytest.raises(ValidationError):
                load_tier_identity(Path(f.name))

    def test_examples_optional(self) -> None:
        content = "---\ntype: identity\nversion: 1\nname: test\nfocus:\n  - item\n---\n\n# Body\n"
        with tempfile.NamedTemporaryFile(mode="w", suffix=".md", delete=False) as f:
            f.write(content)
            f.flush()
            identity, _body = load_tier_identity(Path(f.name))
        assert identity.examples == []


class TestBundledPromptFiles:
    """Validate ALL bundled prompt files have valid typed frontmatter."""

    @pytest.mark.parametrize("tier", TIERS)
    def test_identity_has_valid_frontmatter(self, tier: str) -> None:
        """Each tier identity file MUST have valid typed YAML frontmatter."""
        path = _resolve_identity_path(tier)
        assert path is not None, f"identity_{tier}.md not found"
        fm, body = parse_prompt_file(path, "identity")
        assert isinstance(fm, IdentityFrontmatter)
        assert len(fm.focus) > 0, f"identity_{tier}.md has empty focus"
        assert len(body) > 0, f"identity_{tier}.md has empty body"

    @pytest.mark.parametrize("tier", TIERS)
    def test_identity_name_matches_tier(self, tier: str) -> None:
        """Frontmatter name must match the tier filename."""
        path = _resolve_identity_path(tier)
        assert path is not None
        fm, _body = parse_prompt_file(path, "identity")
        assert isinstance(fm, IdentityFrontmatter)
        assert fm.name == tier

    def test_constitution_has_valid_frontmatter(self) -> None:
        """Bundled constitution.md must have typed frontmatter."""
        from entropic.prompts import _DATA_DIR

        fm, body = parse_prompt_file(_DATA_DIR / "constitution.md", "constitution")
        assert isinstance(fm, ConstitutionFrontmatter)
        assert fm.version == 1
        assert len(body) > 0

    def test_app_context_has_valid_frontmatter(self) -> None:
        """Bundled app_context.md must have typed frontmatter."""
        from entropic.prompts import _DATA_DIR

        fm, body = parse_prompt_file(_DATA_DIR / "app_context.md", "app_context")
        assert isinstance(fm, AppContextFrontmatter)
        assert fm.version == 1
        assert len(body) > 0


def _make_tool_def(name: str) -> dict[str, object]:
    """Create a minimal tool definition for testing."""
    return {
        "name": name,
        "description": f"Tool {name}",
        "inputSchema": {"type": "object", "properties": {}},
    }


class TestToolIsolation:
    """Assembled system prompt must contain ZERO unauthorized tool names."""

    ALL_TOOLS = [
        "filesystem.read_file",
        "filesystem.write_file",
        "filesystem.edit_file",
        "bash.execute",
        "git.status",
        "git.add",
        "git.commit",
        "git.diff",
        "git.log",
        "git.branch",
        "git.checkout",
        "git.reset",
        "entropic.todo_write",
        "entropic.handoff",
        "diagnostics.check_errors",
    ]

    def test_thinking_tier_only_sees_allowed_tools(self) -> None:
        """Thinking tier with allowed tools must not leak other names."""
        allowed = [
            "entropic.todo_write",
            "entropic.handoff",
            "filesystem.read_file",
            "bash.execute",
        ]
        forbidden = [t for t in self.ALL_TOOLS if t not in allowed]

        adapter = GenericAdapter(tier="thinking")
        tools = [_make_tool_def(name) for name in allowed]
        prompt = adapter.format_system_prompt("", tools)

        for name in allowed:
            assert name in prompt, f"Allowed tool '{name}' missing"

        for name in forbidden:
            assert name not in prompt, f"Forbidden tool '{name}' leaked"

    def test_simple_tier_only_sees_handoff(self) -> None:
        """Simple tier with only handoff must not leak any other tools."""
        allowed = ["entropic.handoff"]
        forbidden = [t for t in self.ALL_TOOLS if t not in allowed]

        adapter = GenericAdapter(tier="simple")
        tools = [_make_tool_def(name) for name in allowed]
        prompt = adapter.format_system_prompt("", tools)

        assert "entropic.handoff" in prompt
        for name in forbidden:
            assert name not in prompt, f"Forbidden tool '{name}' leaked"

    def test_thinking_identity_has_first_action_section(self) -> None:
        """Thinking tier must instruct model to create todos before tool use."""
        identity = get_tier_identity_prompt("thinking")
        assert "## First Action" in identity
        assert "entropic.todo_write" in identity

    def test_thinking_identity_has_no_unauthorized_tools(self) -> None:
        """Thinking tier identity must not mention tools outside its set."""
        thinking_allowed = {
            "entropic.todo_write",
            "entropic.handoff",
            "filesystem.read_file",
            "bash.execute",
        }
        forbidden = [t for t in self.ALL_TOOLS if t not in thinking_allowed]

        identity = get_tier_identity_prompt("thinking")
        for name in forbidden:
            assert name not in identity, f"identity_thinking.md mentions unauthorized tool '{name}'"

    @pytest.mark.parametrize("tier", TIERS)
    def test_tool_definitions_only_for_allowed(self, tier: str) -> None:
        """Tool definitions must only include tools in the filtered set."""
        allowed = ["filesystem.read_file"]
        # Tools that should never appear in tool-definition sections
        # (identity prompts may mention tool names — that's expected coupling)
        leaked_names = [
            "filesystem.write_file",
            "filesystem.edit_file",
        ]

        adapter = GenericAdapter(tier=tier)
        tools = [_make_tool_def(name) for name in allowed]
        prompt = adapter.format_system_prompt("", tools)

        # Check tool definition sections only (not identity)
        identity = adapter._get_identity_prompt()
        non_identity = prompt[len(identity) :]

        for name in leaked_names:
            assert name not in non_identity, f"Tool '{name}' leaked into {tier} tier"


class TestUseBundledPrompts:
    """Tests for use_bundled_prompts=False skipping bundled fallback."""

    def test_load_prompt_raises_when_bundled_disabled(self) -> None:
        """load_prompt with use_bundled=False raises if not in prompts_dir."""
        from entropic.prompts import load_prompt

        with pytest.raises(FileNotFoundError):
            load_prompt("constitution", prompts_dir=None, use_bundled=False)

    def test_load_prompt_uses_prompts_dir_when_bundled_disabled(self, tmp_path: Path) -> None:
        """load_prompt finds user file even with use_bundled=False."""
        from entropic.prompts import load_prompt

        user_prompt = tmp_path / "constitution.md"
        user_prompt.write_text("Custom constitution")

        result = load_prompt("constitution", prompts_dir=tmp_path, use_bundled=False)
        assert result == "Custom constitution"

    def test_resolve_identity_path_none_when_bundled_disabled(self) -> None:
        """_resolve_identity_path returns None for bundled tiers when disabled."""
        result = _resolve_identity_path("thinking", prompts_dir=None, use_bundled=False)
        assert result is None


class TestPromptsDir:
    """Tests for prompts_dir enforcement — no silent fallback to bundled."""

    def test_strict_load_prompt_raises_when_missing(self, tmp_path: Path) -> None:
        """prompts_dir + use_bundled=False, file missing → error."""
        from entropic.prompts import load_prompt

        with pytest.raises(FileNotFoundError, match="not found in"):
            load_prompt("constitution", prompts_dir=tmp_path, use_bundled=False)

    def test_strict_resolve_identity_no_fallback(self, tmp_path: Path) -> None:
        """_resolve_identity_path returns None in strict mode (no fallback)."""
        result = _resolve_identity_path("thinking", prompts_dir=tmp_path, use_bundled=False)
        assert result is None

    def test_prompts_dir_with_bundled_falls_back(self, tmp_path: Path) -> None:
        """prompts_dir set but use_bundled=True → falls back to bundled."""
        from entropic.prompts import load_prompt

        # tmp_path has no constitution.md, but bundled exists
        result = load_prompt("constitution", prompts_dir=tmp_path, use_bundled=True)
        assert len(result) > 0  # Got bundled constitution

    def test_load_prompt_finds_file_in_prompts_dir(self, tmp_path: Path) -> None:
        """prompts_dir set and file exists → loads from prompts_dir."""
        from entropic.prompts import load_prompt

        (tmp_path / "constitution.md").write_text("Custom content")
        result = load_prompt("constitution", prompts_dir=tmp_path)
        assert result == "Custom content"

    def test_strict_get_identity_prompt_raises(self, tmp_path: Path) -> None:
        """get_identity_prompt in strict mode raises when identity file missing."""
        from entropic.prompts import get_identity_prompt

        (tmp_path / "constitution.md").write_text("Constitution")

        with pytest.raises(FileNotFoundError, match="identity_custom"):
            get_identity_prompt("custom", prompts_dir=tmp_path, use_bundled=False)

    def test_resolve_identity_path_finds_user_file(self, tmp_path: Path) -> None:
        """_resolve_identity_path finds user file even with use_bundled=False."""
        identity = tmp_path / "identity_custom.md"
        identity.write_text(
            "---\ntype: identity\nversion: 1\nname: custom\nfocus:\n  - testing\n---\nBody"
        )

        result = _resolve_identity_path("custom", prompts_dir=tmp_path, use_bundled=False)
        assert result == identity


class TestProgrammaticConfig:
    """Tests that EntropyConfig works without ConfigLoader or files."""

    def test_entropy_config_constructs_without_files(self) -> None:
        """EntropyConfig(...) works with just keyword args, no disk I/O."""
        from entropic.config.schema import EntropyConfig

        config = EntropyConfig(
            models={"tiers": {"custom": {"path": "/tmp/model.gguf"}}, "default": "custom"},
            routing={"enabled": False, "fallback_tier": "custom"},
        )
        assert config.models.default == "custom"
        assert "custom" in config.models.tiers
        assert config.routing.enabled is False

    def test_entropy_config_with_use_bundled_false(self) -> None:
        """use_bundled_prompts=False is accepted by EntropyConfig."""
        from entropic.config.schema import EntropyConfig

        config = EntropyConfig(use_bundled_prompts=False)
        assert config.use_bundled_prompts is False
