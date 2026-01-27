# Implementation 02: Inference Engine

> Model loading, inference, and multi-model orchestration

**Prerequisites:** Implementation 01 complete
**Estimated Time:** 4-6 hours with Claude Code
**Checkpoint:** Can load model and generate response via CLI

---

## Objectives

1. Implement llama-cpp-python backend wrapper
2. Create Qwen-specific chat adapter
3. Build model orchestrator for multi-model management
4. Implement task router with heuristics
5. Add streaming generation support

---

## 1. LlamaCpp Backend

### File: `src/entropi/inference/backend.py`

```python
"""
Abstract backend interface and common utilities.

This module defines the interface that all backends must implement
and provides common utilities for token counting, prompt formatting, etc.
"""
from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from typing import Any, AsyncIterator


@dataclass
class GenerationConfig:
    """Configuration for a single generation."""

    max_tokens: int = 4096
    temperature: float = 0.7
    top_p: float = 0.9
    top_k: int = 40
    repeat_penalty: float = 1.1
    stop: list[str] = field(default_factory=list)
    stream: bool = False


@dataclass
class TokenUsage:
    """Token usage statistics."""

    prompt_tokens: int = 0
    completion_tokens: int = 0

    @property
    def total_tokens(self) -> int:
        """Total tokens used."""
        return self.prompt_tokens + self.completion_tokens


# Re-export from core.base for convenience
from entropi.core.base import (
    GenerationResult,
    Message,
    ModelBackend,
    ToolCall,
)

__all__ = [
    "GenerationConfig",
    "TokenUsage",
    "GenerationResult",
    "Message",
    "ModelBackend",
    "ToolCall",
]
```

### File: `src/entropi/inference/llama_cpp.py`

```python
"""
llama-cpp-python backend implementation.

Wraps llama-cpp-python for GGUF model inference with CUDA support.
"""
import asyncio
import time
from pathlib import Path
from typing import Any, AsyncIterator

from llama_cpp import Llama

from entropi.config.schema import ModelConfig
from entropi.core.base import GenerationResult, Message, ModelBackend, ToolCall
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

        # Parse tool calls from response
        content, tool_calls = self._parse_response(result["choices"][0]["message"]["content"])

        return GenerationResult(
            content=content,
            tool_calls=tool_calls,
            finish_reason=result["choices"][0].get("finish_reason", "stop"),
            token_count=result.get("usage", {}).get("completion_tokens", 0),
            generation_time_ms=elapsed,
        )

    def _generate_sync(
        self,
        messages: list[dict],
        config: GenerationConfig,
    ) -> dict:
        """Synchronous generation."""
        return self._model.create_chat_completion(
            messages=messages,
            max_tokens=config.max_tokens,
            temperature=config.temperature,
            top_p=config.top_p,
            top_k=config.top_k,
            repeat_penalty=config.repeat_penalty,
            stop=config.stop if config.stop else None,
        )

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

        # Create stream generator
        loop = asyncio.get_event_loop()

        def create_stream():
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

        # Yield chunks
        for chunk in stream:
            if chunk["choices"][0]["delta"].get("content"):
                yield chunk["choices"][0]["delta"]["content"]

    def _convert_messages(self, messages: list[Message]) -> list[dict]:
        """Convert Message objects to llama-cpp format."""
        result = []
        for msg in messages:
            converted = {
                "role": msg.role,
                "content": msg.content,
            }
            result.append(converted)
        return result

    def _parse_response(self, content: str) -> tuple[str, list[ToolCall]]:
        """
        Parse response for tool calls.

        Qwen uses a specific format for tool calls that we need to parse.

        Args:
            content: Raw response content

        Returns:
            Tuple of (cleaned content, tool calls)
        """
        # TODO: Implement Qwen tool call parsing in adapter
        # For now, return content as-is
        return content, []

    def count_tokens(self, text: str) -> int:
        """Count tokens in text."""
        if self._model is None:
            # Estimate if model not loaded
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
```

---

## 2. Qwen Chat Adapter

### File: `src/entropi/inference/adapters/base.py`

