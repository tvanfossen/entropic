"""Tests for prompt loading and classification prompt building."""

import tempfile
from pathlib import Path

import pytest
from entropi.inference.adapters.base import GenericAdapter
from entropi.prompts import (
    _resolve_identity_path,
    get_tier_identity_prompt,
    load_tier_identity,
)

TIERS = ["simple", "code", "normal", "thinking"]


class TestLoadTierIdentity:
    """Tests for load_tier_identity with YAML frontmatter."""

    def test_parses_frontmatter_and_body(self) -> None:
        content = "---\nname: test\nfocus:\n  - item one\n  - item two\n---\n\n# Body\n"
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
        from pydantic import ValidationError

        content = "---\nname: test\nfocus: []\n---\n\n# Body\n"
        with tempfile.NamedTemporaryFile(mode="w", suffix=".md", delete=False) as f:
            f.write(content)
            f.flush()
            with pytest.raises(ValidationError):
                load_tier_identity(Path(f.name))

    def test_examples_optional(self) -> None:
        content = "---\nname: test\nfocus:\n  - item\n---\n\n# Body\n"
        with tempfile.NamedTemporaryFile(mode="w", suffix=".md", delete=False) as f:
            f.write(content)
            f.flush()
            identity, _body = load_tier_identity(Path(f.name))
        assert identity.examples == []


class TestIdentityFilesHaveFrontmatter:
    """Validate all bundled identity files have valid frontmatter."""

    @pytest.mark.parametrize("tier", TIERS)
    def test_identity_has_valid_frontmatter(self, tier: str) -> None:
        """Each tier identity file MUST have valid YAML frontmatter with focus."""
        path = _resolve_identity_path(tier)
        assert path is not None, f"identity_{tier}.md not found"
        identity, body = load_tier_identity(path)
        assert len(identity.focus) > 0, f"identity_{tier}.md has empty focus"
        assert len(body) > 0, f"identity_{tier}.md has empty body"

    @pytest.mark.parametrize("tier", TIERS)
    def test_identity_name_matches_tier(self, tier: str) -> None:
        """Frontmatter name must match the tier filename."""
        path = _resolve_identity_path(tier)
        assert path is not None
        identity, _body = load_tier_identity(path)
        assert identity.name == tier


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
        "entropi.todo_write",
        "entropi.handoff",
        "diagnostics.check_errors",
    ]

    def test_thinking_tier_only_sees_allowed_tools(self) -> None:
        """Thinking tier with allowed tools must not leak other names."""
        allowed = ["entropi.todo_write", "entropi.handoff", "filesystem.read_file", "bash.execute"]
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
        allowed = ["entropi.handoff"]
        forbidden = [t for t in self.ALL_TOOLS if t not in allowed]

        adapter = GenericAdapter(tier="simple")
        tools = [_make_tool_def(name) for name in allowed]
        prompt = adapter.format_system_prompt("", tools)

        assert "entropi.handoff" in prompt
        for name in forbidden:
            assert name not in prompt, f"Forbidden tool '{name}' leaked"

    def test_thinking_identity_has_first_action_section(self) -> None:
        """Thinking tier must instruct model to create todos before tool use."""
        identity = get_tier_identity_prompt("thinking")
        assert "## First Action" in identity
        assert "entropi.todo_write" in identity

    def test_thinking_identity_has_no_unauthorized_tools(self) -> None:
        """Thinking tier identity must not mention tools outside its set."""
        thinking_allowed = {
            "entropi.todo_write",
            "entropi.handoff",
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
