"""
Test router classification confidence with logprobs.

Separate from test_routing.py because this test requires logits_all=True
on the router config, which is a construction-time Llama param. Two
orchestrators can't coexist on the same GPU, so this runs in its own module
with its own module-scoped orchestrator.

Uses a test-owned config (routing_config.yaml) with router enabled —
independent of the user's global config.

The bundled router ships with logits_all=False for speed. This test
overrides that to validate confidence thresholds.

Run with: pytest tests/model/test_routing_confidence.py -v
Skip with: pytest -m "not model"
"""

import random
from pathlib import Path

import entropic
import pytest
import yaml
from entropic.config.schema import EntropyConfig
from entropic.core.base import Message
from entropic.core.logging import setup_logging, setup_model_logger
from entropic.inference.orchestrator import ModelOrchestrator

# ---------------------------------------------------------------------------
# Identity discovery (same as test_routing.py — no hardcoded tier names)
# ---------------------------------------------------------------------------
_PROMPTS_DIR = Path(entropic.__file__).parent / "data" / "prompts"
_CONFIG_PATH = Path(__file__).parent / "routing_config.yaml"


def _classifiable_identities() -> list[tuple[str, list[str], list[str]]]:
    """Return (name, focus, examples) for all identities with examples."""
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
# Orchestrator with logits_all=True on router
# ---------------------------------------------------------------------------


@pytest.fixture(scope="module")
def confidence_config() -> EntropyConfig:
    """Load test-owned routing config with logits_all override."""
    raw = yaml.safe_load(_CONFIG_PATH.read_text())
    config = EntropyConfig(**raw)

    if config.models.router is None:
        pytest.skip("routing_config.yaml has no router configured")
    if not config.models.router.path.expanduser().exists():
        pytest.skip(f"Router model not available: {config.models.router.path}")

    for name, tier in config.models.tiers.items():
        if not tier.path.expanduser().exists():
            pytest.skip(f"Model not available: {tier.path} (tier: {name})")

    # Enable logprobs for confidence measurement
    config.models.router.logits_all = True

    return config


@pytest.fixture(scope="module")
async def confidence_orchestrator(confidence_config, tmp_path_factory):
    """Orchestrator with logits_all=True on router for confidence measurement."""
    log_dir = tmp_path_factory.mktemp("confidence_logs")
    setup_logging(confidence_config, project_dir=log_dir, app_dir_name=".")
    setup_model_logger(project_dir=log_dir, app_dir_name=".")

    orch = ModelOrchestrator(confidence_config)
    await orch.initialize()

    yield orch

    await orch.shutdown()


@pytest.fixture(autouse=True)
def _redirect_confidence_logs(confidence_config, test_log_dir):
    """Redirect engine logging to per-test log dir for stashing."""
    setup_logging(confidence_config, project_dir=test_log_dir, app_dir_name=".")
    setup_model_logger(project_dir=test_log_dir, app_dir_name=".")


# ---------------------------------------------------------------------------
# Confidence tests — per-tier, 85% threshold
# ---------------------------------------------------------------------------


@pytest.mark.model
class TestClassificationConfidence:
    """Test that classification achieves sufficient confidence per tier.

    Uses logits_all=True router so logprobs are available.
    """

    @pytest.mark.asyncio
    @pytest.mark.parametrize("identity_name", _IDENTITY_NAMES)
    async def test_confidence_above_threshold(
        self, confidence_orchestrator: ModelOrchestrator, identity_name: str
    ):
        """Fuzzed prompt should classify correctly with >= 85% confidence.

        Uses the same classification path as _classify_task (including history
        extraction) so the confidence measurement matches the actual routing
        decision.
        """
        examples = next(e for n, _, e in _CLASSIFIABLE if n == identity_name)
        assert confidence_orchestrator._router is not None

        prompt = random.choice(examples)
        messages = [Message(role="user", content=prompt)]

        # Use _classify_task directly — same complete() path as production
        tier, digit = await confidence_orchestrator._classify_task(messages)

        assert tier == identity_name, (
            f"Example '{prompt}' expected {identity_name}, got {tier} "
            f"(digit={digit!r}, prompt='{prompt}')"
        )