```python
"""
Base adapter interface for model-specific formatting.

Different models use different chat formats and tool call conventions.
Adapters handle these differences.
"""
from abc import ABC, abstractmethod
from typing import Any

from entropi.core.base import Message, ToolCall


class ChatAdapter(ABC):
    """Abstract base class for chat format adapters."""

    @property
    @abstractmethod
    def chat_format(self) -> str:
        """Get llama-cpp chat format name."""
        pass

    @abstractmethod
    def format_system_prompt(
        self,
        base_prompt: str,
        tools: list[dict[str, Any]] | None = None,
    ) -> str:
        """
        Format system prompt with optional tool definitions.

        Args:
            base_prompt: Base system prompt
            tools: Tool definitions

        Returns:
            Formatted system prompt
        """
        pass

    @abstractmethod
    def parse_tool_calls(self, content: str) -> tuple[str, list[ToolCall]]:
        """
        Parse tool calls from model output.

        Args:
            content: Model output

        Returns:
            Tuple of (cleaned content, tool calls)
        """
        pass

    @abstractmethod
    def format_tool_result(self, tool_call: ToolCall, result: str) -> Message:
        """
        Format tool result for injection into conversation.

        Args:
            tool_call: Original tool call
            result: Tool execution result

        Returns:
            Message to inject
        """
        pass
```

### File: `src/entropi/inference/adapters/qwen.py`

```python
"""
Qwen-specific chat adapter.

Handles Qwen's ChatML format and tool call conventions.
"""
import json
import re
import uuid
from typing import Any

from entropi.core.base import Message, ToolCall
from entropi.inference.adapters.base import ChatAdapter


class QwenAdapter(ChatAdapter):
    """Adapter for Qwen models."""

    # Qwen tool call markers
    TOOL_CALL_START = "<tool_call>"
    TOOL_CALL_END = "</tool_call>"

    @property
    def chat_format(self) -> str:
        """Get llama-cpp chat format name."""
        return "chatml"

    def format_system_prompt(
        self,
        base_prompt: str,
        tools: list[dict[str, Any]] | None = None,
    ) -> str:
        """
        Format system prompt with tool definitions.

        Args:
            base_prompt: Base system prompt
            tools: Tool definitions

        Returns:
            Formatted system prompt
        """
        if not tools:
            return base_prompt

        # Format tools in Qwen's expected format
        tools_text = self._format_tools(tools)

        return f"""{base_prompt}

# Available Tools

You have access to the following tools. To use a tool, respond with a tool call in this format:

{self.TOOL_CALL_START}
{{"name": "tool_name", "arguments": {{"arg1": "value1"}}}}
{self.TOOL_CALL_END}

{tools_text}

After receiving tool results, continue your response or make additional tool calls as needed.
"""

    def _format_tools(self, tools: list[dict[str, Any]]) -> str:
        """Format tool definitions for the prompt."""
        lines = []
        for tool in tools:
            name = tool.get("name", "unknown")
            description = tool.get("description", "No description")
            schema = tool.get("inputSchema", {})

            lines.append(f"## {name}")
            lines.append(f"{description}")

            if schema.get("properties"):
                lines.append("\nParameters:")
                for param, details in schema["properties"].items():
                    param_type = details.get("type", "any")
                    param_desc = details.get("description", "")
                    required = param in schema.get("required", [])
                    req_marker = " (required)" if required else ""
                    lines.append(f"- {param} ({param_type}){req_marker}: {param_desc}")

            lines.append("")

        return "\n".join(lines)

    def parse_tool_calls(self, content: str) -> tuple[str, list[ToolCall]]:
        """
        Parse tool calls from Qwen output.

        Args:
            content: Model output

        Returns:
            Tuple of (cleaned content, tool calls)
        """
        tool_calls = []

        # Find all tool call blocks
        pattern = re.compile(
            rf"{re.escape(self.TOOL_CALL_START)}\s*(.*?)\s*{re.escape(self.TOOL_CALL_END)}",
            re.DOTALL,
        )

        matches = pattern.findall(content)

        for match in matches:
            try:
                # Parse JSON
                data = json.loads(match.strip())
                tool_call = ToolCall(
                    id=str(uuid.uuid4()),
                    name=data.get("name", ""),
                    arguments=data.get("arguments", {}),
                )
                tool_calls.append(tool_call)
            except json.JSONDecodeError:
                # Skip malformed tool calls
                continue

        # Remove tool call blocks from content
        cleaned = pattern.sub("", content).strip()

        return cleaned, tool_calls

    def format_tool_result(self, tool_call: ToolCall, result: str) -> Message:
        """
        Format tool result for conversation.

        Args:
            tool_call: Original tool call
            result: Tool execution result

        Returns:
            Message to inject
        """
        content = f"""<tool_result>
Tool: {tool_call.name}
Call ID: {tool_call.id}
Result:
{result}
</tool_result>"""

        return Message(
            role="tool",
            content=content,
            tool_results=[{
                "call_id": tool_call.id,
                "name": tool_call.name,
                "result": result,
            }],
        )
```

