"""Prompt templates for Entropic."""

from __future__ import annotations

from pathlib import Path
from typing import TYPE_CHECKING, Literal

import yaml
from pydantic import BaseModel, Field

if TYPE_CHECKING:
    from entropic.core.base import ModelTier

# Package data directory
_DATA_DIR = Path(__file__).parent.parent / "data" / "prompts"

# Valid prompt types for frontmatter validation
PromptType = Literal["constitution", "app_context", "identity"]


class PromptFrontmatter(BaseModel):
    """Base schema for all prompt file YAML frontmatter."""

    type: PromptType
    version: int = Field(ge=1)


class ConstitutionFrontmatter(PromptFrontmatter):
    """Frontmatter for constitution prompt files."""

    type: Literal["constitution"] = "constitution"


class AppContextFrontmatter(PromptFrontmatter):
    """Frontmatter for app_context prompt files."""

    type: Literal["app_context"] = "app_context"


class IdentityFrontmatter(PromptFrontmatter):
    """Frontmatter for tier identity prompt files.

    Inference behavior params live here (not in ModelConfig).
    Config contains hardware/load-time params only.
    """

    type: Literal["identity"] = "identity"
    name: str
    focus: list[str] = Field(min_length=1)
    examples: list[str] = []
    grammar: str | None = None
    auto_chain: str | None = None
    allowed_tools: list[str] = []
    max_output_tokens: int = 1024
    temperature: float = 0.7
    repeat_penalty: float = 1.1
    enable_thinking: bool = False
    model_preference: str = "primary"
    interstitial: bool = False
    routable: bool = True


# Map type string to schema class
_FRONTMATTER_SCHEMAS: dict[str, type[PromptFrontmatter]] = {
    "constitution": ConstitutionFrontmatter,
    "app_context": AppContextFrontmatter,
    "identity": IdentityFrontmatter,
}


def parse_prompt_file(
    path: Path,
    expected_type: PromptType,
) -> tuple[PromptFrontmatter, str]:
    """Parse a prompt file: validate frontmatter, return (frontmatter, body).

    Args:
        path: Path to the .md prompt file.
        expected_type: Expected frontmatter type field value.

    Returns:
        (frontmatter_model, markdown_body)

    Raises:
        ValueError: Missing, malformed, or type-mismatched frontmatter.
        ValidationError: Frontmatter fails schema validation.
    """
    content = path.read_text()
    if not content.startswith("---"):
        raise ValueError(f"Prompt file {path} missing YAML frontmatter")
    parts = content.split("---", 2)
    if len(parts) < 3:
        raise ValueError(f"Prompt file {path} has malformed frontmatter")

    data = yaml.safe_load(parts[1])
    if not isinstance(data, dict):
        raise ValueError(f"Prompt file {path} frontmatter is not a YAML mapping")

    actual_type = data.get("type")
    if actual_type != expected_type:
        raise ValueError(
            f"Prompt file {path} has type '{actual_type}' " f"but was loaded as '{expected_type}'"
        )

    schema_cls = _FRONTMATTER_SCHEMAS[expected_type]
    frontmatter = schema_cls(**data)
    body = parts[2].strip()
    return frontmatter, body


# Backwards-compatible alias
TierIdentity = IdentityFrontmatter


def load_tier_identity(path: Path) -> tuple[TierIdentity, str]:
    """Load identity file: validate frontmatter, return (metadata, body).

    Returns:
        (IdentityFrontmatter, markdown_body) — body is the content after
        frontmatter, used as system prompt identity by the adapter.

    Raises:
        ValueError: Missing or invalid frontmatter.
        ValidationError: Frontmatter fails schema (e.g. missing focus).
    """
    frontmatter, body = parse_prompt_file(path, "identity")
    assert isinstance(frontmatter, IdentityFrontmatter)
    return frontmatter, body


def build_classification_prompt(
    tiers: list[ModelTier],
    message: str,
    history: list[str] | None = None,
) -> str:
    """Auto-generate classification prompt from tier focus + examples.

    Args:
        tiers: Ordered list of tiers (index+1 = classification digit)
        message: User message to classify
        history: Recent user messages for context

    Returns:
        Classification prompt ending with trailing space for digit output
    """
    # Tier descriptions from focus
    identity_lines = []
    for i, tier in enumerate(tiers, 1):
        focus_str = ", ".join(tier.focus)
        identity_lines.append(f"{i} = {tier.name.upper()}: {focus_str}")
    identities_text = "\n".join(identity_lines)

    # Few-shot examples from tier.examples (frontmatter)
    example_lines = []
    for i, tier in enumerate(tiers, 1):
        for ex in tier.examples:
            example_lines.append(f'"{ex}" -> {i}')
    examples_text = "\n".join(example_lines)

    # History context (compact for small router model)
    history_text = ""
    if history:
        history_text = "Recent messages: " + " | ".join(history[-5:])

    # Assemble prompt
    parts = ["Classify the message. Reply with the number only.", ""]
    parts.append(identities_text)
    parts.append("")
    if history_text:
        parts.append(history_text)
        parts.append("")
    if examples_text:
        parts.append(examples_text)
    parts.append(f'"{message}" -> ')

    return "\n".join(parts)
