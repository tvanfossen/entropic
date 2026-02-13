"""Tests for prompt loading and classification prompt building."""

import pytest
from entropi.inference.adapters.base import GenericAdapter
from entropi.prompts import (
    _extract_focus_points,
    get_tier_identity_prompt,
)

TIERS = ["simple", "code", "normal", "thinking"]


class TestExtractFocusPoints:
    """Tests for _extract_focus_points."""

    def test_extracts_bullet_points(self) -> None:
        content = "# Tier\n\n## Focus\n\n- Item one\n- Item two\n"
        result = _extract_focus_points(content, "test")
        assert result == "item one, item two"

    def test_stops_at_next_section(self) -> None:
        content = "# Tier\n\n## Focus\n\n- Only this\n\n## Other\n\n- Not this\n"
        result = _extract_focus_points(content, "test")
        assert result == "only this"
        assert "not this" not in result

    def test_raises_when_focus_missing(self) -> None:
        content = "# Tier\n\n## Other Section\n\n- Stuff\n"
        with pytest.raises(ValueError, match="Missing '## Focus'"):
            _extract_focus_points(content, "broken")

    def test_raises_when_focus_empty(self) -> None:
        content = "# Tier\n\n## Focus\n\n## Next Section\n"
        with pytest.raises(ValueError, match="Missing '## Focus'"):
            _extract_focus_points(content, "empty")

    def test_includes_tier_name_in_error(self) -> None:
        content = "# No focus here\n"
        with pytest.raises(ValueError, match="identity_mytier.md"):
            _extract_focus_points(content, "mytier")


class TestIdentityFilesHaveFocus:
    """Validate all identity files have required ## Focus section."""

    @pytest.mark.parametrize("tier", TIERS)
    def test_identity_has_focus_section(self, tier: str) -> None:
        """Each tier identity file MUST have a ## Focus section.

        This is required for router classification. Without it,
        the router cannot distinguish between tiers.
        """
        identity = get_tier_identity_prompt(tier)
        # Should not raise — if it does, the identity file is broken
        focus = _extract_focus_points(identity, tier)
        assert len(focus) > 0, f"identity_{tier}.md has empty Focus section"


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
