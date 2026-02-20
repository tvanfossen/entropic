"""Central prompt loading, validation, and caching.

PromptManager is the single authority for all prompt files. It loads
constitution, app_context, and per-tier identity prompts from either
bundled defaults (shipped with entropic-engine) or custom paths
specified in config.

Config semantics:
    constitution / identity:
        absent/None  → bundled default
        False        → disabled (nothing injected)
        Path         → custom file (must exist, validated)
    app_context:
        absent/None  → disabled (nothing injected)
        False        → disabled
        Path         → custom file (must exist, validated)
"""

from __future__ import annotations

from pathlib import Path
from typing import Any, Literal

from entropic.core.logging import get_logger
from entropic.prompts import (
    IdentityFrontmatter,
    PromptFrontmatter,
    parse_prompt_file,
)

logger = get_logger("prompts.manager")

# Bundled prompts ship inside the installed package
_BUNDLED_DIR = Path(__file__).parent.parent / "data" / "prompts"


class _CachedPrompt:
    """A loaded and validated prompt file."""

    __slots__ = ("frontmatter", "body", "source")

    def __init__(self, frontmatter: PromptFrontmatter, body: str, source: str) -> None:
        self.frontmatter = frontmatter
        self.body = body
        self.source = source  # "bundled" or a user-facing path string


