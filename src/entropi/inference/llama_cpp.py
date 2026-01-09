"""
llama-cpp-python backend implementation.

Wraps llama-cpp-python for GGUF model inference with CUDA support.

NOTE: We intentionally do NOT use llama-cpp-python's native tool calling
(passing `tools` to create_chat_completion). The `chatml-function-calling`
template has a bug where tool message content is dropped:

    <|im_start|>tool
    <|im_end|>   ← CONTENT DROPPED

This causes the model to never see tool results, leading to hallucination.

Instead, we use content-based tool calling:
1. Tools are injected into the system prompt by the adapter
2. Model outputs tool calls as JSON in its response content
3. Adapter parses tool calls from content
4. Tool results are sent as user messages (which chatml renders correctly)

This approach works reliably with all Qwen models. Revisit if llama-cpp-python
fixes the chatml-function-calling template.
"""

import asyncio
import contextlib
import io
import os
import sys
import time
from collections.abc import AsyncIterator
from pathlib import Path
from typing import Any

from llama_cpp import Llama, LlamaGrammar

from entropi.config.schema import ModelConfig
from entropi.core.base import GenerationResult, Message, ModelBackend
from entropi.core.logging import get_logger
from entropi.inference.adapters.base import ChatAdapter, get_adapter
from entropi.inference.backend import GenerationConfig

logger = get_logger("inference.llama_cpp")


