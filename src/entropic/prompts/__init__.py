"""Prompt templates for Entropic."""

from __future__ import annotations

from pathlib import Path
from typing import TYPE_CHECKING, Literal

import yaml
from pydantic import BaseModel, Field

from entropic.core.logging import get_logger

if TYPE_CHECKING:
    from entropic.core.base import ModelTier

logger = get_logger("prompts")

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
    """Frontmatter for tier identity prompt files."""

    type: Literal["identity"] = "identity"
    name: str
    focus: list[str] = Field(min_length=1)
    examples: list[str] = []


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


def load_prompt(
    name: str,
    prompts_dir: Path | None = None,
    use_bundled: bool = True,
) -> str:
    """
    Load a prompt template by name.

    Checks in order:
    1. User's prompts_dir (if provided and file exists)
    2. Bundled defaults in package data (if use_bundled=True)

    Args:
        name: Prompt name (e.g., "constitution" loads "constitution.md")
        prompts_dir: Optional user prompts directory
        use_bundled: If False, skip bundled fallback (raise if not in prompts_dir)

    Returns:
        Prompt content

    Raises:
        FileNotFoundError: If prompt not found in any location
    """
    filename = f"{name}.md"

    # Check user prompts dir first
    if prompts_dir:
        user_path = prompts_dir / filename
        if user_path.exists():
            logger.debug(f"Loading prompt '{name}' from {user_path}")
            return user_path.read_text()
        # prompts_dir set + bundled disabled = strict mode (consumer owns all prompts)
        if not use_bundled:
            raise FileNotFoundError(
                f"Prompt '{name}' not found in {prompts_dir}. Expected: {user_path}"
            )

    # Fall back to bundled defaults
    if use_bundled:
        default_path = _DATA_DIR / filename
        if default_path.exists():
            logger.debug(f"Loading prompt '{name}' from bundled defaults")
            return default_path.read_text()

    raise FileNotFoundError(f"Prompt '{name}' not found in {_DATA_DIR}")


def get_constitution_prompt(
    prompts_dir: Path | None = None,
    use_bundled: bool = True,
) -> str:
    """Get the constitution prompt (safety guardrails).

    Consumers can replace the default constitution by placing a
    ``constitution.md`` in their ``prompts_dir``. If none is found
    and ``use_bundled=True``, falls back to the bundled default.

    A constitution should always exist — set ``use_bundled=False``
    only when providing your own via ``prompts_dir``.
    """
    return load_prompt("constitution", prompts_dir, use_bundled=use_bundled)


def get_tier_identity_prompt(
    tier: str,
    prompts_dir: Path | None = None,
    use_bundled: bool = True,
) -> str:
    """Get the identity prompt for a specific model tier.

    Returns the full file content (frontmatter + body). For just the body
    (markdown after frontmatter), use load_tier_identity() instead.
    """
    return load_prompt(f"identity_{tier}", prompts_dir, use_bundled=use_bundled)


def get_identity_prompt(
    tier: str,
    prompts_dir: Path | None = None,
    use_bundled: bool = True,
) -> str:
    """Get the full identity prompt: constitution + tier-specific body.

    Loads the identity file, strips YAML frontmatter, and combines
    constitution + markdown body for use as the adapter's system prompt.

    When ``prompts_dir`` is set, the identity file MUST exist there —
    no silent fallback to bundled defaults. This surfaces typos and
    missing files immediately instead of silently substituting.

    Raises:
        FileNotFoundError: If prompts_dir is set but identity file missing,
            or if use_bundled=False and no identity file found.
    """
    constitution = get_constitution_prompt(prompts_dir, use_bundled=use_bundled)

    # Try to load with frontmatter parsing first
    identity_path = _resolve_identity_path(tier, prompts_dir, use_bundled=use_bundled)
    if identity_path:
        _identity, body = load_tier_identity(identity_path)
        return f"{constitution}\n\n{body}"

    # Strict mode: prompts_dir set + bundled disabled = error on missing
    if prompts_dir and not use_bundled:
        raise FileNotFoundError(
            f"No identity file for tier '{tier}' in {prompts_dir}. "
            f"Expected: {prompts_dir / f'identity_{tier}.md'}"
        )

    # Try bundled raw file (no frontmatter), or fall back to constitution only
    try:
        tier_identity = get_tier_identity_prompt(tier, prompts_dir, use_bundled=use_bundled)
        return f"{constitution}\n\n{tier_identity}"
    except FileNotFoundError:
        logger.warning(
            "No identity file for tier '%s' — using constitution only. "
            "Create identity_%s.md or set use_bundled_prompts=False to enforce.",
            tier,
            tier,
        )
        return constitution


def _resolve_identity_path(
    tier: str,
    prompts_dir: Path | None = None,
    use_bundled: bool = True,
) -> Path | None:
    """Find the identity file path for a tier, checking user dir then bundled.

    When ``prompts_dir`` is set AND ``use_bundled`` is False, only looks
    in prompts_dir — no bundled fallback. Otherwise, checks prompts_dir
    first, then falls back to bundled.
    """
    filename = f"identity_{tier}.md"

    # Check user prompts dir first
    if prompts_dir:
        user_path = prompts_dir / filename
        if user_path.exists():
            return user_path
        if not use_bundled:
            return None  # Strict mode — no fallback

    # Bundled fallback
    default_path = _DATA_DIR / filename
    return default_path if (use_bundled and default_path.exists()) else None


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


def build_classification_grammar(num_tiers: int) -> str:
    """Generate GBNF grammar constraining output to valid tier digits.

    Args:
        num_tiers: Number of tiers (generates digits 1..N)

    Returns:
        GBNF grammar string
    """
    digits = " | ".join(f'"{i}"' for i in range(1, num_tiers + 1))
    return f'root ::= ({digits}) "\\n"'
