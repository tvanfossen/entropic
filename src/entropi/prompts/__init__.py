"""Prompt templates for Entropi."""

from __future__ import annotations

from pathlib import Path
from typing import TYPE_CHECKING

import yaml
from pydantic import BaseModel, Field

from entropi.core.logging import get_logger

if TYPE_CHECKING:
    from entropi.core.base import ModelTier

logger = get_logger("prompts")

# Package data directory
_DATA_DIR = Path(__file__).parent.parent / "data" / "prompts"


class TierIdentity(BaseModel):
    """Schema for identity file YAML frontmatter."""

    name: str
    focus: list[str] = Field(min_length=1)
    examples: list[str] = []


def load_tier_identity(path: Path) -> tuple[TierIdentity, str]:
    """Load identity file: validate frontmatter, return (metadata, body).

    Returns:
        (TierIdentity, markdown_body) — body is the content after frontmatter,
        used as system prompt identity by the adapter.

    Raises:
        ValueError: Missing or invalid frontmatter.
        ValidationError: Frontmatter fails schema (e.g. missing focus).
    """
    content = path.read_text()
    if not content.startswith("---"):
        raise ValueError(f"Identity file {path} missing YAML frontmatter")
    parts = content.split("---", 2)
    if len(parts) < 3:
        raise ValueError(f"Identity file {path} has malformed frontmatter")
    frontmatter_str = parts[1]
    body = parts[2]
    data = yaml.safe_load(frontmatter_str)
    identity = TierIdentity(**data)
    return identity, body.strip()


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

    # Fall back to bundled defaults
    if use_bundled:
        default_path = _DATA_DIR / filename
        if default_path.exists():
            logger.debug(f"Loading prompt '{name}' from bundled defaults")
            return default_path.read_text()

    raise FileNotFoundError(f"Prompt '{name}' not found in {prompts_dir or _DATA_DIR}")


def get_constitution_prompt(
    prompts_dir: Path | None = None,
    use_bundled: bool = True,
) -> str:
    """Get the constitution prompt (shared principles across all tiers)."""
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
    """
    constitution = get_constitution_prompt(prompts_dir, use_bundled=use_bundled)

    # Try to load with frontmatter parsing first
    identity_path = _resolve_identity_path(tier, prompts_dir, use_bundled=use_bundled)
    if identity_path:
        _identity, body = load_tier_identity(identity_path)
        return f"{constitution}\n\n{body}"

    # Fallback: load raw (no frontmatter)
    tier_identity = get_tier_identity_prompt(tier, prompts_dir, use_bundled=use_bundled)
    return f"{constitution}\n\n{tier_identity}"


def _resolve_identity_path(
    tier: str,
    prompts_dir: Path | None = None,
    use_bundled: bool = True,
) -> Path | None:
    """Find the identity file path for a tier, checking user dir then bundled."""
    filename = f"identity_{tier}.md"

    if prompts_dir:
        user_path = prompts_dir / filename
        if user_path.exists():
            return user_path

    if use_bundled:
        default_path = _DATA_DIR / filename
        if default_path.exists():
            return default_path

    return None


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


# Legacy compatibility — get_classification_prompt with old signature
def get_classification_prompt(
    message: str,
    prompts_dir: Path | None = None,
    history: list[str] | None = None,
) -> str:
    """Build classification prompt from identity file frontmatter.

    Loads each tier's identity file to extract focus points and examples
    via YAML frontmatter, then auto-generates the classification prompt.

    This is the backward-compatible entry point used by the orchestrator.
    New code should use build_classification_prompt() directly.
    """
    from entropi.core.base import ModelTier

    tier_names = ["simple", "code", "normal", "thinking"]
    tiers: list[ModelTier] = []
    for name in tier_names:
        identity_path = _resolve_identity_path(name, prompts_dir)
        if identity_path:
            identity, _body = load_tier_identity(identity_path)
            tiers.append(ModelTier(name, focus=identity.focus, examples=identity.examples))
        else:
            # Minimal fallback
            tiers.append(ModelTier(name, focus=[name]))

    return build_classification_prompt(tiers, message, history)
