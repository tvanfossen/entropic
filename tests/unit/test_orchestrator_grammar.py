"""Tests for grammar injection in ModelOrchestrator."""

import logging
from pathlib import Path
from unittest.mock import AsyncMock, MagicMock, PropertyMock

import pytest
from entropic.config.schema import LibraryConfig, TierConfig
from entropic.core.base import GenerationResult, ModelBackend
from entropic.inference.orchestrator import ModelOrchestrator


def _make_orchestrator(grammar_path: Path | None = None) -> ModelOrchestrator:
    """Build an orchestrator with a single tier and mock backend."""
    tier_cfg = TierConfig(path=Path("/fake.gguf"), grammar=grammar_path)
    config = LibraryConfig(
        models={"tiers": {"normal": tier_cfg}, "default": "normal"},
        routing={"enabled": False, "fallback_tier": "normal"},
    )

    mock_backend = MagicMock(spec=ModelBackend)
    mock_backend.is_loaded = True
    type(mock_backend).config = PropertyMock(return_value=tier_cfg)
    mock_backend.generate = AsyncMock(
        return_value=GenerationResult(content="ok", tool_calls=[], finish_reason="stop"),
    )
    mock_backend.adapter = MagicMock()
    mock_backend.adapter.parse_tool_calls.return_value = ("ok", [])

    def factory(model_config: object, tier_name: str) -> MagicMock:
        return mock_backend

    orch = ModelOrchestrator(config, backend_factory=factory)
    return orch


class TestGrammarInjection:
    """Tests for _resolve_grammar and injection into generate/generate_stream."""

    @pytest.mark.asyncio
    async def test_grammar_injected_generate(self, tmp_path: Path) -> None:
        """generate() receives grammar kwarg when tier has grammar path."""
        gbnf = tmp_path / "test.gbnf"
        gbnf.write_text('root ::= "hello"')
        orch = _make_orchestrator(grammar_path=gbnf)
        await orch.initialize()

        tier = orch._tier_list[0]
        messages = [MagicMock(role="user", content="hi")]
        await orch.generate(messages, tier=tier)

        backend = orch._tiers[tier]
        call_kwargs = backend.generate.call_args
        assert call_kwargs.kwargs.get("grammar") == 'root ::= "hello"'

    @pytest.mark.asyncio
    async def test_grammar_injected_stream(self, tmp_path: Path) -> None:
        """generate_stream() receives grammar kwarg when tier has grammar path."""
        gbnf = tmp_path / "test.gbnf"
        gbnf.write_text('root ::= "hello"')
        orch = _make_orchestrator(grammar_path=gbnf)
        await orch.initialize()

        tier = orch._tier_list[0]
        backend = orch._tiers[tier]

        async def fake_stream(*args: object, **kwargs: object) -> None:  # type: ignore[misc]
            # Must be an async generator
            return
            yield  # noqa: RET504 — makes this an async generator

        backend.generate_stream = fake_stream

        # Verify _resolve_grammar returns the grammar for this tier
        grammar_str = orch._resolve_grammar(tier)
        assert grammar_str == 'root ::= "hello"'

    @pytest.mark.asyncio
    async def test_grammar_not_injected_when_none(self) -> None:
        """No grammar config means no injection."""
        orch = _make_orchestrator(grammar_path=None)
        await orch.initialize()

        tier = orch._tier_list[0]
        messages = [MagicMock(role="user", content="hi")]
        await orch.generate(messages, tier=tier)

        backend = orch._tiers[tier]
        call_kwargs = backend.generate.call_args
        assert "grammar" not in call_kwargs.kwargs

    @pytest.mark.asyncio
    async def test_grammar_cached(self, tmp_path: Path) -> None:
        """Second call uses cached string, not re-read."""
        gbnf = tmp_path / "test.gbnf"
        gbnf.write_text('root ::= "hello"')
        orch = _make_orchestrator(grammar_path=gbnf)
        await orch.initialize()

        tier = orch._tier_list[0]

        result1 = orch._resolve_grammar(tier)
        # Mutate the file — cache should prevent re-read
        gbnf.write_text('root ::= "changed"')
        result2 = orch._resolve_grammar(tier)

        assert result1 == result2 == 'root ::= "hello"'

    @pytest.mark.asyncio
    async def test_grammar_missing_file_warns(
        self, tmp_path: Path, caplog: pytest.LogCaptureFixture
    ) -> None:
        """Nonexistent grammar path logs warning, returns None."""
        missing = tmp_path / "nonexistent.gbnf"
        orch = _make_orchestrator(grammar_path=missing)
        await orch.initialize()

        tier = orch._tier_list[0]
        with caplog.at_level(logging.WARNING):
            result = orch._resolve_grammar(tier)

        assert result is None
        assert "Grammar file not found" in caplog.text

    @pytest.mark.asyncio
    async def test_grammar_override_via_kwargs(self, tmp_path: Path) -> None:
        """Explicit grammar= in kwargs takes precedence over config."""
        gbnf = tmp_path / "test.gbnf"
        gbnf.write_text('root ::= "from_config"')
        orch = _make_orchestrator(grammar_path=gbnf)
        await orch.initialize()

        tier = orch._tier_list[0]
        messages = [MagicMock(role="user", content="hi")]
        await orch.generate(messages, tier=tier, grammar='root ::= "override"')

        backend = orch._tiers[tier]
        call_kwargs = backend.generate.call_args
        assert call_kwargs.kwargs.get("grammar") == 'root ::= "override"'
