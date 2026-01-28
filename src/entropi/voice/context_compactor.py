"""
Context compaction for voice conversation windows.

Uses a secondary LLM to summarize conversation history between
PersonaPlex windows, maintaining context within memory constraints.

The prompt injection uses a structured XML format with priority levels
to ensure important context (especially safety/ethics) takes precedence.
"""

from __future__ import annotations

import asyncio
import hashlib
import json
from dataclasses import dataclass, field
from enum import IntEnum
from typing import TYPE_CHECKING

from entropi.core.logging import get_logger

if TYPE_CHECKING:
    from entropi.config.schema import VoiceConfig
    from entropi.inference.llama_cpp import LlamaCppBackend

logger = get_logger("voice.context_compactor")


class ContextPriority(IntEnum):
    """
    Priority levels for context sections.

    Higher priority sections are preserved when context must be truncated.
    CRITICAL sections are never removed.
    """

    CRITICAL = 100  # Safety/ethics - never truncated
    HIGH = 75  # Core task instructions
    MEDIUM = 50  # Recent conversation context
    LOW = 25  # Historical context, nice-to-have details


@dataclass
class ContextSection:
    """A section of context with priority and change tracking."""

    id: str
    priority: ContextPriority
    content: str
    version: int = 1
    checksum: str = ""

    def __post_init__(self) -> None:
        """Compute checksum after init."""
        self.checksum = hashlib.md5(self.content.encode()).hexdigest()[:8]

    def has_changed(self, previous_checksum: str) -> bool:
        """Check if content has changed from previous version."""
        return self.checksum != previous_checksum


@dataclass
class StructuredPrompt:
    """
    Structured prompt with prioritized sections and delta tracking.

    Format injected into voice model:
    ```xml
    <context version="3">
      <section id="system" priority="critical" changed="false">
        Core safety and ethics instructions...
      </section>
      <section id="task" priority="high" changed="true" delta="added coding task">
        Current task description...
      </section>
      <section id="history" priority="medium" changed="true">
        Summarized conversation history...
      </section>
    </context>
    ```
    """

    sections: dict[str, ContextSection] = field(default_factory=dict)
    version: int = 1
    _previous_checksums: dict[str, str] = field(default_factory=dict)

    def add_section(
        self,
        id: str,
        content: str,
        priority: ContextPriority = ContextPriority.MEDIUM,
    ) -> None:
        """
        Add or update a context section.

        Args:
            id: Section identifier
            content: Section content
            priority: Section priority level
        """
        # Track previous checksum for delta detection
        if id in self.sections:
            self._previous_checksums[id] = self.sections[id].checksum

        existing = self.sections.get(id)
        new_version = (existing.version + 1) if existing else 1

        self.sections[id] = ContextSection(
            id=id,
            priority=priority,
            content=content,
            version=new_version,
        )

    def remove_section(self, id: str) -> None:
        """Remove a context section."""
        if id in self.sections:
            self._previous_checksums[id] = self.sections[id].checksum
            del self.sections[id]

    def bump_version(self) -> None:
        """Increment prompt version after changes."""
        self.version += 1

    def to_xml(self, include_deltas: bool = True) -> str:
        """
        Serialize to XML format for injection.

        Args:
            include_deltas: Whether to include change indicators

        Returns:
            XML-formatted context string
        """
        lines = [f'<context version="{self.version}">']

        # Sort by priority (highest first) then by id
        sorted_sections = sorted(
            self.sections.values(),
            key=lambda s: (-s.priority, s.id),
        )

        for section in sorted_sections:
            priority_name = section.priority.name.lower()
            prev_checksum = self._previous_checksums.get(section.id, "")
            changed = section.has_changed(prev_checksum) if prev_checksum else True

            attrs = f'id="{section.id}" priority="{priority_name}"'
            if include_deltas:
                attrs += f' changed="{str(changed).lower()}"'
                attrs += f' v="{section.version}"'

            lines.append(f"  <section {attrs}>")
            # Indent content
            for line in section.content.split("\n"):
                lines.append(f"    {line}")
            lines.append("  </section>")

        lines.append("</context>")
        return "\n".join(lines)

    def to_json(self, include_deltas: bool = True) -> str:
        """
        Serialize to JSON format for injection.

        Args:
            include_deltas: Whether to include change indicators

        Returns:
            JSON-formatted context string
        """
        sections_data: list[dict[str, str | bool | int]] = []

        for section in sorted(
            self.sections.values(),
            key=lambda s: (-s.priority, s.id),
        ):
            prev_checksum = self._previous_checksums.get(section.id, "")
            changed = section.has_changed(prev_checksum) if prev_checksum else True

            section_data: dict[str, str | bool | int] = {
                "id": section.id,
                "priority": section.priority.name.lower(),
                "content": section.content,
            }
            if include_deltas:
                section_data["changed"] = changed
                section_data["version"] = section.version

            sections_data.append(section_data)

        return json.dumps(
            {"version": self.version, "sections": sections_data},
            indent=2,
        )

    def get_total_length(self) -> int:
        """Get total character length of all sections."""
        return sum(len(s.content) for s in self.sections.values())

    def truncate_to_budget(self, max_chars: int) -> None:
        """
        Truncate lower-priority sections to fit budget.

        CRITICAL sections are never truncated.

        Args:
            max_chars: Maximum total character budget
        """
        current_length = self.get_total_length()
        if current_length <= max_chars:
            return

        # Sort by priority (lowest first for removal)
        by_priority = sorted(
            self.sections.values(),
            key=lambda s: (s.priority, -len(s.content)),
        )

        for section in by_priority:
            if section.priority >= ContextPriority.CRITICAL:
                # Never remove critical sections
                continue

            if current_length <= max_chars:
                break

            # Remove this section
            removed_length = len(section.content)
            del self.sections[section.id]
            current_length -= removed_length
            logger.debug(f"Truncated section '{section.id}' ({removed_length} chars)")