---

## 3. Model Orchestrator

### File: `src/entropi/inference/orchestrator.py`

```python
"""
Model orchestrator for task-specialized routing.

Routes between Qwen3 (reasoning) and Qwen2.5-Coder (code generation)
based on task type, with thinking mode toggle.
"""
import asyncio
from enum import Enum
from typing import Any, AsyncIterator

from entropi.config.schema import EntropyConfig, ModelConfig
from entropi.core.base import GenerationResult, Message, ModelBackend
from entropi.core.logging import get_logger
from entropi.inference.adapters.qwen import QwenAdapter
from entropi.inference.llama_cpp import LlamaCppBackend

logger = get_logger("inference.orchestrator")


class TaskType(Enum):
    """Task types for routing."""

    CODE_GENERATION = "code"     # Routes to Qwen2.5-Coder
    REASONING = "reasoning"      # Routes to Qwen3 (8B or 14B)


class ModelRole(Enum):
    """Model roles in the task-specialized architecture."""

    THINKING = "thinking"   # Qwen3-14B - deep reasoning
    NORMAL = "normal"       # Qwen3-8B - fast reasoning
    CODE = "code"           # Qwen2.5-Coder-7B - code generation
    MICRO = "micro"         # Qwen2.5-Coder-0.5B - routing


class ModelOrchestrator:
    """
    Task-specialized model orchestrator.

    Routes requests between:
    - Qwen3 (8B or 14B based on thinking mode) for reasoning/planning
    - Qwen2.5-Coder-7B for ALL code generation tasks
    - Qwen2.5-Coder-0.5B for routing decisions (always loaded)
    """

    def __init__(self, config: EntropyConfig) -> None:
        """
        Initialize orchestrator.

        Args:
            config: Application configuration
        """
        self.config = config
        self._models: dict[ModelRole, ModelBackend] = {}
        self._adapter = QwenAdapter()
        self._lock = asyncio.Lock()
        self._thinking_enabled = config.thinking.default

    @property
    def thinking_enabled(self) -> bool:
        """Check if thinking mode is enabled."""
        return self._thinking_enabled

    async def set_thinking_mode(self, enabled: bool) -> None:
        """
        Toggle thinking mode.

        When enabled, uses Qwen3-14B for reasoning.
        When disabled, uses Qwen3-8B for reasoning.
        Code generation always uses Qwen2.5-Coder-7B.

        Args:
            enabled: Whether to enable thinking mode
        """
        if enabled == self._thinking_enabled:
            return

        async with self._lock:
            self._thinking_enabled = enabled

            # Swap reasoning models
            if enabled:
                # Unload normal (8B), load thinking (14B)
                if ModelRole.NORMAL in self._models and self._models[ModelRole.NORMAL].is_loaded:
                    await self._models[ModelRole.NORMAL].unload()
                    logger.info("Unloaded normal model (8B)")

                if ModelRole.THINKING in self._models:
                    await self._models[ModelRole.THINKING].load()
                    logger.info("Loaded thinking model (14B)")
            else:
                # Unload thinking (14B), load normal (8B)
                if ModelRole.THINKING in self._models and self._models[ModelRole.THINKING].is_loaded:
                    await self._models[ModelRole.THINKING].unload()
                    logger.info("Unloaded thinking model (14B)")

                if ModelRole.NORMAL in self._models:
                    await self._models[ModelRole.NORMAL].load()
                    logger.info("Loaded normal model (8B)")

    async def initialize(self) -> None:
        """Initialize and load configured models."""
        logger.info("Initializing task-specialized model orchestrator")

        models_config = self.config.models

        # Create backends for configured models
        if models_config.thinking:
            self._models[ModelRole.THINKING] = self._create_backend(models_config.thinking)

        if models_config.normal:
            self._models[ModelRole.NORMAL] = self._create_backend(models_config.normal)

        if models_config.code:
            self._models[ModelRole.CODE] = self._create_backend(models_config.code)

        if models_config.micro:
            self._models[ModelRole.MICRO] = self._create_backend(models_config.micro)

        # Always load micro model for routing
        if ModelRole.MICRO in self._models:
            await self._models[ModelRole.MICRO].load()
            logger.info("Loaded micro model for routing")

        # Always load code model (used for all code generation)
        if ModelRole.CODE in self._models:
            await self._models[ModelRole.CODE].load()
            logger.info("Loaded code model (Qwen2.5-Coder-7B)")

        # Load reasoning model based on thinking mode
        if self._thinking_enabled:
            if ModelRole.THINKING in self._models:
                await self._models[ModelRole.THINKING].load()
                logger.info("Loaded thinking model (Qwen3-14B)")
        else:
            if ModelRole.NORMAL in self._models:
                await self._models[ModelRole.NORMAL].load()
                logger.info("Loaded normal model (Qwen3-8B)")

    def _create_backend(self, model_config: ModelConfig) -> ModelBackend:
        """Create a backend for a model configuration."""
        return LlamaCppBackend(
            config=model_config,
            chat_format=self._adapter.chat_format,
        )

    async def shutdown(self) -> None:
        """Shutdown and unload all models."""
        logger.info("Shutting down model orchestrator")

        for role, model in self._models.items():
            if model.is_loaded:
                await model.unload()
                logger.info(f"Unloaded {role.value} model")

    async def generate(
        self,
        messages: list[Message],
        force_code_model: bool = False,
        **kwargs: Any,
    ) -> GenerationResult:
        """
        Generate a response using the appropriate model.

        Args:
            messages: Conversation messages
            force_code_model: Force use of code model
            **kwargs: Additional generation parameters

        Returns:
            Generation result
        """
        # Determine which model to use
        model = await self._select_model(messages, force_code_model)

        logger.debug(f"Generating with {self._get_model_name(model)} model")
        return await model.generate(messages, **kwargs)

    async def generate_stream(
        self,
        messages: list[Message],
        force_code_model: bool = False,
        **kwargs: Any,
    ) -> AsyncIterator[str]:
        """
        Generate a streaming response.

        Args:
            messages: Conversation messages
            force_code_model: Force use of code model
            **kwargs: Additional generation parameters

        Yields:
            Response chunks
        """
        model = await self._select_model(messages, force_code_model)

        logger.debug(f"Streaming with {self._get_model_name(model)} model")
        async for chunk in model.generate_stream(messages, **kwargs):
            yield chunk

    async def _select_model(
        self,
        messages: list[Message],
        force_code_model: bool = False,
    ) -> ModelBackend:
        """
        Select the appropriate model for the task.

        Args:
            messages: Conversation messages
            force_code_model: Force use of code model

        Returns:
            Model backend to use
        """
        if force_code_model:
            return await self._get_model(ModelRole.CODE)

        # Classify the task
        task_type = await self._classify_task(messages)

        if task_type == TaskType.CODE_GENERATION:
            return await self._get_model(ModelRole.CODE)
        else:
            # Use reasoning model based on thinking mode
            if self._thinking_enabled:
                return await self._get_model(ModelRole.THINKING)
            else:
                return await self._get_model(ModelRole.NORMAL)

    async def _classify_task(self, messages: list[Message]) -> TaskType:
        """
        Classify the task type (code generation vs reasoning).

        Args:
            messages: Conversation messages

        Returns:
            Task type
        """
        if not self.config.routing.enabled:
            return TaskType.REASONING  # Default to reasoning model

        # Get last user message
        user_message = ""
        for msg in reversed(messages):
            if msg.role == "user":
                user_message = msg.content
                break

        # Try heuristic classification first
        heuristic_result = self._classify_heuristic(user_message)
        if heuristic_result is not None:
            return heuristic_result

        # Fall back to model-based classification
        return await self._classify_with_model(user_message)

    def _classify_heuristic(self, message: str) -> TaskType | None:
        """
        Classify task using heuristics (no model call).

        Args:
            message: User message

        Returns:
            Task type or None if heuristics inconclusive
        """
        message_lower = message.lower()
        routing_config = self.config.routing

        # Check for code generation keywords
        for keyword in routing_config.code_generation_keywords:
            if keyword in message_lower:
                return TaskType.CODE_GENERATION

        # Check for reasoning keywords
        for keyword in routing_config.reasoning_keywords:
            if keyword in message_lower:
                return TaskType.REASONING

        # Inconclusive - will fall back to model classification
        return None

    async def _classify_with_model(self, message: str) -> TaskType:
        """
        Classify task using micro model.

        Args:
            message: User message

        Returns:
            Task type
        """
        if ModelRole.MICRO not in self._models:
            return TaskType.REASONING  # Safe default

        classification_prompt = f"""Classify this request into one category:
- CODE: Writing, implementing, fixing, or refactoring code
- REASONING: Planning, explaining, reviewing, or analyzing

Request: {message[:500]}

Respond with only: CODE or REASONING"""

        micro = await self._get_model(ModelRole.MICRO)
        result = await micro.generate(
            [Message(role="user", content=classification_prompt)],
            max_tokens=10,
            temperature=0.0,
        )

        response = result.content.strip().upper()

        if "CODE" in response:
            return TaskType.CODE_GENERATION
        else:
            return TaskType.REASONING

    async def _get_model(self, role: ModelRole) -> ModelBackend:
        """
        Get a model by role, loading if necessary.

        Args:
            role: Model role

        Returns:
            Model backend

        Raises:
            ValueError: If role not configured
        """
        if role not in self._models:
            raise ValueError(f"No model configured for role: {role.value}")

        model = self._models[role]
        if not model.is_loaded:
            await model.load()

        return model

    def _get_model_name(self, model: ModelBackend) -> str:
        """Get human-readable name for a model."""
        for role, m in self._models.items():
            if m is model:
                return role.value
        return "unknown"

    def get_loaded_models(self) -> list[str]:
        """Get list of currently loaded models."""
        return [role.value for role, model in self._models.items() if model.is_loaded]

    def get_status(self) -> dict[str, Any]:
        """Get orchestrator status."""
        return {
            "thinking_enabled": self._thinking_enabled,
            "loaded_models": self.get_loaded_models(),
            "routing_enabled": self.config.routing.enabled,
        }
```

