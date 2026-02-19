"""
Voice interface subsystem for Entropi.

Provides real-time speech-to-speech conversation using NVIDIA PersonaPlex,
with context compaction between conversation windows.
"""

from entropic.voice.audio_io import AudioIO
from entropic.voice.context_compactor import (
    CompactionResult,
    ContextCompactor,
    ContextPriority,
    ContextSection,
    StructuredPrompt,
)
from entropic.voice.controller import PersonaPlexController, VoiceCallbacks, VoiceState
from entropic.voice.thinking_audio import ThinkingAudioManager

__all__ = [
    "AudioIO",
    "CompactionResult",
    "ContextCompactor",
    "ContextPriority",
    "ContextSection",
    "PersonaPlexController",
    "StructuredPrompt",
    "ThinkingAudioManager",
    "VoiceCallbacks",
    "VoiceState",
]
