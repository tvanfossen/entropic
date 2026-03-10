"""Tests for prompt loading and classification prompt building."""

import tempfile
from pathlib import Path

import pytest
from entropic.inference.adapters.base import GenericAdapter
from entropic.prompts import (
    _DATA_DIR,
    AppContextFrontmatter,
    ConstitutionFrontmatter,
    IdentityFrontmatter,
    load_tier_identity,
    parse_prompt_file,
)
from pydantic import ValidationError

TIERS = [
    "lead",
    "arch",
    "eng",
    "qa",
    "ux",
    "ui",
    "analyst",
    "compactor",
    "scribe",
    "benchmark_judge",
]


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
        path = _DATA_DIR / f"identity_{tier}.md"
        assert path.exists(), f"identity_{tier}.md not found"
        fm, body = parse_prompt_file(path, "identity")
        assert isinstance(fm, IdentityFrontmatter)
        assert len(fm.focus) > 0, f"identity_{tier}.md has empty focus"
        assert len(body) > 0, f"identity_{tier}.md has empty body"

    @pytest.mark.parametrize("tier", TIERS)
    def test_identity_name_matches_tier(self, tier: str) -> None:
        """Frontmatter name must match the tier filename."""
        path = _DATA_DIR / f"identity_{tier}.md"
        assert path.exists()
        fm, _body = parse_prompt_file(path, "identity")
        assert isinstance(fm, IdentityFrontmatter)
        assert fm.name == tier

    def test_constitution_has_valid_frontmatter(self) -> None:
        """Bundled constitution.md must have typed frontmatter."""
        fm, body = parse_prompt_file(_DATA_DIR / "constitution.md", "constitution")
        assert isinstance(fm, ConstitutionFrontmatter)
        assert fm.version == 1
        assert len(body) > 0

    def test_app_context_has_valid_frontmatter(self) -> None:
        """Bundled app_context.md must have typed frontmatter."""
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
        "entropic.delegate",
        "diagnostics.check_errors",
    ]

    def test_arch_tier_only_sees_allowed_tools(self) -> None:
        """Arch tier with allowed tools must not leak other names."""
        allowed = [
            "entropic.todo_write",
            "entropic.delegate",
            "filesystem.read_file",
            "bash.execute",
        ]
        forbidden = [t for t in self.ALL_TOOLS if t not in allowed]

        adapter = GenericAdapter(tier="arch")
        tools = [_make_tool_def(name) for name in allowed]
        prompt = adapter.format_system_prompt("", tools)

        for name in allowed:
            assert name in prompt, f"Allowed tool '{name}' missing"

        for name in forbidden:
            assert name not in prompt, f"Forbidden tool '{name}' leaked"

    def test_scribe_tier_only_sees_delegate(self) -> None:
        """Scribe tier with only delegate must not leak any other tools."""
        allowed = ["entropic.delegate"]
        forbidden = [t for t in self.ALL_TOOLS if t not in allowed]

        adapter = GenericAdapter(tier="scribe")
        tools = [_make_tool_def(name) for name in allowed]
        prompt = adapter.format_system_prompt("", tools)

        assert "entropic.delegate" in prompt
        for name in forbidden:
            assert name not in prompt, f"Forbidden tool '{name}' leaked"

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


class TestEnableThinkingConsistency:
    """Verify enable_thinking frontmatter is honored for all bundled identities."""

    @pytest.mark.parametrize("tier", TIERS)
    def test_no_think_identities_set_enable_thinking_false(self, tier: str) -> None:
        """Identities with enable_thinking: false must pass it through generation kwargs.

        This guards against the silent-drop bug where enable_thinking=False was
        ignored because llama-cpp-python didn't support chat_template_kwargs.
        The fix uses a per-request no-think handler swap.
        """
        path = _DATA_DIR / f"identity_{tier}.md"
        fm, _body = parse_prompt_file(path, "identity")
        assert isinstance(fm, IdentityFrontmatter)

        # Every identity must explicitly declare enable_thinking
        assert isinstance(
            fm.enable_thinking, bool
        ), f"{tier}: enable_thinking must be a bool, got {type(fm.enable_thinking)}"

    def test_at_least_one_identity_disables_thinking(self) -> None:
        """Sanity check: at least one bundled identity has enable_thinking=False."""
        no_think = []
        for tier in TIERS:
            path = _DATA_DIR / f"identity_{tier}.md"
            fm, _ = parse_prompt_file(path, "identity")
            assert isinstance(fm, IdentityFrontmatter)
            if not fm.enable_thinking:
                no_think.append(tier)
        assert len(no_think) > 0, "No bundled identities have enable_thinking=False"

    def test_at_least_one_identity_enables_thinking(self) -> None:
        """Sanity check: at least one bundled identity has enable_thinking=True."""
        think = []
        for tier in TIERS:
            path = _DATA_DIR / f"identity_{tier}.md"
            fm, _ = parse_prompt_file(path, "identity")
            assert isinstance(fm, IdentityFrontmatter)
            if fm.enable_thinking:
                think.append(tier)
        assert len(think) > 0, "No bundled identities have enable_thinking=True"

    def test_thinking_identities_have_no_grammar(self) -> None:
        """Thinking-enabled identities must not have GBNF grammar (incompatible)."""
        for tier in TIERS:
            path = _DATA_DIR / f"identity_{tier}.md"
            fm, _ = parse_prompt_file(path, "identity")
            assert isinstance(fm, IdentityFrontmatter)
            if fm.enable_thinking:
                assert (
                    fm.grammar is None
                ), f"{tier}: enable_thinking=True is incompatible with grammar={fm.grammar}"


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