class LlamaCppBackend(ModelBackend):
    """llama-cpp-python model backend."""

    def __init__(
        self,
        config: ModelConfig,
        adapter: ChatAdapter | None = None,
        prompts_dir: Path | None = None,
    ) -> None:
        """
        Initialize backend.

        Args:
            config: Model configuration
            adapter: Chat adapter (resolved from config.adapter if None)
            prompts_dir: Optional directory for user prompt overrides
        """
        self.config = config
        self._adapter = adapter or get_adapter(config.adapter, prompts_dir=prompts_dir)
        self._model: Llama | None = None
        self._lock = asyncio.Lock()
        self._last_finish_reason: str = "stop"

    async def load(self) -> None:
        """Load the model into memory."""
        if self._model is not None:
            return

        async with self._lock:
            if self._model is not None:
                return

            logger.info(f"Loading model: {self.config.path}")
            start = time.time()

            # Run in thread pool to avoid blocking
            loop = asyncio.get_event_loop()
            self._model = await loop.run_in_executor(
                None,
                self._load_model_sync,
            )

            elapsed = time.time() - start
            logger.info(f"Model loaded in {elapsed:.2f}s")

    def _load_model_sync(self) -> Llama:
        """Synchronous model loading."""
        # Suppress llama.cpp informational messages (n_ctx_per_seq < n_ctx_train etc)
        # These go to stderr from the C++ layer and can't be controlled via verbose=False
        old_stderr = sys.stderr
        sys.stderr = io.StringIO()
        try:
            model = Llama(
                model_path=str(self.config.path),
                n_ctx=self.config.context_length,
                n_gpu_layers=self.config.gpu_layers,
                chat_format=self._adapter.chat_format,
                verbose=False,
            )
        finally:
            sys.stderr = old_stderr
        return model

    async def unload(self) -> None:
        """Unload the model from memory."""
        async with self._lock:
            if self._model is not None:
                # Let garbage collector handle cleanup
                self._model = None
                logger.info("Model unloaded")

    async def generate(
        self,
        messages: list[Message],
        max_tokens: int = 4096,
        stop: list[str] | None = None,
        grammar: str | None = None,
        **kwargs: Any,
    ) -> GenerationResult:
        """
        Generate a response.

        Args:
            messages: Conversation messages
            max_tokens: Maximum tokens to generate
            stop: Stop sequences
            grammar: Optional GBNF grammar string to constrain output
            **kwargs: Additional generation parameters

        Returns:
            Generation result
        """
        if self._model is None:
            await self.load()

        # Convert messages to llama-cpp format
        llama_messages = self._convert_messages(messages)

        # Build generation config
        gen_config = GenerationConfig(
            max_tokens=max_tokens,
            temperature=kwargs.get("temperature", self.config.temperature),
            top_p=kwargs.get("top_p", self.config.top_p),
            top_k=kwargs.get("top_k", self.config.top_k),
            repeat_penalty=kwargs.get("repeat_penalty", self.config.repeat_penalty),
            stop=stop or [],
            grammar=grammar,
        )

        # Run generation in thread pool
        start = time.time()
        loop = asyncio.get_event_loop()
        result = await loop.run_in_executor(
            None,
            lambda: self._generate_sync(llama_messages, gen_config),
        )
        elapsed = int((time.time() - start) * 1000)

        # Extract content from response
        message = result["choices"][0]["message"]
        content = message.get("content") or ""
        finish_reason = result["choices"][0].get("finish_reason", "unknown")
        logger.debug(f"\nGeneration complete: finish_reason={finish_reason}, content_len={len(content)}")

        return GenerationResult(
            content=content,
            tool_calls=[],  # Tool calls parsed from content by adapter
            finish_reason=result["choices"][0].get("finish_reason", "stop"),
            token_count=result.get("usage", {}).get("completion_tokens", 0),
            generation_time_ms=elapsed,
        )

    def _generate_sync(
        self,
        messages: list[dict[str, Any]],
        config: GenerationConfig,
    ) -> dict[str, Any]:
        """Synchronous generation."""
        assert self._model is not None

        # Build kwargs for create_chat_completion
        kwargs: dict[str, Any] = {
            "messages": messages,
            "max_tokens": config.max_tokens,
            "temperature": config.temperature,
            "top_p": config.top_p,
            "top_k": config.top_k,
            "repeat_penalty": config.repeat_penalty,
        }

        if config.stop:
            kwargs["stop"] = config.stop

        # Apply GBNF grammar if provided (constrains output to match pattern)
        if config.grammar:
            kwargs["grammar"] = LlamaGrammar.from_string(config.grammar)
            logger.debug(f"Using GBNF grammar: {config.grammar}")

        result: dict[str, Any] = self._model.create_chat_completion(**kwargs)
        return result

    async def generate_stream(
        self,
        messages: list[Message],
        max_tokens: int = 4096,
        stop: list[str] | None = None,
        **kwargs: Any,
    ) -> AsyncIterator[str]:
        """
        Generate a streaming response.

        Args:
            messages: Conversation messages
            max_tokens: Maximum tokens to generate
            stop: Stop sequences
            **kwargs: Additional generation parameters

        Yields:
            Response chunks
        """
        if self._model is None:
            await self.load()

        llama_messages = self._convert_messages(messages)

        # Reset model state before generation to ensure clean KV cache
        # This prevents llama_decode errors when context changes between calls
        # (especially important for multi-turn tool use conversations)
        if hasattr(self._model, "reset"):
            self._model.reset()

        gen_config = GenerationConfig(
            max_tokens=max_tokens,
            temperature=kwargs.get("temperature", self.config.temperature),
            top_p=kwargs.get("top_p", self.config.top_p),
            top_k=kwargs.get("top_k", self.config.top_k),
            repeat_penalty=kwargs.get("repeat_penalty", self.config.repeat_penalty),
            stop=stop or [],
            stream=True,
        )

        # Create stream generator in thread pool
        loop = asyncio.get_event_loop()

        def create_stream() -> Any:
            assert self._model is not None
            return self._model.create_chat_completion(
                messages=llama_messages,
                max_tokens=gen_config.max_tokens,
                temperature=gen_config.temperature,
                top_p=gen_config.top_p,
                top_k=gen_config.top_k,
                repeat_penalty=gen_config.repeat_penalty,
                stop=gen_config.stop if gen_config.stop else None,
                stream=True,
            )

        stream = await loop.run_in_executor(None, create_stream)

        # Yield chunks from stream, capture finish_reason from final chunk
        for chunk in stream:
            choice = chunk["choices"][0]
            delta = choice.get("delta", {})
            if delta.get("content"):
                yield delta["content"]
            # Capture finish_reason from final chunk
            if choice.get("finish_reason"):
                self._last_finish_reason = choice["finish_reason"]
                logger.debug(f"Stream finish_reason: {self._last_finish_reason}")

    def _convert_messages(self, messages: list[Message]) -> list[dict[str, Any]]:
        """Convert Message objects to llama-cpp format."""
        return [{"role": msg.role, "content": msg.content} for msg in messages]

    def count_tokens(self, text: str) -> int:
        """Count tokens in text."""
        if self._model is None:
            # Estimate if model not loaded (~4 chars per token)
            return len(text) // 4
        return len(self._model.tokenize(text.encode()))

    @property
    def context_length(self) -> int:
        """Get the model's context length."""
        return self.config.context_length

    @property
    def is_loaded(self) -> bool:
        """Check if model is loaded."""
        return self._model is not None

    @property
    def adapter(self) -> ChatAdapter:
        """Get the adapter for this backend."""
        return self._adapter

    @property
    def last_finish_reason(self) -> str:
        """Get the finish_reason from the last generation (streaming or non-streaming)."""
        return self._last_finish_reason
