"""Tests for grammar passthrough and finish_reason in LlamaCppBackend."""

from __future__ import annotations

import asyncio
from unittest.mock import MagicMock, patch

import pytest
from entropic.inference.backend import GenerationConfig
from entropic.inference.llama_cpp import LlamaCppBackend


class TestStreamGrammarInGenConfig:
    """Verify generate_stream() populates grammar on GenerationConfig."""

    def test_stream_grammar_in_gen_config(self) -> None:
        """GenerationConfig accepts grammar field for streaming."""
        config = GenerationConfig(stream=True, grammar='root ::= "hello"')
        assert config.grammar == 'root ::= "hello"'
        assert config.stream is True

    def test_stream_grammar_none_default(self) -> None:
        """GenerationConfig defaults grammar to None."""
        config = GenerationConfig(stream=True)
        assert config.grammar is None


class TestStreamToQueuePassesGrammar:
    """Verify _stream_to_queue passes LlamaGrammar to create_chat_completion."""

    @patch("entropic.inference.llama_cpp.LlamaGrammar")
    def test_grammar_object_created_and_passed(self, mock_grammar_cls: MagicMock) -> None:
        """_stream_to_queue creates LlamaGrammar and passes it to llama-cpp."""
        mock_grammar_obj = MagicMock()
        mock_grammar_cls.from_string.return_value = mock_grammar_obj

        config = GenerationConfig(
            stream=True,
            grammar='root ::= "test"',
            stop=[],
        )

        # Build a minimal mock backend to call _stream_to_queue
        with patch.object(LlamaCppBackend, "__init__", lambda self, **kw: None):
            backend = LlamaCppBackend.__new__(LlamaCppBackend)

        mock_model = MagicMock()
        mock_model.create_chat_completion.return_value = iter([])
        backend._model = mock_model
        backend._last_finish_reason = "stop"

        loop = asyncio.new_event_loop()
        queue: asyncio.Queue[str | None] = asyncio.Queue()
        cancel = MagicMock()
        cancel.is_set.return_value = False

        backend._stream_to_queue([], config, queue, loop, cancel)
        loop.close()

        mock_grammar_cls.from_string.assert_called_once_with('root ::= "test"')
        call_kwargs = mock_model.create_chat_completion.call_args.kwargs
        assert call_kwargs["grammar"] is mock_grammar_obj

    @patch("entropic.inference.llama_cpp.LlamaGrammar")
    def test_no_grammar_passes_none(self, mock_grammar_cls: MagicMock) -> None:
        """_stream_to_queue passes grammar=None when no grammar configured."""
        config = GenerationConfig(stream=True, grammar=None, stop=[])

        with patch.object(LlamaCppBackend, "__init__", lambda self, **kw: None):
            backend = LlamaCppBackend.__new__(LlamaCppBackend)

        mock_model = MagicMock()
        mock_model.create_chat_completion.return_value = iter([])
        backend._model = mock_model
        backend._last_finish_reason = "stop"

        loop = asyncio.new_event_loop()
        queue: asyncio.Queue[str | None] = asyncio.Queue()
        cancel = MagicMock()
        cancel.is_set.return_value = False

        backend._stream_to_queue([], config, queue, loop, cancel)
        loop.close()

        mock_grammar_cls.from_string.assert_not_called()
        call_kwargs = mock_model.create_chat_completion.call_args.kwargs
        assert call_kwargs["grammar"] is None


# ===========================================================================
# Non-streaming finish_reason propagation
# ===========================================================================


class TestNonStreamingFinishReason:
    """Verify generate() updates _last_finish_reason (C1 fix)."""

    @staticmethod
    def _make_backend() -> LlamaCppBackend:
        with patch.object(LlamaCppBackend, "__init__", lambda s, **kw: None):
            backend = LlamaCppBackend.__new__(LlamaCppBackend)
        backend._last_finish_reason = "stop"
        backend._model = MagicMock()
        backend._lock = MagicMock()
        backend.config = MagicMock(temperature=0.7, top_p=0.9, top_k=40, repeat_penalty=1.1)
        backend._adapter = MagicMock()
        return backend

    @pytest.mark.asyncio
    async def test_length_propagates(self) -> None:
        """finish_reason=length from model → _last_finish_reason updated."""
        backend = self._make_backend()
        response = {
            "choices": [{"message": {"content": "partial"}, "finish_reason": "length"}],
            "usage": {"completion_tokens": 100},
        }
        with patch.object(type(backend), "_generate_sync", return_value=response):
            result = await backend.generate([], max_tokens=100)

        assert result.finish_reason == "length"
        assert backend._last_finish_reason == "length"

    @pytest.mark.asyncio
    async def test_stop_propagates(self) -> None:
        """finish_reason=stop from model → _last_finish_reason updated."""
        backend = self._make_backend()
        backend._last_finish_reason = "length"
        response = {
            "choices": [{"message": {"content": "done"}, "finish_reason": "stop"}],
            "usage": {"completion_tokens": 50},
        }
        with patch.object(type(backend), "_generate_sync", return_value=response):
            result = await backend.generate([], max_tokens=100)

        assert result.finish_reason == "stop"
        assert backend._last_finish_reason == "stop"
