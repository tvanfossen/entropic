"""
Test model routing classification with real model inference.

Tests validate that the router model correctly classifies user prompts
into appropriate model tiers. Tier names are discovered dynamically from
the bundled identity library.

Classification tests use ``_classify_task()`` directly — router model only,
no model swap triggered. A separate test validates cross-model swap lifecycle.

Run with: pytest tests/model/test_routing.py -v
Skip with: pytest -m "not model"
"""

import random
from pathlib import Path

import entropic
import pytest
import yaml
from entropic.config.loader import ConfigLoader
from entropic.core.base import Message
from entropic.inference.orchestrator import ModelOrchestrator

# ---------------------------------------------------------------------------
# Identity discovery — avoids hardcoding tier names
# ---------------------------------------------------------------------------
_PROMPTS_DIR = Path(entropic.__file__).parent / "data" / "prompts"


def _routable_identities() -> list[tuple[str, list[str], list[str]]]:
    """Return (name, focus, examples) for all routable identities.

    Parses YAML frontmatter directly from bundled identity files.
    Only includes identities where routable=True.
    """
    result = []
    for path in sorted(_PROMPTS_DIR.glob("identity_*.md")):
        name = path.stem.removeprefix("identity_")
        text = path.read_text()
        if not text.startswith("---"):
            continue
        end = text.index("---", 3)
        fm = yaml.safe_load(text[3:end])
        if fm.get("routable", True):
            result.append((name, fm.get("focus", []), fm.get("examples", [])))
    return result


_ROUTABLE = _routable_identities()
_IDENTITY_NAMES = [name for name, _, _ in _ROUTABLE]


def _tiers_on_non_default_model() -> list[str]:
    """Return routable tier names that use a different model file than the default."""
    config = ConfigLoader().load()
    default_name = config.models.default
    default_config = config.models.tiers.get(default_name)
    if not default_config:
        return []
    default_path = str(default_config.path)
    result = []
    for name in _IDENTITY_NAMES:
        tier_config = config.models.tiers.get(name)
        if tier_config and str(tier_config.path) != default_path:
            result.append(name)
    return result


_NON_DEFAULT_MODEL_TIERS = _tiers_on_non_default_model()


# ---------------------------------------------------------------------------
# Parametrized routing tests — one per routable identity
# ---------------------------------------------------------------------------


@pytest.mark.model
class TestRoutingClassification:
    """Test that example prompts classify to their source identity.

    Dynamically parametrized from the bundled identity library.
    Each test picks a random example from the identity's frontmatter
    and verifies the router classifies it to the correct tier.

    Uses _classify_task() directly — router model only, no model swap.
    """

    @pytest.mark.asyncio
    @pytest.mark.parametrize("identity_name", _IDENTITY_NAMES)
    async def test_example_classifies_to_own_tier(
        self, orchestrator: ModelOrchestrator, identity_name: str
    ):
        """An example prompt from an identity should classify to that tier."""
        examples = next(e for n, _, e in _ROUTABLE if n == identity_name)

        prompt = random.choice(examples)
        tier, raw = await orchestrator._classify_task([Message(role="user", content=prompt)])
        assert tier == identity_name, (
            f"Example '{prompt}' expected to classify as {identity_name}, "
            f"got {tier} (raw: {raw!r})"
        )


@pytest.mark.model
class TestClassificationSpeed:
    """Test that classification is fast (using small router model).

    Uses the bundled router config (logits_all=False) — same as real engine.
    """

    @pytest.mark.asyncio
    async def test_classification_under_500ms(self, orchestrator: ModelOrchestrator):
        """Steady-state classification should complete in under 500ms.

        A warm-up classification runs first to eliminate cold-cache effects.
        The timed run measures pure routing speed.
        """
        import time

        _, _, examples = random.choice(_ROUTABLE)
        prompt = random.choice(examples)
        messages = [Message(role="user", content=prompt)]

        # Warm-up
        await orchestrator._classify_task(messages)

        start = time.perf_counter()
        await orchestrator._classify_task(messages)
        elapsed_ms = (time.perf_counter() - start) * 1000

        assert elapsed_ms < 500, f"Classification took {elapsed_ms:.0f}ms, expected <500ms"

    @pytest.mark.asyncio
    async def test_multiple_classifications_consistent(self, orchestrator: ModelOrchestrator):
        """Same prompt should classify consistently (deterministic with temp=0)."""
        _, _, examples = random.choice(_ROUTABLE)
        prompt = random.choice(examples)

        messages = [Message(role="user", content=prompt)]
        results = []
        for _ in range(3):
            tier, _ = await orchestrator._classify_task(messages)
            results.append(tier)

        assert all(r == results[0] for r in results), f"Inconsistent results: {results}"


@pytest.mark.model
class TestCrossModelSwap:
    """Test that routing to a tier on a different model file works.

    Validates the full route() → deactivate → load lifecycle for cross-model
    transitions. Randomly selects a non-default-model tier to test.
    """

    @pytest.mark.asyncio
    async def test_route_to_non_default_model_tier(self, orchestrator: ModelOrchestrator):
        """Routing to a tier on a different model file completes without error."""
        if not _NON_DEFAULT_MODEL_TIERS:
            pytest.skip("No non-default-model routable tiers available")

        target_name = random.choice(_NON_DEFAULT_MODEL_TIERS)
        target_tier = orchestrator._find_tier(target_name)
        assert target_tier is not None, f"Tier '{target_name}' not found in orchestrator"

        # Force route to specific tier (bypasses classification)
        model = await orchestrator._get_model(target_tier)

        assert model.is_loaded, f"Model for {target_name} not loaded after swap"