---

## 5. Task-Aware Router (Separate Module)

### File: `src/entropi/inference/router.py`

```python
"""
Task-aware router for model selection.

Provides utilities for classifying tasks and selecting models.
"""
import re
from enum import Enum

from entropi.config.schema import RoutingConfig
from entropi.core.logging import get_logger

logger = get_logger("inference.router")


class TaskType(Enum):
    """Task types for routing."""

    CODE_GENERATION = "code"
    REASONING = "reasoning"


def classify_task_heuristic(
    message: str,
    config: RoutingConfig,
) -> TaskType | None:
    """
    Classify a task using heuristics.

    Args:
        message: User message
        config: Routing configuration

    Returns:
        Task type or None if inconclusive
    """
    message_lower = message.lower()

    # Check for code generation patterns
    code_patterns = [
        r"\bwrite\s+(a\s+)?function\b",
        r"\bimplement\s+",
        r"\bcreate\s+(a\s+)?(class|function|method)\b",
        r"\bfix\s+(this\s+)?(bug|error|issue)\b",
        r"\brefactor\b",
        r"\badd\s+test",
        r"\bgenerate\s+code\b",
        r"```",  # Code block in message
    ]

    for pattern in code_patterns:
        if re.search(pattern, message_lower):
            return TaskType.CODE_GENERATION

    # Check for reasoning patterns
    reasoning_patterns = [
        r"\bexplain\s+(how|why|what)\b",
        r"\bplan\s+(how|to)\b",
        r"\bhow\s+should\b",
        r"\bwhat\s+approach\b",
        r"\breview\s+(this|my)\b",
        r"\banalyze\b",
        r"\bcompare\b",
    ]

    for pattern in reasoning_patterns:
        if re.search(pattern, message_lower):
            return TaskType.REASONING

    # Check keyword lists from config
    for keyword in config.code_generation_keywords:
        if keyword.lower() in message_lower:
            return TaskType.CODE_GENERATION

    for keyword in config.reasoning_keywords:
        if keyword.lower() in message_lower:
            return TaskType.REASONING

    return None


