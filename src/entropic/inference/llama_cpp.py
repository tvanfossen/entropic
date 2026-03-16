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
import inspect
import io
import sys
import threading
import time
from collections.abc import AsyncIterator
from typing import Any

from llama_cpp import Llama, LlamaGrammar
from llama_cpp.llama_chat_format import Jinja2ChatFormatter

from entropic.config.schema import ModelConfig
from entropic.core.base import GenerationResult, Message, ModelBackend, ModelState
from entropic.core.logging import get_logger
from entropic.inference.adapters.base import ChatAdapter, get_adapter
from entropic.inference.backend import GenerationConfig
from entropic.prompts.manager import PromptManager

logger = get_logger("inference.llama_cpp")

_gpu_check_done = False


def _check_gpu_offload() -> None:
    """Warn once if llama-cpp-python was built without GPU support.

    Checks llama_supports_gpu_offload() unconditionally — if the installed
    build is CPU-only, logs a warning with install commands for GPU backends.
    """
    global _gpu_check_done  # noqa: PLW0603
    if _gpu_check_done:
        return
    _gpu_check_done = True

    try:
        from llama_cpp import llama_supports_gpu_offload

        if llama_supports_gpu_offload():
            return
    except ImportError:
        pass

    logger.warning(
        "llama-cpp-python is installed WITHOUT GPU acceleration. "
        "Models will run on CPU only, which is significantly slower.\n"
        "\n"
        "Recommended:\n"
        "  Source install:  ./install.sh  (auto-detects GPU, builds with CUDA)\n"
        "  PyPI install:    entropic setup-cuda  (rebuilds with CUDA)\n"
        "\n"
        "For other backends (Metal, Vulkan, SYCL, etc.), see:\n"
        "  https://github.com/abetlen/llama-cpp-python#installation"
    )