# Prompt template for summarization
COMPACTION_PROMPT = """<|im_start|>system
You are a conversation summarizer. Create a brief, information-dense summary of the conversation below. Focus on:
1. Key topics discussed
2. Important decisions or conclusions
3. Action items or next steps
4. Technical details that may be needed for continuity

Keep the summary concise (2-4 sentences) but preserve essential context.
<|im_end|>
<|im_start|>user
Summarize this conversation:

{transcript}
<|im_end|>
<|im_start|>assistant
"""

# Default system section content (CRITICAL priority)
DEFAULT_SYSTEM_PROMPT = """You are a helpful, harmless, and honest AI coding assistant.

Core principles:
- Always prioritize user safety and well-being
- Never assist with harmful, illegal, or unethical activities
- Be truthful and acknowledge limitations
- Respect privacy and confidentiality
- Follow secure coding practices"""


@dataclass
class CompactionResult:
    """Result of context compaction."""

    summary: str
    original_length: int
    summary_length: int
    processing_time_ms: float
    structured_prompt: StructuredPrompt | None = None


class ContextCompactor:
    """
    Compacts conversation context using a secondary LLM.

    Designed to run during the "thinking moment" between PersonaPlex
    conversation windows, using a small, fast model (e.g., Qwen3-0.6B).

    Uses structured prompts with priority levels to ensure:
    1. Safety/ethics context is never lost (CRITICAL)
    2. Task context takes precedence over history (HIGH)
    3. Recent conversation is preserved when possible (MEDIUM)
    4. Older context can be summarized/dropped (LOW)
    """

    def __init__(
        self,
        config: VoiceConfig,
        inference: LlamaCppBackend | None = None,
    ) -> None:
        """
        Initialize context compactor.

        Args:
            config: Voice configuration
            inference: Optional shared inference instance
        """
        self._config = config
        self._inference = inference
        self._model_loaded = False
        self._accumulated_context: list[str] = []
        self._structured_prompt = StructuredPrompt()
        self._lock = asyncio.Lock()

        # Initialize with default system prompt
        self._structured_prompt.add_section(
            id="system",
            content=DEFAULT_SYSTEM_PROMPT,
            priority=ContextPriority.CRITICAL,
        )

    async def initialize(self) -> None:
        """Initialize the context compactor.

        Note: Secondary model support is planned but not yet implemented.
        Currently uses simple truncation for context compaction.
        """
        # TODO: Implement secondary model loading when API is stabilized
        # For now, use simple truncation fallback
        logger.info("Context compactor initialized (using truncation fallback)")

    def set_task_context(self, task_description: str) -> None:
        """
        Set the current task context (HIGH priority).

        Args:
            task_description: Description of the current coding task
        """
        self._structured_prompt.add_section(
            id="task",
            content=task_description,
            priority=ContextPriority.HIGH,
        )
        self._structured_prompt.bump_version()

    def set_system_context(self, system_prompt: str) -> None:
        """
        Update the system context (CRITICAL priority).

        Args:
            system_prompt: System instructions including safety guidelines
        """
        full_prompt = DEFAULT_SYSTEM_PROMPT + "\n\n" + system_prompt
        self._structured_prompt.add_section(
            id="system",
            content=full_prompt,
            priority=ContextPriority.CRITICAL,
        )
        self._structured_prompt.bump_version()

    async def compact(
        self,
        transcript: str,
        previous_summary: str | None = None,
    ) -> CompactionResult:
        """
        Compact a conversation transcript.

        Args:
            transcript: Recent conversation transcript
            previous_summary: Optional summary from previous windows

        Returns:
            Compaction result with summary and metrics
        """
        import time

        start_time = time.perf_counter()

        # Build context with previous summary if available
        if previous_summary:
            context = f"Previous context: {previous_summary}\n\nRecent conversation:\n{transcript}"
        else:
            context = transcript

        # Generate summary using secondary model
        # TODO: Use secondary LLM for proper summarization when API is ready
        # For now, use smart truncation
        summary = self._simple_truncate(context, max_chars=800)

        elapsed = (time.perf_counter() - start_time) * 1000

        # Update structured prompt with new summary
        self._structured_prompt.add_section(
            id="history",
            content=summary,
            priority=ContextPriority.MEDIUM,
        )
        self._structured_prompt.bump_version()

        logger.debug(
            f"Compacted {len(transcript)} chars -> {len(summary)} chars "
            f"in {elapsed:.0f}ms"
        )

        return CompactionResult(
            summary=summary,
            original_length=len(transcript),
            summary_length=len(summary),
            processing_time_ms=elapsed,
            structured_prompt=self._structured_prompt,
        )

    def _simple_truncate(self, text: str, max_chars: int = 500) -> str:
        """Simple truncation fallback when model unavailable."""
        if len(text) <= max_chars:
            return text

        # Take beginning and end
        half = max_chars // 2
        return f"{text[:half]}... [truncated] ...{text[-half:]}"

    def add_to_context(self, text: str) -> None:
        """
        Add text to accumulated context.

        Args:
            text: Text to add (typically a transcript segment)
        """
        self._accumulated_context.append(text)

    def get_accumulated_context(self) -> str:
        """Get all accumulated context as a single string."""
        return "\n".join(self._accumulated_context)

    def clear_context(self) -> None:
        """Clear accumulated context."""
        self._accumulated_context.clear()

    async def build_injection_prompt(
        self,
        summary: str,
        initial_prompt: str,
        format: str = "xml",
        max_chars: int | None = None,
    ) -> str:
        """
        Build the context injection prompt for PersonaPlex.

        Args:
            summary: Compacted conversation summary
            initial_prompt: Initial system prompt (added to task section)
            format: Output format ("xml", "json", or "plain")
            max_chars: Optional character budget (truncates low-priority sections)

        Returns:
            Formatted prompt for injection into PersonaPlex
        """
        # Update task context with initial prompt
        if initial_prompt:
            self._structured_prompt.add_section(
                id="task",
                content=initial_prompt,
                priority=ContextPriority.HIGH,
            )

        # Update history with summary
        if summary:
            self._structured_prompt.add_section(
                id="history",
                content=summary,
                priority=ContextPriority.MEDIUM,
            )

        # Apply budget if specified
        if max_chars:
            self._structured_prompt.truncate_to_budget(max_chars)

        self._structured_prompt.bump_version()

        # Format output
        if format == "xml":
            return self._structured_prompt.to_xml()
        elif format == "json":
            return self._structured_prompt.to_json()
        else:
            # Plain text fallback
            if not summary:
                return initial_prompt

            return f"""{initial_prompt}

Context from previous conversation:
{summary}

Continue assisting based on this context."""

    def get_structured_prompt(self) -> StructuredPrompt:
        """Get the current structured prompt."""
        return self._structured_prompt

    async def cleanup(self) -> None:
        """Clean up resources."""
        async with self._lock:
            if self._inference is not None and self._model_loaded:
                try:
                    await self._inference.unload()
                except Exception as e:
                    logger.warning(f"Error unloading compaction model: {e}")
            self._model_loaded = False
            self._inference = None
            self._accumulated_context.clear()