class PromptManager:
    """Central prompt loading, validation, and caching.

    Created once at startup. Loads all prompts eagerly, validates
    frontmatter, caches bodies, and prints a summary to the terminal.

    Args:
        constitution: Path to custom file, False to disable, None for bundled.
        app_context: Path to custom file, False to disable, None = disabled.
        tier_identities: Dict mapping tier name → (Path | False | None).
            None = bundled default, False = disabled, Path = custom.
        quiet: Suppress terminal output (for tests).
    """

    def __init__(
        self,
        *,
        constitution: Path | Literal[False] | None = None,
        app_context: Path | Literal[False] | None = None,
        tier_identities: dict[str, Path | Literal[False] | None] | None = None,
        quiet: bool = False,
    ) -> None:
        self._constitution: _CachedPrompt | None = None
        self._app_context: _CachedPrompt | None = None
        self._identities: dict[str, _CachedPrompt] = {}
        self._quiet = quiet

        self._load_constitution(constitution)
        self._load_app_context(app_context)
        if tier_identities:
            for tier_name, spec in tier_identities.items():
                self._load_identity(tier_name, spec)

        if not quiet:
            self._print_summary()

    # -- Loading helpers --------------------------------------------------

    def _load_constitution(self, spec: Path | Literal[False] | None) -> None:
        if spec is False:
            logger.info("Constitution disabled by config")
            return
        if spec is None:
            self._constitution = self._load_bundled("constitution.md", "constitution")
            return
        self._constitution = self._load_custom(Path(spec), "constitution")

    def _load_app_context(self, spec: Path | Literal[False] | None) -> None:
        if spec is None or spec is False:
            logger.info("App context disabled (not configured)")
            return
        self._app_context = self._load_custom(Path(spec), "app_context")

    def _load_identity(self, tier_name: str, spec: Path | Literal[False] | None) -> None:
        if spec is False:
            logger.info("Identity for tier '%s' disabled by config", tier_name)
            return
        if spec is None:
            bundled_path = _BUNDLED_DIR / f"identity_{tier_name}.md"
            if bundled_path.exists():
                self._identities[tier_name] = self._load_file(bundled_path, "identity", "bundled")
            else:
                logger.warning(
                    "No bundled identity for tier '%s' — tier will have no identity",
                    tier_name,
                )
            return
        self._identities[tier_name] = self._load_custom(Path(spec), "identity")

    def _load_bundled(self, filename: str, expected_type: str) -> _CachedPrompt:
        path = _BUNDLED_DIR / filename
        if not path.exists():
            raise FileNotFoundError(f"Bundled prompt {filename} not found at {path}")
        return self._load_file(path, expected_type, "bundled")

    def _load_custom(self, path: Path, expected_type: str) -> _CachedPrompt:
        resolved = path.expanduser().resolve()
        if not resolved.exists():
            raise FileNotFoundError(f"Prompt file not found: {resolved}")
        return self._load_file(resolved, expected_type, str(path))

    @staticmethod
    def _load_file(path: Path, expected_type: str, source: str) -> _CachedPrompt:
        fm, body = parse_prompt_file(path, expected_type)  # type: ignore[arg-type]
        return _CachedPrompt(fm, body, source)

    # -- Public API -------------------------------------------------------

    @property
    def has_constitution(self) -> bool:
        return self._constitution is not None

    @property
    def has_app_context(self) -> bool:
        return self._app_context is not None

    def get_constitution(self) -> str | None:
        """Return constitution body, or None if disabled."""
        return self._constitution.body if self._constitution else None

    def get_app_context(self) -> str | None:
        """Return app_context body, or None if disabled/unconfigured."""
        return self._app_context.body if self._app_context else None

    def get_identity(self, tier: str) -> str | None:
        """Return identity body for a tier, or None if disabled/missing."""
        cached = self._identities.get(tier)
        return cached.body if cached else None

    def get_identity_frontmatter(self, tier: str) -> IdentityFrontmatter | None:
        """Return identity frontmatter for a tier (focus, examples, etc.)."""
        cached = self._identities.get(tier)
        if cached and isinstance(cached.frontmatter, IdentityFrontmatter):
            return cached.frontmatter
        return None

    def get_assembled_prompt(self, tier: str) -> str:
        """Assemble the full base prompt for a tier.

        Assembly order:
            1. Constitution (if enabled)
            2. Tier identity (if enabled)
            3. App context (if configured)

        Returns:
            Assembled prompt string. May be empty if everything is disabled.
        """
        parts: list[str] = []
        if self._constitution:
            parts.append(self._constitution.body)
        identity = self._identities.get(tier)
        if identity:
            parts.append(identity.body)
        if self._app_context:
            parts.append(self._app_context.body)
        return "\n\n".join(parts)

    # -- Terminal output --------------------------------------------------

    def _print_summary(self) -> None:
        """Print prompt loading summary to terminal."""
        lines = ["Prompts:"]
        lines.append(f"  constitution   {self._source_label(self._constitution)}")
        lines.append(
            f"  app_context    {self._source_label(self._app_context, default='disabled')}"
        )
        identity_parts = []
        for name, cached in sorted(self._identities.items()):
            identity_parts.append(f"{name}: {self._source_label(cached)}")
        if identity_parts:
            lines.append(f"  identity       {' | '.join(identity_parts)}")
        else:
            lines.append("  identity       (none)")
        print("\n".join(lines))  # noqa: T201

    @staticmethod
    def _source_label(cached: _CachedPrompt | None, default: str = "disabled") -> str:
        if cached is None:
            return default
        version = cached.frontmatter.version
        if cached.source == "bundled":
            return f"bundled (v{version})"
        return f"{cached.source} (v{version})"

    # -- Factory ----------------------------------------------------------

    @classmethod
    def from_config(cls, config: Any, *, quiet: bool = False) -> PromptManager:
        """Create PromptManager from an EntropyConfig or LibraryConfig.

        Reads constitution, app_context from top-level config fields.
        Reads identity from each tier's config entry.
        Falls back to legacy prompts_dir/use_bundled_prompts if new
        fields are absent (backward compatibility during migration).
        """
        constitution = getattr(config, "constitution", None)
        app_context_val = getattr(config, "app_context", None)

        tier_identities: dict[str, Path | Literal[False] | None] = {}
        for tier_name, tier_config in config.models.tiers.items():
            tier_identities[tier_name] = getattr(tier_config, "identity", None)

        return cls(
            constitution=constitution,
            app_context=app_context_val,
            tier_identities=tier_identities,
            quiet=quiet,
        )