class LlamaCppBackend(ModelBackend):
    """llama-cpp-python model backend."""

    def __init__(
        self,
        config: ModelConfig,
        tier: str,
        adapter: ChatAdapter | None = None,
        prompt_manager: PromptManager | None = None,
    ) -> None:
        """
        Initialize backend.

        Args:
            config: Model configuration
            tier: Model tier name
            adapter: Chat adapter (resolved from config.adapter if None)
            prompt_manager: Central prompt loader
        """
        self.config = config
        self._adapter = adapter or get_adapter(config.adapter, tier, prompt_manager=prompt_manager)
        self._model: Llama | None = None
        self._state: ModelState = ModelState.COLD
        self._lock = asyncio.Lock()
        self._last_finish_reason: str = "stop"
        self._has_chat_template_kwargs: bool = False
        self._default_chat_handler: Any = None
        self._no_think_chat_handler: Any = None

    # ------------------------------------------------------------------
    # Three-state lifecycle: COLD → WARM → ACTIVE
    # ------------------------------------------------------------------

    @property
    def state(self) -> ModelState:
        """Current lifecycle state."""
        return self._state

    async def warm(self) -> None:
        """Load model into CPU RAM only (n_gpu_layers=0).

        COLD → WARM. No-op if already WARM or ACTIVE.
        """
        if self._state != ModelState.COLD:
            return

        async with self._lock:
            if self._state != ModelState.COLD:
                return

            _check_gpu_offload()
            logger.info(f"[VRAM] Warming: {self.config.path.name}")
            start = time.time()

            loop = asyncio.get_running_loop()
            self._model = await loop.run_in_executor(
                None, lambda: self._load_model_sync(gpu_layers=0)
            )
            self._detect_template_support()
            self._state = ModelState.WARM

            elapsed = time.time() - start
            logger.info(f"[VRAM] Warm in {elapsed:.2f}s (CPU RAM, mlock={self.config.use_mlock})")

    async def activate(self, gpu_layers: int = -1) -> None:
        """Promote model to GPU (WARM/COLD → ACTIVE).

        If COLD, warms first. No-op if already ACTIVE.

        Args:
            gpu_layers: GPU layers to offload. -1 = all layers.
        """
        if self._state == ModelState.ACTIVE:
            return

        if self._state == ModelState.COLD:
            await self.warm()

        async with self._lock:
            if self._state == ModelState.ACTIVE:
                return  # concurrent call already activated

            logger.info(f"[VRAM] Activating: {self.config.path.name} ({gpu_layers} GPU layers)")
            start = time.time()

            await self._swap_model(gpu_layers)
            self._detect_template_support()
            self._state = ModelState.ACTIVE

            elapsed = time.time() - start
            logger.info(f"[VRAM] Active in {elapsed:.2f}s")

    async def deactivate(self) -> None:
        """Release GPU layers, return to WARM (ACTIVE → WARM).

        No-op if not ACTIVE.
        """
        if self._state != ModelState.ACTIVE:
            return

        async with self._lock:
            if self._state != ModelState.ACTIVE:
                return

            logger.info(f"[VRAM] Deactivating: {self.config.path.name}")
            start = time.time()

            await self._swap_model(gpu_layers=0)
            self._detect_template_support()
            self._state = ModelState.WARM

            elapsed = time.time() - start
            logger.info(f"[VRAM] Deactivated in {elapsed:.2f}s")

    async def load(self) -> None:
        """Load the model. Convenience wrapper: warm() + activate()."""
        await self.warm()
        await self.activate(self.config.gpu_layers)

    async def unload(self) -> None:
        """Unload the model fully (→ COLD). Releases all RAM and VRAM."""
        async with self._lock:
            if self._model is not None:
                self._model.close()
            self._model = None
            self._state = ModelState.COLD
            logger.info(f"[VRAM] Unloaded: {self.config.path.name}")

    async def _swap_model(self, gpu_layers: int) -> None:
        """Free current model instance and reload with specified GPU layers.

        Caller must hold self._lock. Page cache (mlock) makes this fast
        when going WARM→ACTIVE (~1–3s PCIe transfer, no disk I/O).

        Old model is freed inside the executor thread so llama_free() fires
        in a native thread context, not the asyncio event loop (which causes
        a GC-triggered crash in llama_cpp._internals.free_model).
        """
        old_model = self._model
        self._model = None

        loop = asyncio.get_running_loop()
        self._model = await loop.run_in_executor(
            None, lambda: self._free_and_load(old_model, gpu_layers)
        )

    def _free_and_load(self, old_model: "Llama | None", gpu_layers: int) -> "Llama":
        """Free old model and load new one in the executor thread.

        Explicitly closes the Llama model's native resources before dropping
        the Python reference. This prevents the GC-triggered ``__del__`` →
        ``free_model`` segfault in llama_cpp._internals, where native CUDA
        resources are freed in an inconsistent state during garbage collection.
        """
        import gc

        if old_model is not None:
            # Explicit close releases native resources (CUDA, mmap) in a
            # controlled context — NOT during GC's __del__ finalization.
            old_model.close()
            del old_model
            gc.collect()

        return self._load_model_sync(gpu_layers=gpu_layers)

    def _load_model_sync(self, gpu_layers: int | None = None) -> Llama:
        """Synchronous model load. gpu_layers=None uses config value."""
        effective_layers = gpu_layers if gpu_layers is not None else self.config.gpu_layers
        # Suppress llama.cpp informational messages (n_ctx_per_seq < n_ctx_train etc)
        old_stderr = sys.stderr
        sys.stderr = io.StringIO()
        try:
            return Llama(
                model_path=str(self.config.path),
                n_ctx=self.config.context_length,
                n_gpu_layers=effective_layers,
                chat_format=self._adapter.chat_format,
                use_mmap=True,
                use_mlock=self.config.use_mlock,
                logits_all=self.config.logits_all,
                verbose=False,
            )
        finally:
            sys.stderr = old_stderr

    def _detect_template_support(self) -> None:
        """Detect chat_template_kwargs support and cache no-think handler.

        When llama-cpp-python doesn't support ``chat_template_kwargs``, we build
        a no-think chat handler from the GGUF template. Both the default and
        no-think handlers are cached so they can be swapped per-request — the
        same backend may serve identities with and without thinking enabled.
        """
        assert self._model is not None
        sig = inspect.signature(self._model.create_chat_completion)
        self._has_chat_template_kwargs = "chat_template_kwargs" in sig.parameters

        # Cache default handler + build no-think variant for per-request swap
        if not self._has_chat_template_kwargs:
            self._default_chat_handler = self._model.chat_handler
            self._no_think_chat_handler = self._build_no_think_handler()

    def _build_no_think_handler(self) -> Any:
        """Build a chat handler that injects enable_thinking=False into the jinja template.

        Works around llama-cpp-python's Jinja2ChatFormatter not forwarding
        extra kwargs to the template render call. Constructs a formatter from
        the GGUF's embedded template and monkey-patches its render method to
        always set enable_thinking=False.

        Returns:
            The patched chat handler, or None if template not available.
        """
        assert self._model is not None
        template = self._model.metadata.get("tokenizer.chat_template")
        if not template:
            logger.warning("No chat template in GGUF metadata, cannot build no-think handler")
            return None

        eos_id = self._model.token_eos()
        bos_id = self._model.token_bos()
        eos_token = self._model._model.token_get_text(eos_id) if eos_id != -1 else ""
        bos_token = self._model._model.token_get_text(bos_id) if bos_id != -1 else ""

        formatter = Jinja2ChatFormatter(
            template=template,
            eos_token=eos_token,
            bos_token=bos_token,
            stop_token_ids=[eos_id],
        )

        original_render = formatter._environment.render

        def render_no_think(**kwargs):
            kwargs.setdefault("enable_thinking", False)
            return original_render(**kwargs)

        formatter._environment.render = render_no_think
        handler = formatter.to_chat_handler()
        logger.info("Built no-think chat handler (enable_thinking=False)")
        return handler

    def _build_gen_config(
        self,
        max_tokens: int = 4096,
        stop: list[str] | None = None,
        grammar: str | None = None,
        stream: bool = False,
        **kwargs: Any,
    ) -> GenerationConfig:
        """Build GenerationConfig with kwargs overriding backend defaults."""
        return GenerationConfig(
            max_tokens=max_tokens,
            temperature=kwargs.get("temperature", 0.7),
            top_p=kwargs.get("top_p", 0.9),
            top_k=kwargs.get("top_k", 40),
            repeat_penalty=kwargs.get("repeat_penalty", 1.1),
            stop=stop or [],
            grammar=grammar,
            logprobs=kwargs.get("logprobs"),
            stream=stream,
            chat_template_kwargs=kwargs.get("chat_template_kwargs", {}),
        )

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
        if not self.is_loaded:
            await self.load()

        # Convert messages to llama-cpp format
        llama_messages = self._convert_messages(messages)
        gen_config = self._build_gen_config(
            max_tokens=max_tokens, stop=stop, grammar=grammar, **kwargs
        )

        # Run generation in thread pool
        start = time.time()
        loop = asyncio.get_running_loop()
        result = await loop.run_in_executor(
            None,
            lambda: self._generate_sync(llama_messages, gen_config),
        )
        elapsed = int((time.time() - start) * 1000)

        # Extract content from response
        message = result["choices"][0]["message"]
        content = message.get("content") or ""
        finish_reason = result["choices"][0].get("finish_reason", "stop")
        self._last_finish_reason = finish_reason
        logger.debug(
            f"\nGeneration complete: finish_reason={finish_reason}, content_len={len(content)}"
        )

        return GenerationResult(
            content=content,
            tool_calls=[],  # Tool calls parsed from content by adapter
            finish_reason=finish_reason,
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

        # Pass template kwargs (e.g. enable_thinking for Qwen3.5)
        if config.chat_template_kwargs and self._has_chat_template_kwargs:
            kwargs["chat_template_kwargs"] = config.chat_template_kwargs

        # Per-request no-think handler swap when llama-cpp doesn't support
        # chat_template_kwargs natively. Swap handler before generation,
        # restore after — backends are shared across identities.
        needs_no_think = (
            not self._has_chat_template_kwargs
            and config.chat_template_kwargs
            and config.chat_template_kwargs.get("enable_thinking") is False
            and self._no_think_chat_handler is not None
        )
        if needs_no_think:
            self._model.chat_handler = self._no_think_chat_handler

        try:
            result: dict[str, Any] = self._model.create_chat_completion(**kwargs)
        finally:
            if needs_no_think:
                self._model.chat_handler = self._default_chat_handler

        return result

    async def complete(
        self,
        prompt: str,
        max_tokens: int = 16,
        grammar: str | None = None,
        **kwargs: Any,
    ) -> GenerationResult:
        """Raw text completion (no chat template).

        Used for classification and other tasks where the model should
        continue a prompt directly without chat scaffolding.

        Args:
            prompt: Raw text prompt to complete
            max_tokens: Maximum tokens to generate
            grammar: Optional GBNF grammar to constrain output
            **kwargs: Additional generation parameters

        Returns:
            Generation result
        """
        if not self.is_loaded:
            await self.load()

        gen_config = self._build_gen_config(max_tokens=max_tokens, grammar=grammar, **kwargs)

        start = time.time()
        loop = asyncio.get_running_loop()
        result = await loop.run_in_executor(
            None,
            lambda: self._complete_sync(prompt, gen_config),
        )
        elapsed = int((time.time() - start) * 1000)

        choice = result["choices"][0]
        content = choice.get("text", "")

        # Extract logprobs if requested
        raw_logprobs = choice.get("logprobs")
        logprobs_list = None
        if raw_logprobs and raw_logprobs.get("token_logprobs"):
            logprobs_list = [
                {"token": tok, "logprob": lp}
                for tok, lp in zip(
                    raw_logprobs["tokens"], raw_logprobs["token_logprobs"], strict=False
                )
                if lp is not None
            ]

        return GenerationResult(
            content=content,
            tool_calls=[],
            finish_reason=choice.get("finish_reason", "stop"),
            token_count=result.get("usage", {}).get("completion_tokens", 0),
            generation_time_ms=elapsed,
            logprobs=logprobs_list,
        )

    def _complete_sync(self, prompt: str, config: GenerationConfig) -> dict[str, Any]:
        """Synchronous raw text completion."""
        assert self._model is not None

        kwargs: dict[str, Any] = {
            "prompt": prompt,
            "max_tokens": config.max_tokens,
            "temperature": config.temperature,
            "top_p": config.top_p,
            "top_k": config.top_k,
            "repeat_penalty": config.repeat_penalty,
            "logprobs": config.logprobs,
        }

        if config.grammar:
            kwargs["grammar"] = LlamaGrammar.from_string(config.grammar)

        result: dict[str, Any] = self._model.create_completion(**kwargs)
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
        if not self.is_loaded:
            await self.load()

        llama_messages = self._convert_messages(messages)
        self._reset_model_if_needed()

        gen_config = self._build_gen_config(
            max_tokens=max_tokens,
            stop=stop,
            grammar=kwargs.pop("grammar", None),
            stream=True,
            **kwargs,
        )

        # Use a queue to bridge sync stream iteration with async yielding
        queue: asyncio.Queue[str | None] = asyncio.Queue()
        loop = asyncio.get_running_loop()
        cancel = threading.Event()

        # Start stream iteration in background thread
        stream_task = loop.run_in_executor(
            None,
            lambda: self._stream_to_queue(
                llama_messages,
                gen_config,
                queue,
                loop,
                cancel,
            ),
        )

        # Yield chunks from queue asynchronously (doesn't block event loop)
        try:
            while True:
                chunk = await queue.get()
                if chunk is None:
                    break
                yield chunk
        finally:
            cancel.set()
            logger.info("Stream consumer exited, cancel signal sent to background thread")
            try:
                await asyncio.wait_for(stream_task, timeout=30.0)
            except Exception:
                logger.warning("Stream thread did not exit cleanly within 30s after cancel")

    def _reset_model_if_needed(self) -> None:
        """Reset model state before generation to ensure clean KV cache."""
        if self._model is not None and hasattr(self._model, "reset"):
            self._model.reset()

    def _stream_to_queue(
        self,
        messages: list[dict[str, Any]],
        config: GenerationConfig,
        queue: asyncio.Queue[str | None],
        loop: asyncio.AbstractEventLoop,
        cancel: threading.Event,
    ) -> None:
        """Run synchronous stream iteration in thread, pushing chunks to queue."""
        assert self._model is not None
        try:
            grammar_obj = LlamaGrammar.from_string(config.grammar) if config.grammar else None
            stream_kwargs: dict[str, Any] = {
                "messages": messages,
                "max_tokens": config.max_tokens,
                "temperature": config.temperature,
                "top_p": config.top_p,
                "top_k": config.top_k,
                "repeat_penalty": config.repeat_penalty,
                "stop": config.stop if config.stop else None,
                "stream": True,
                "grammar": grammar_obj,
            }
            if config.chat_template_kwargs and self._has_chat_template_kwargs:
                stream_kwargs["chat_template_kwargs"] = config.chat_template_kwargs

            # Per-request no-think handler swap (same as _generate_sync)
            needs_no_think = (
                not self._has_chat_template_kwargs
                and config.chat_template_kwargs
                and config.chat_template_kwargs.get("enable_thinking") is False
                and self._no_think_chat_handler is not None
            )
            if needs_no_think:
                self._model.chat_handler = self._no_think_chat_handler

            try:
                stream = self._model.create_chat_completion(**stream_kwargs)
                self._process_stream_chunks(stream, queue, loop, cancel)
            finally:
                if needs_no_think:
                    self._model.chat_handler = self._default_chat_handler
        except Exception as e:
            logger.exception(f"Stream error: {e}")
        finally:
            try:
                loop.call_soon_threadsafe(queue.put_nowait, None)
            except RuntimeError:
                logger.warning("Event loop closed before stream sentinel could be sent")

    def _process_stream_chunks(
        self,
        stream: Any,
        queue: asyncio.Queue[str | None],
        loop: asyncio.AbstractEventLoop,
        cancel: threading.Event,
    ) -> None:
        """Process stream chunks and push to queue."""
        for chunk in stream:
            if cancel.is_set():
                logger.info("Stream cancelled by consumer, stopping chunk iteration")
                break
            choice = chunk["choices"][0]
            delta = choice.get("delta", {})
            if delta.get("content"):
                try:
                    loop.call_soon_threadsafe(queue.put_nowait, delta["content"])
                except RuntimeError:
                    logger.info("Event loop closed during stream, stopping chunk iteration")
                    break
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
        return int(self.config.context_length)

    @property
    def adapter(self) -> ChatAdapter:
        """Get the adapter for this backend."""
        return self._adapter

    @property
    def last_finish_reason(self) -> str:
        """Get the finish_reason from the last generation (streaming or non-streaming)."""
        return self._last_finish_reason
