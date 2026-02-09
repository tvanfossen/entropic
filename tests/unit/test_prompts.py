"""Tests for prompt loading and classification prompt building."""

import pytest
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