def is_code_generation_task(message: str) -> bool:
    """
    Quick check if message is likely a code generation task.

    Args:
        message: User message

    Returns:
        True if likely code generation
    """
    code_indicators = [
        "write",
        "implement",
        "create function",
        "create class",
        "fix bug",
        "fix this",
        "refactor",
        "add test",
        "generate code",
        "code for",
        "```",
    ]

    message_lower = message.lower()
    return any(indicator in message_lower for indicator in code_indicators)
```

    @property
    def adapter(self) -> QwenAdapter:
        """Get the chat adapter."""
        return self._adapter

    def get_loaded_models(self) -> list[str]:
        """Get list of currently loaded model tiers."""
        return [tier.value for tier, model in self._models.items() if model.is_loaded]

    def count_tokens(self, text: str, tier: ModelTier | None = None) -> int:
        """
        Count tokens in text.

        Args:
            text: Text to count
            tier: Model tier to use for counting

        Returns:
            Token count
        """
        if tier is None:
            tier = ModelTier(self.config.models.default)

        if tier in self._models and self._models[tier].is_loaded:
            return self._models[tier].count_tokens(text)

        # Estimate if no model loaded
        return len(text) // 4
```

---

## 4. Unit Tests

### File: `tests/unit/test_inference.py`

```python
"""Tests for inference engine."""
import pytest

from entropi.core.base import Message, ToolCall
from entropi.inference.adapters.qwen import QwenAdapter


class TestQwenAdapter:
    """Tests for Qwen adapter."""

    def setup_method(self) -> None:
        """Set up test fixtures."""
        self.adapter = QwenAdapter()

    def test_chat_format(self) -> None:
        """Test chat format is chatml."""
        assert self.adapter.chat_format == "chatml"

    def test_format_system_prompt_no_tools(self) -> None:
        """Test formatting system prompt without tools."""
        result = self.adapter.format_system_prompt("You are helpful.")
        assert result == "You are helpful."

    def test_format_system_prompt_with_tools(self) -> None:
        """Test formatting system prompt with tools."""
        tools = [
            {
                "name": "read_file",
                "description": "Read a file",
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "path": {"type": "string", "description": "File path"},
                    },
                    "required": ["path"],
                },
            }
        ]
        result = self.adapter.format_system_prompt("You are helpful.", tools)

        assert "read_file" in result
        assert "Read a file" in result
        assert "path" in result
        assert "<tool_call>" in result

    def test_parse_tool_calls_single(self) -> None:
        """Test parsing single tool call."""
        content = """I'll read that file for you.

<tool_call>
{"name": "read_file", "arguments": {"path": "test.py"}}
</tool_call>"""

        cleaned, tool_calls = self.adapter.parse_tool_calls(content)

        assert "I'll read that file for you." in cleaned
        assert "<tool_call>" not in cleaned
        assert len(tool_calls) == 1
        assert tool_calls[0].name == "read_file"
        assert tool_calls[0].arguments == {"path": "test.py"}

    def test_parse_tool_calls_multiple(self) -> None:
        """Test parsing multiple tool calls."""
        content = """<tool_call>
{"name": "read_file", "arguments": {"path": "a.py"}}
</tool_call>

<tool_call>
{"name": "read_file", "arguments": {"path": "b.py"}}
</tool_call>"""

        _, tool_calls = self.adapter.parse_tool_calls(content)
        assert len(tool_calls) == 2

    def test_parse_tool_calls_malformed(self) -> None:
        """Test parsing malformed tool calls."""
        content = """<tool_call>
not valid json
</tool_call>"""

        _, tool_calls = self.adapter.parse_tool_calls(content)
        assert len(tool_calls) == 0

    def test_format_tool_result(self) -> None:
        """Test formatting tool result."""
        tool_call = ToolCall(
            id="123",
            name="read_file",
            arguments={"path": "test.py"},
        )

        message = self.adapter.format_tool_result(tool_call, "file contents")

        assert message.role == "tool"
        assert "read_file" in message.content
        assert "file contents" in message.content
        assert message.tool_results[0]["call_id"] == "123"
```

