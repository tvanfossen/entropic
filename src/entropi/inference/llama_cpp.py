"""
llama-cpp-python backend implementation.

Wraps llama-cpp-python for GGUF model inference with CUDA support.
"""

import asyncio
import time
from collections.abc import AsyncIterator
from typing import Any

from llama_cpp import Llama

from entropi.config.schema import ModelConfig
from entropi.core.base import GenerationResult, Message, ModelBackend
from entropi.core.logging import get_logger
from entropi.inference.backend import GenerationConfig

logger = get_logger("inference.llama_cpp")


class LlamaCppBackend(ModelBackend):
    """llama-cpp-python model backend."""

    def __init__(
        self,
        config: ModelConfig,
        chat_format: str = "chatml",
    ) -> None:
        """
        Initialize backend.

        Args:
            config: Model configuration
            chat_format: Chat format to use (chatml for Qwen)
        """
        self.config = config
        self.chat_format = chat_format
        self._model: Llama | None = None
        self._lock = asyncio.Lock()

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
        return Llama(
            model_path=str(self.config.path),
            n_ctx=self.config.context_length,
            n_gpu_layers=self.config.gpu_layers,
            chat_format=self.chat_format,
            verbose=False,
        )

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
        **kwargs: Any,
    ) -> GenerationResult:
        """
        Generate a response.

        Args:
            messages: Conversation messages
            max_tokens: Maximum tokens to generate
            stop: Stop sequences
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
        content = result["choices"][0]["message"]["content"]

        return GenerationResult(
            content=content,
            tool_calls=[],  # Tool calls parsed by adapter
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
        result: dict[str, Any] = self._model.create_chat_completion(
            messages=messages,
            max_tokens=config.max_tokens,
            temperature=config.temperature,
            top_p=config.top_p,
            top_k=config.top_k,
            repeat_penalty=config.repeat_penalty,
            stop=config.stop if config.stop else None,
        )
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

        # Yield chunks from stream
        for chunk in stream:
            delta = chunk["choices"][0]["delta"]
            if delta.get("content"):
                yield delta["content"]

    def _convert_messages(self, messages: list[Message]) -> list[dict[str, str]]:
        """Convert Message objects to llama-cpp format."""
        result = []
        for msg in messages:
            converted = {
                "role": msg.role,
                "content": msg.content,
            }
            result.append(converted)
        return result

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
