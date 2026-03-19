"""
Test model routing classification with real model inference.

Tests validate that the router model correctly classifies user prompts
into appropriate model tiers. Tier names are discovered dynamically from
the bundled identity library.

Uses a test-owned config (routing_config.yaml) with router enabled —
independent of the user's global config.

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
from entropic.config.schema import LibraryConfig
from entropic.core.base import Message
from entropic.core.logging import setup_logging, setup_model_logger
from entropic.inference.orchestrator import ModelOrchestrator

# ---------------------------------------------------------------------------
# Identity discovery — avoids hardcoding tier names
# ---------------------------------------------------------------------------
_PROMPTS_DIR = Path(entropic.__file__).parent / "data" / "prompts"
_CONFIG_PATH = Path(__file__).parent / "routing_config.yaml"


def _classifiable_identities() -> list[tuple[str, list[str], list[str]]]:
    """Return (name, focus, examples) for all identities with examples.

    Parses YAML frontmatter directly from bundled identity files.
    Includes any identity that has non-empty examples — routing is an
    engine capability independent of whether a consumer enables it.
    """
    result = []
    for path in sorted(_PROMPTS_DIR.glob("identity_*.md")):
        name = path.stem.removeprefix("identity_")
        text = path.read_text()
        if not text.startswith("---"):
            continue
        end = text.index("---", 3)
        fm = yaml.safe_load(text[3:end])
        examples = fm.get("examples", [])
        if examples:
            result.append((name, fm.get("focus", []), examples))
    return result


_CLASSIFIABLE = _classifiable_identities()
_IDENTITY_NAMES = [name for name, _, _ in _CLASSIFIABLE]


# ---------------------------------------------------------------------------
# Test-owned config and orchestrator
# ---------------------------------------------------------------------------


@pytest.fixture(scope="module")
def routing_config() -> LibraryConfig:
    """Load the test-owned routing config. Skips if models unavailable."""
    raw = yaml.safe_load(_CONFIG_PATH.read_text())
    config = LibraryConfig(**raw)

    # Verify router model exists
    if config.models.router is None:
        pytest.skip("routing_config.yaml has no router configured")
    if not config.models.router.path.expanduser().exists():
        pytest.skip(f"Router model not available: {config.models.router.path}")

    # Verify at least one tier model exists
    for name, tier in config.models.tiers.items():
        if not tier.path.expanduser().exists():
            pytest.skip(f"Model not available: {tier.path} (tier: {name})")

    return config


@pytest.fixture(scope="module")
async def routing_orchestrator(routing_config, tmp_path_factory):
    """Module-scoped orchestrator with routing enabled. Router loaded once."""
    log_dir = tmp_path_factory.mktemp("routing_logs")
    setup_logging(routing_config, project_dir=log_dir, app_dir_name=".")
    setup_model_logger(project_dir=log_dir, app_dir_name=".")

    orch = ModelOrchestrator(routing_config)
    await orch.initialize()

    yield orch

    await orch.shutdown()


@pytest.fixture(autouse=True)
def _redirect_routing_logs(routing_config, test_log_dir):
    """Redirect engine logging to per-test log dir before each test.

    The orchestrator is module-scoped (model loaded once), but logging
    is redirected per-test so each test's session.log is captured
    independently in test-reports/logs/<test_name>/.
    """
    setup_logging(routing_config, project_dir=test_log_dir, app_dir_name=".")
    setup_model_logger(project_dir=test_log_dir, app_dir_name=".")


# ---------------------------------------------------------------------------
# Parametrized routing tests — one per classifiable identity
# ---------------------------------------------------------------------------


@pytest.mark.model
class TestRoutingClassification:
    """Test that example prompts classify to their source identity.

    Dynamically parametrized from the bundled identity library.
    Each test classifies ALL examples from the identity's frontmatter
    and passes if a majority classify correctly. This accounts for
    inherent variance in small router models (0.6B).

    Uses _classify_task() directly — router model only, no model swap.
    """

    @pytest.mark.asyncio
    @pytest.mark.parametrize("identity_name", _IDENTITY_NAMES)
    async def test_example_classifies_to_own_tier(
        self, routing_orchestrator: ModelOrchestrator, identity_name: str
    ):
        """A majority of example prompts should classify to their tier."""
        examples = next(e for n, _, e in _CLASSIFIABLE if n == identity_name)

        correct = 0
        misses: list[str] = []
        for prompt in examples:
            tier, raw = await routing_orchestrator._classify_task(
                [Message(role="user", content=prompt)]
            )
            if tier == identity_name:
                correct += 1
            else:
                misses.append(f"  '{prompt}' → {tier} (raw: {raw!r})")

        threshold = len(examples) / 2
        assert correct > threshold, (
            f"{identity_name}: {correct}/{len(examples)} correct "
            f"(need >{threshold:.0f}).\nMisclassified:\n" + "\n".join(misses)
        )


# ---------------------------------------------------------------------------
# Novel prompt tests — messages NOT in any identity's examples
# ---------------------------------------------------------------------------

# (prompt, acceptable_tiers) — must NOT appear in any identity's examples.
# Ambiguous prompts list multiple acceptable tiers.
_NOVEL_PROMPTS = [
    ("Tell me about Python decorators", {"lead", "analyst"}),
    ("Create a REST API endpoint for user registration", {"eng"}),
    ("Should we split this into separate services or keep it monolithic?", {"arch"}),
    ("Why is my test segfaulting?", {"qa", "eng"}),
    ("Hey", {"lead"}),
    ("Thanks, that's all", {"lead"}),
    ("Where is the config parser defined?", {"eng", "analyst", "arch"}),
    ("What are the tradeoffs of microservices?", {"arch", "lead"}),
]


@pytest.mark.model
class TestNovelClassification:
    """Test routing with prompts NOT in the few-shot examples.

    This catches recency bias and overfitting to examples — the router
    must generalize to unseen messages, not just pattern-match.
    """

    @pytest.mark.asyncio
    @pytest.mark.parametrize(
        "prompt,acceptable",
        _NOVEL_PROMPTS,
        ids=[p[0][:40] for p in _NOVEL_PROMPTS],
    )
    async def test_novel_prompt_classifies_correctly(
        self, routing_orchestrator: ModelOrchestrator, prompt: str, acceptable: set[str]
    ):
        """A novel prompt should classify to one of the acceptable tiers."""
        tier, raw = await routing_orchestrator._classify_task(
            [Message(role="user", content=prompt)]
        )
        tier_name = tier.name if hasattr(tier, "name") else str(tier)
        assert tier_name in acceptable, (
            f"Novel prompt '{prompt}' expected one of {acceptable}, "
            f"got {tier_name} (raw: {raw!r})"
        )


@pytest.mark.model
class TestClassificationSpeed:
    """Test that classification is fast (using small router model).

    Uses the bundled router config (logits_all=False) — same as real engine.
    """

    @pytest.mark.asyncio
    async def test_classification_under_500ms(self, routing_orchestrator: ModelOrchestrator):
        """Steady-state classification should complete in under 500ms.

        A warm-up classification runs first to eliminate cold-cache effects.
        The timed run measures pure routing speed.
        """
        import time

        _, _, examples = random.choice(_CLASSIFIABLE)
        prompt = random.choice(examples)
        messages = [Message(role="user", content=prompt)]

        # Warm-up
        await routing_orchestrator._classify_task(messages)

        start = time.perf_counter()
        await routing_orchestrator._classify_task(messages)
        elapsed_ms = (time.perf_counter() - start) * 1000

        assert elapsed_ms < 500, f"Classification took {elapsed_ms:.0f}ms, expected <500ms"

    @pytest.mark.asyncio
    async def test_multiple_classifications_consistent(
        self, routing_orchestrator: ModelOrchestrator
    ):
        """Same prompt should classify consistently (deterministic with temp=0)."""
        _, _, examples = random.choice(_CLASSIFIABLE)
        prompt = random.choice(examples)

        messages = [Message(role="user", content=prompt)]
        results = []
        for _ in range(3):
            tier, _ = await routing_orchestrator._classify_task(messages)
            results.append(tier)

        assert all(r == results[0] for r in results), f"Inconsistent results: {results}"


@pytest.mark.model
class TestRoutingLifecycle:
    """Test route → get_model lifecycle for non-default tiers.

    Validates that routing to a non-default tier and obtaining its model
    completes without error, regardless of whether tiers share a backend.
    """

    @pytest.mark.asyncio
    async def test_route_to_non_default_tier(
        self, routing_config: LibraryConfig, routing_orchestrator: ModelOrchestrator
    ):
        """Routing to a non-default tier and getting its model succeeds."""
        default_name = routing_config.models.default
        non_default = [n for n in _IDENTITY_NAMES if n != default_name]
        assert non_default, "Config must have at least one non-default tier"

        target_name = random.choice(non_default)
        target_tier = routing_orchestrator._find_tier(target_name)
        assert target_tier is not None, f"Tier '{target_name}' not found in orchestrator"

        model = await routing_orchestrator._get_model(target_tier)
        assert model.is_loaded, f"Model for {target_name} not loaded after route"