---

## 5. Integration with CLI

Update `src/entropi/app.py` to use the orchestrator:

```python
"""
Application orchestrator - updated for inference support.
"""
from pathlib import Path

from rich.console import Console

from entropi.config.schema import EntropyConfig
from entropi.core.base import Message
from entropi.core.logging import get_logger
from entropi.inference.orchestrator import ModelOrchestrator


class Application:
    """Main application orchestrator."""

    def __init__(
        self,
        config: EntropyConfig,
        project_dir: Path | None = None,
    ) -> None:
        """Initialize application."""
        self.config = config
        self.project_dir = project_dir or Path.cwd()
        self.logger = get_logger("app")
        self.console = Console()

        # Components
        self._orchestrator: ModelOrchestrator | None = None

    async def initialize(self) -> None:
        """Initialize all components."""
        self.logger.info("Initializing Entropi...")

        # Initialize model orchestrator
        self._orchestrator = ModelOrchestrator(self.config)
        await self._orchestrator.initialize()

        self.logger.info("Entropi initialized")

    async def shutdown(self) -> None:
        """Shutdown all components."""
        self.logger.info("Shutting down...")

        if self._orchestrator:
            await self._orchestrator.shutdown()

        self.logger.info("Shutdown complete")

    async def run(self) -> None:
        """Run the interactive application."""
        try:
            await self.initialize()

            self.console.print("[bold green]Entropi[/bold green] initialized!")
            self.console.print(f"Loaded models: {self._orchestrator.get_loaded_models()}")
            self.console.print("\n[yellow]Interactive mode not yet implemented.[/yellow]")

        except KeyboardInterrupt:
            self.console.print("\n[yellow]Interrupted[/yellow]")
        finally:
            await self.shutdown()

    async def single_turn(self, message: str, stream: bool = True) -> None:
        """Process a single message."""
        try:
            await self.initialize()

            self.console.print(f"[dim]You: {message}[/dim]\n")

            messages = [Message(role="user", content=message)]

            if stream:
                self.console.print("[bold]Assistant:[/bold] ", end="")
                async for chunk in self._orchestrator.generate_stream(messages):
                    self.console.print(chunk, end="")
                self.console.print()
            else:
                result = await self._orchestrator.generate(messages)
                self.console.print(f"[bold]Assistant:[/bold] {result.content}")
                self.console.print(f"\n[dim]Tokens: {result.token_count}, Time: {result.generation_time_ms}ms[/dim]")

        finally:
            await self.shutdown()
```

---

## Checkpoint: Verification

After implementing this phase:

```bash
cd ~/projects/entropi
source ~/.venvs/entropi/bin/activate

# Run tests
pytest tests/unit/test_inference.py -v

# Test single turn generation
entropi ask "What is a fibonacci sequence?"

# Test with streaming
entropi ask "Write a hello world function in Python"

# Test status
entropi status
```

**Success Criteria:**
- [ ] Adapter tests pass
- [ ] `entropi ask` generates responses
- [ ] Streaming output works
- [ ] Model loads without errors
- [ ] Token count displayed

---

## Next Phase

Proceed to **Implementation 03: MCP Client** to implement tool integration.
