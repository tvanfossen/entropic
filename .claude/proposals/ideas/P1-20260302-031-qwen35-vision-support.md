---
version: 1.0.0
type: proposal
schema_version: 1
id: P1-20260302-031
title: "Qwen3.5 vision support via llama-cpp-python multimodal pipeline"
priority: P1
component: inference
author: tvanfossen
author_email: vanfosst@gmail.com
created: 2026-03-02
updated: 2026-03-02
tags: [vision, multimodal, qwen35, llama-cpp, mmproj, inference]
completed_date: null
scoped_files:
  - "src/entropic/core/base.py"
  - "src/entropic/inference/llama_cpp.py"
  - "src/entropic/inference/adapters/base.py"
  - "src/entropic/inference/adapters/qwen35.py"
  - "src/entropic/inference/orchestrator.py"
  - "src/entropic/config/schema.py"
  - "src/entropic/core/engine.py"
depends_on: []
blocks: []
supersedes: []
---

# Qwen3.5 Vision Support via llama-cpp-python Multimodal Pipeline

## Problem Statement

Qwen3.5-35B-A3B is natively multimodal — vision was baked in via early fusion during
pretraining (OCRBench 91.0, ScreenSpot Pro 68.6, VideoMME 86.6). llama.cpp has already
landed support for this, including mmproj-F16.gguf files and bug fixes for the early
fusion architecture (PR #19930).

Entropic cannot use any of this. The engine is text-only at every layer:

```
Message.content: str                    ← no image content parts
_convert_messages: copies str verbatim  ← no content array support
LlamaCppBackend: no chat_handler        ← no vision handler loaded
ModelConfig: no mmproj_path             ← no config for vision projector
Adapters: text-only parsing             ← no image-aware formatting
```

## Upstream Status (as of 2026-03-02)

| Component | Status |
|-----------|--------|
| llama.cpp Qwen3.5 vision | **Working** — in-tree, bug fixes landing |
| mmproj-F16.gguf | **Exists** — confirmed in issue #19929 |
| llama-cpp-python `Llava15ChatHandler` | **Exists** — handles mmproj loading + image encoding |
| llama-cpp-python `mtmd_cpp.py` | **Exists** — ctypes bindings to libmtmd (multimodal) |
| OpenAI-format content arrays | **Supported** — `create_chat_completion` accepts `image_url` parts |
| KV prefix caching for Qwen3.5 | **Broken** — issue #19858, performance impact only |
| CUDA perf regression | **Open** — issue #19894, 35% slower than Qwen3-30B-A3B |

### Remaining upstream risk

- llama-cpp-python may not yet expose a Qwen3.5-specific chat handler. The generic
  `Llava15ChatHandler` works for LLaVA-family models. Testing needed to confirm it
  handles Qwen3.5's early fusion mmproj correctly.
- VRAM headroom: mmproj-F16 adds overhead on top of Q2_K's ~12GB. Whether vision
  fits in 16GB with usable context is unverified.

## Design

### Content model: multimodal messages

Widen `Message.content` to support OpenAI-format content arrays alongside plain strings.
This is the format llama-cpp-python already accepts.

```python
# Content part types (new)
@dataclass
class TextContent:
    type: Literal["text"] = "text"
    text: str = ""

@dataclass
class ImageContent:
    type: Literal["image_url"] = "image_url"
    image_url: str = ""  # URL, data URI, or local file path

ContentPart = TextContent | ImageContent

# Widened Message
@dataclass
class Message:
    role: str
    content: str | list[ContentPart]  # str for text-only, list for multimodal
    tool_calls: list[dict[str, Any]] = field(default_factory=list)
    tool_results: list[dict[str, Any]] = field(default_factory=list)
    metadata: dict[str, Any] = field(default_factory=dict)

    @property
    def text(self) -> str:
        """Extract text content regardless of format."""
        if isinstance(self.content, str):
            return self.content
        return " ".join(p.text for p in self.content if isinstance(p, TextContent))
```

**Backward compatibility:** Every existing callsite uses `content` as `str`. The `text`
property provides a migration path, but all existing code continues to work because
text-only messages still use plain strings. Only consumers that construct multimodal
messages use the list form.

### Config: mmproj_path on ModelConfig

```python
class ModelConfig(BaseModel):
    path: ExpandedPath
    mmproj_path: OptionalExpandedPath = None  # Vision projector GGUF
    adapter: str = "qwen2"
    # ... rest unchanged
```

When `mmproj_path` is set, the backend loads the vision handler. When absent (default),
behavior is identical to today — text-only inference.

Consumer config:

```yaml
models:
  tiers:
    normal:
      path: ~/models/gguf/Qwen3.5-35B-A3B-Q2_K.gguf
      mmproj_path: ~/models/gguf/Qwen3.5-35B-A3B-mmproj-F16.gguf
      adapter: qwen35
      gpu_layers: 40
```

### Backend: vision handler loading

In `LlamaCppBackend._load_model_sync`, conditionally construct a chat handler:

```python
def _load_model_sync(self) -> Llama:
    chat_handler = None
    if self.config.mmproj_path:
        from llama_cpp.llama_chat_format import Llava15ChatHandler
        chat_handler = Llava15ChatHandler(
            clip_model_path=str(self.config.mmproj_path)
        )
        logger.info(f"Vision handler loaded: {self.config.mmproj_path}")

    model = Llama(
        model_path=str(self.config.path),
        n_ctx=self.config.context_length,
        n_gpu_layers=self.config.gpu_layers,
        chat_handler=chat_handler,      # None = text-only (current behavior)
        chat_format=self._adapter.chat_format if not chat_handler else None,
        use_mlock=True,
        verbose=False,
    )
    return model
```

When `chat_handler` is provided, llama-cpp-python uses it instead of the `chat_format`
string. The handler manages image encoding, token interleaving, and generation internally.

### Message conversion: pass content arrays through

```python
def _convert_messages(self, messages: list[Message]) -> list[dict[str, Any]]:
    result = []
    for msg in messages:
        if isinstance(msg.content, str):
            result.append({"role": msg.role, "content": msg.content})
        else:
            # Multimodal: convert to OpenAI-format content array
            parts = []
            for part in msg.content:
                if isinstance(part, TextContent):
                    parts.append({"type": "text", "text": part.text})
                elif isinstance(part, ImageContent):
                    parts.append({
                        "type": "image_url",
                        "image_url": {"url": part.image_url},
                    })
            result.append({"role": msg.role, "content": parts})
    return result
```

### Adapter awareness

Adapters that parse tool calls from content need to handle the case where content
is a list. The `_clean_content` and `parse_tool_calls` methods operate on strings.
Two options:

1. **Extract text before parsing** — adapters call `msg.text` to get the string
   portion, parse tool calls from that. Images are not tool calls.
2. **No adapter changes** — by the time the adapter sees the response, it's always
   a string (model output is text). Multimodal content only appears in *input*
   messages, never in assistant responses.

**Option 2 is correct.** The model's output is always text. Only user messages contain
image parts. Adapters parse assistant output, so they never encounter content arrays.
No adapter changes needed.

### Engine: consumer-facing API

`engine.run()` currently accepts `prompt: str`. For vision, consumers pass multimodal
messages directly:

```python
# Text-only (existing API, unchanged)
async for msg in engine.run("explain this code"):
    pass

# Multimodal (new overload or new method)
from entropic.core.base import Message, ImageContent, TextContent

msg = Message(
    role="user",
    content=[
        ImageContent(image_url="file:///path/to/screenshot.png"),
        TextContent(text="What's in this image?"),
    ],
)
async for response in engine.run_message(msg):
    pass
```

The `run()` string API remains the default. `run_message()` (or a `Message` overload)
handles the multimodal case. This keeps the simple path simple.

## VRAM Budget

| Component | Estimated VRAM |
|-----------|---------------|
| Qwen3.5-35B-A3B Q2_K | ~12 GB |
| mmproj-F16 | ~0.5-1.5 GB (estimated, needs measurement) |
| KV cache (4K context) | ~0.5-1 GB |
| **Total** | **~13-14.5 GB** |
| Available (RTX PRO 4000) | 16 GB |
| **Headroom** | **~1.5-3 GB** |

This is tight but likely viable. The mmproj overhead needs empirical measurement.
If it doesn't fit, options:

1. Reduce context length when vision is active
2. Use a lower quantization mmproj (if available)
3. Reduce gpu_layers to partially offload to CPU

## Scope Boundaries

### In scope

- `Message.content` widening to `str | list[ContentPart]`
- `mmproj_path` config field on `ModelConfig`
- Vision handler loading in `LlamaCppBackend`
- `_convert_messages` multimodal support
- `Message.text` convenience property
- `engine.run_message()` or `run()` overload for `Message` input
- Unit tests for content model, message conversion, config validation
- Integration test with real mmproj (model test)

### Out of scope

- Video input (Qwen3.5 supports it, but scope creep — separate proposal)
- Configurable backend per tier (separate proposal, additive)
- Image generation / editing (Qwen3.5 is vision-to-text, not text-to-image)
- MCP tool integration for image capture / screenshot
- Streaming image chunks (images are input, not output)
- Base64 encoding helpers (consumer responsibility)

### NOT modified

| File | Why |
|------|-----|
| Adapters (qwen35, qwen3, etc.) | Model output is always text — adapters never see image parts |
| Router / classification | Router processes text prompts, not images |
| Compaction manager | Compaction operates on text content; images can be dropped or preserved via metadata |
| TUI | Presentation layer handles text; image display is a TUI concern, not engine |

## Verification

1. **Unit**: `Message` with `str` content works identically to today
2. **Unit**: `Message` with `list[ContentPart]` roundtrips through `_convert_messages`
3. **Unit**: `Message.text` extracts text from mixed content
4. **Unit**: `mmproj_path` validates (exists, is file, expands ~)
5. **Unit**: `LlamaCppBackend` without `mmproj_path` behaves identically to today
6. **Integration**: Load model with mmproj, send image + text message, get response
7. **VRAM**: Measure actual VRAM with mmproj loaded vs text-only (Q2_K baseline)
8. **Backward compat**: All existing tests pass without modification

## Risks

| Risk | Likelihood | Mitigation |
|------|-----------|------------|
| `Llava15ChatHandler` doesn't work for Qwen3.5 early fusion | Medium | Test with actual mmproj. May need a Qwen3.5-specific handler — check if llama-cpp-python has one. |
| mmproj + Q2_K exceeds 16GB | Low-medium | Measure empirically. Reduce context or gpu_layers as fallback. |
| llama-cpp-python PyPI build lacks mtmd/mmproj support | Medium | `install.sh` / `setup-cuda` builds from source with full support. PyPI users get text-only (existing behavior). |
| `Message.content` type widening breaks downstream consumers | Low | Plain `str` still works. Only new code uses `list[ContentPart]`. `text` property for migration. |
| Performance regression from content type checking | Negligible | Single `isinstance` check per message. |

## Implementation Order

```
Phase 1: Content model + config (no behavioral change)
  ├── ContentPart types in core/base.py
  ├── Message.content widening + .text property
  ├── mmproj_path on ModelConfig
  ├── _convert_messages multimodal path
  └── Unit tests

Phase 2: Backend vision loading
  ├── chat_handler construction in _load_model_sync
  ├── chat_format vs chat_handler dispatch
  └── Unit tests (mocked handler)

Phase 3: Engine API
  ├── run_message() or run() overload
  └── Integration test

Phase 4: Empirical validation
  ├── VRAM measurement with mmproj
  ├── Model test with real image input
  └── Handler compatibility verification
```

Phase 1 ships independently with zero behavioral change — all existing code paths
are unaffected. Phase 2-3 can ship together. Phase 4 is validation, not code.

## Open Questions

- **@architect: QUESTION** — Does `Llava15ChatHandler` handle Qwen3.5's early fusion
  mmproj, or is a new handler needed? Needs empirical testing with the actual file.
- **@architect: QUESTION** — Should `run()` accept `str | Message` directly (simpler
  API, one method) or should `run_message()` be a separate method (clearer separation)?
- **@architect: QUESTION** — When vision is active and VRAM is tight, should the engine
  auto-reduce context length, or should the consumer handle this via config?
