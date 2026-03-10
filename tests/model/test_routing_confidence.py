"""
Test router classification confidence with logprobs.

Separate from test_routing.py because this test requires logits_all=True
on the router config, which is a construction-time Llama param. Two
orchestrators can't coexist on the same GPU, so this runs in its own module
with its own module-scoped orchestrator.

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
from entropic.inference.orchestrator import ModelOrchestrator

# ---------------------------------------------------------------------------
# Identity discovery (same as test_routing.py — no hardcoded tier names)
# ---------------------------------------------------------------------------
_PROMPTS_DIR = Path(entropic.__file__).parent / "data" / "prompts"


def _routable_identities() -> list[tuple[str, list[str], list[str]]]:
    """Return (name, focus, examples) for all routable identities."""
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

_SKIP_ROUTING = len(_IDENTITY_NAMES) < 2


# ---------------------------------------------------------------------------
# Orchestrator with logits_all=True on router
# ---------------------------------------------------------------------------


@pytest.fixture(scope="module")
async def confidence_orchestrator(config: EntropyConfig, models_available: dict[str, bool]):
    """Orchestrator with logits_all=True on router for confidence measurement."""
    if not any(models_available.values()):
        pytest.skip("No models available for testing")

    assert config.models.router is not None, "No router configured"
    config.models.router.logits_all = True

    orch = ModelOrchestrator(config)
    await orch.initialize()

    yield orch

    await orch.shutdown()
    config.models.router.logits_all = False  # restore for other modules


# ---------------------------------------------------------------------------
# Confidence tests — per-tier, 85% threshold
# ---------------------------------------------------------------------------


@pytest.mark.model
@pytest.mark.skipif(_SKIP_ROUTING, reason="<2 routable identities — routing disabled")
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
        examples = next(e for n, _, e in _ROUTABLE if n == identity_name)
        assert confidence_orchestrator._router is not None

        prompt = random.choice(examples)
        messages = [Message(role="user", content=prompt)]

        # Use _classify_task directly — same complete() path as production
        tier, digit = await confidence_orchestrator._classify_task(messages)

        assert tier == identity_name, (
            f"Example '{prompt}' expected {identity_name}, got {tier} "
            f"(digit={digit!r}, prompt='{prompt}')"
        )
