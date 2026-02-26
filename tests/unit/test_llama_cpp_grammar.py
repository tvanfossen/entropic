"""Tests for grammar passthrough in LlamaCppBackend streaming path."""

from unittest.mock import MagicMock, patch

from entropic.inference.backend import GenerationConfig


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
        from entropic.inference.llama_cpp import LlamaCppBackend

        with patch.object(LlamaCppBackend, "__init__", lambda self, **kw: None):
            backend = LlamaCppBackend.__new__(LlamaCppBackend)

        mock_model = MagicMock()
        mock_model.create_chat_completion.return_value = iter([])
        backend._model = mock_model
        backend._last_finish_reason = "stop"

        import asyncio

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

        from entropic.inference.llama_cpp import LlamaCppBackend

        with patch.object(LlamaCppBackend, "__init__", lambda self, **kw: None):
            backend = LlamaCppBackend.__new__(LlamaCppBackend)

        mock_model = MagicMock()
        mock_model.create_chat_completion.return_value = iter([])
        backend._model = mock_model
        backend._last_finish_reason = "stop"

        import asyncio

        loop = asyncio.new_event_loop()
        queue: asyncio.Queue[str | None] = asyncio.Queue()
        cancel = MagicMock()
        cancel.is_set.return_value = False

        backend._stream_to_queue([], config, queue, loop, cancel)
        loop.close()

        mock_grammar_cls.from_string.assert_not_called()
        call_kwargs = mock_model.create_chat_completion.call_args.kwargs
        assert call_kwargs["grammar"] is None
