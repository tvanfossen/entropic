"""Tests for prompt loading and classification prompt building."""

import pytest
from entropi.inference.adapters.base import GenericAdapter
from entropi.prompts import (
    _extract_focus_points,
    _tool_name_to_guidance_filename,
    get_per_tool_guidance,
    get_tier_identity_prompt,
    get_tool_usage_prompt,
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


class TestToolNameToGuidanceFilename:
    """Tests for _tool_name_to_guidance_filename."""

    def test_strips_server_prefix(self) -> None:
        assert _tool_name_to_guidance_filename("filesystem.read_file") == "read_file"

    def test_no_prefix(self) -> None:
        assert _tool_name_to_guidance_filename("read_file") == "read_file"

    def test_replaces_remaining_dots(self) -> None:
        assert _tool_name_to_guidance_filename("server.some.nested") == "some_nested"

    def test_entropi_prefix(self) -> None:
        assert _tool_name_to_guidance_filename("entropi.todo_write") == "todo_write"

    def test_system_prefix(self) -> None:
        assert _tool_name_to_guidance_filename("system.handoff") == "handoff"


class TestGetPerToolGuidance:
    """Tests for get_per_tool_guidance."""

    def test_loads_existing_guidance(self) -> None:
        result = get_per_tool_guidance(["filesystem.read_file"])
        assert "## Tool Guidance" in result
        assert "filesystem.read_file" in result

    def test_skips_missing_guidance(self) -> None:
        result = get_per_tool_guidance(["nonexistent.fake_tool"])
        assert result == ""

    def test_empty_list_returns_empty(self) -> None:
        result = get_per_tool_guidance([])
        assert result == ""

    def test_multiple_tools(self) -> None:
        tools = ["filesystem.read_file", "filesystem.write_file"]
        result = get_per_tool_guidance(tools)
        assert "filesystem.read_file" in result
        assert "filesystem.write_file" in result


class TestToolUsageHasNoToolNames:
    """tool_usage.md must contain zero specific tool names."""

    # Every dotted tool name that exists in the system
    ALL_TOOL_NAMES = [
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
        "system.handoff",
        "diagnostics.check_errors",
    ]

    def test_no_dotted_tool_names(self) -> None:
        """tool_usage.md must not contain any dotted tool names."""
        prompt = get_tool_usage_prompt()
        for name in self.ALL_TOOL_NAMES:
            assert name not in prompt, f"tool_usage.md contains '{name}'"

    def test_no_bare_tool_names(self) -> None:
        """tool_usage.md must not contain bare tool names like write_file."""
        prompt = get_tool_usage_prompt()
        bare_names = [
            "read_file",
            "write_file",
            "edit_file",
            "bash_execute",
            "todo_write",
            "check_errors",
        ]
        for name in bare_names:
            assert name not in prompt, f"tool_usage.md contains '{name}'"


def _make_tool_def(name: str) -> dict[str, object]:
    """Create a minimal tool definition for testing."""
    return {
        "name": name,
        "description": f"Tool {name}",
        "inputSchema": {"type": "object", "properties": {}},
    }


class TestToolIsolation:
    """Assembled system prompt must contain ZERO unauthorized tool names."""

    # All tools that exist in the system
    ALL_TOOLS = TestToolUsageHasNoToolNames.ALL_TOOL_NAMES

    def test_thinking_tier_only_sees_allowed_tools(self) -> None:
        """Thinking tier with 3 allowed tools must not leak other names."""
        allowed = ["entropi.todo_write", "system.handoff", "filesystem.read_file"]
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
        allowed = ["system.handoff"]
        forbidden = [t for t in self.ALL_TOOLS if t not in allowed]

        adapter = GenericAdapter(tier="simple")
        tools = [_make_tool_def(name) for name in allowed]
        prompt = adapter.format_system_prompt("", tools)

        assert "system.handoff" in prompt
        for name in forbidden:
            assert name not in prompt, f"Forbidden tool '{name}' leaked"

    def test_thinking_identity_has_no_unauthorized_tools(self) -> None:
        """Thinking tier identity must not mention tools outside its set."""
        thinking_allowed = {
            "entropi.todo_write",
            "system.handoff",
            "filesystem.read_file",
        }
        forbidden = [t for t in self.ALL_TOOLS if t not in thinking_allowed]

        identity = get_tier_identity_prompt("thinking")
        for name in forbidden:
            assert name not in identity, f"identity_thinking.md mentions unauthorized tool '{name}'"

    @pytest.mark.parametrize("tier", TIERS)
    def test_per_tool_guidance_only_for_allowed(self, tier: str) -> None:
        """Per-tool guidance must only load for tools in the filtered set."""
        allowed = ["filesystem.read_file"]
        leaked_guidance_names = [
            "filesystem.write_file",
            "filesystem.edit_file",
            "bash.execute",
        ]

        adapter = GenericAdapter(tier=tier)
        tools = [_make_tool_def(name) for name in allowed]
        prompt = adapter.format_system_prompt("", tools)

        for name in leaked_guidance_names:
            assert name not in prompt, f"Guidance for '{name}' leaked into {tier} tier"
