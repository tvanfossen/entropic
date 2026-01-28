# PersonaPlex Integration Guide for Entropi

This document provides everything needed to integrate PersonaPlex as a library within a control loop architecture. PersonaPlex is a real-time speech-to-speech AI model that processes audio at 12.5 fps and generates both text and audio responses.

## Target Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                     ENTROPI TUI (Control Loop)                  │
├─────────────────────────────────────────────────────────────────┤
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐      │
│  │  PersonaPlex │───▶│  "Thinking"  │───▶│   Local LLM  │      │
│  │  (10-30s)    │    │  Audio Clip  │    │  Inference   │      │
│  └──────────────┘    └──────────────┘    └──────────────┘      │
│         │                   │                   │               │
│         ▼                   ▼                   ▼               │
│  [Audio I/O]         [<2s pause]         [Summarize +          │
│  [Conversation]      [Same voice]         Inject Prompt]       │
│         │                                       │               │
│         └───────────────────────────────────────┘               │
│                    (Loop repeats)                               │
└─────────────────────────────────────────────────────────────────┘
```

The loop operates as follows:
1. **PersonaPlex conversation window** (10-30 seconds) - Real-time bidirectional audio
2. **"Thinking moment" audio** - Pre-recorded clip in the same voice (e.g., "Let me think about that...")
3. **Local LLM inference** - Summarize conversation and generate updated system prompt
4. **Context injection** - Reset PersonaPlex and inject the new context
5. **Repeat**

---

## Table of Contents

1. [Repository Setup](#1-repository-setup)
2. [Core API Reference](#2-core-api-reference)
3. [Memory Budget](#3-memory-budget)
4. [Control Loop Implementation](#4-control-loop-implementation)
5. [Audio Injection](#5-audio-injection)
6. [Context Compaction](#6-context-compaction)
7. [Optimization Parameters](#7-optimization-parameters)
8. [Configuration Schema](#8-configuration-schema)

---

## 1. Repository Setup

### Clone and Install

```bash
# Clone the repository
git clone https://github.com/your-org/personaplex.git
cd personaplex

# Install in development mode
pip install -e ./moshi

# Required dependencies for INT8 quantization
pip install torchao

# Optional: For CPU offloading (not recommended for real-time)
pip install accelerate
```

### Model Downloads

Models are automatically downloaded from HuggingFace on first use. To pre-download:

```python
from huggingface_hub import hf_hub_download

HF_REPO = "nvidia/personaplex-7b-v1"

# Download all required files
mimi_path = hf_hub_download(HF_REPO, "tokenizer-e351c8d8-checkpoint125.safetensors")
moshi_path = hf_hub_download(HF_REPO, "model.safetensors")
tokenizer_path = hf_hub_download(HF_REPO, "tokenizer_spm_32k_3.model")
voices_path = hf_hub_download(HF_REPO, "voices.tgz")
```

---

## 2. Core API Reference

### Constants

```python
SAMPLE_RATE = 24000      # Audio sample rate (Hz)
FRAME_RATE = 12.5        # Frames per second
FRAME_SIZE = 1920        # Samples per frame (SAMPLE_RATE / FRAME_RATE)
DEFAULT_CONTEXT = 3000   # ~4 minutes at 12.5 fps
```

### Model Loading

```python
import torch
import sentencepiece
from moshi.models import loaders

device = torch.device("cuda")

# Load Mimi (audio codec) - need two instances for encode/decode separation
mimi = loaders.get_mimi(mimi_path, device)
other_mimi = loaders.get_mimi(mimi_path, device)  # For user audio decode

# Load text tokenizer
text_tokenizer = sentencepiece.SentencePieceProcessor(tokenizer_path)

# Load Moshi LM with INT8 quantization and reduced context
lm = loaders.get_moshi_lm(
    moshi_path,
    device=device,
    quantize_int8=True,      # ~50% memory reduction
    context=187,             # ~15 seconds at 12.5 fps (default: 3000)
)
lm.eval()
```

#### `get_moshi_lm()` Parameters

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `filename` | `str \| Path` | required | Path to model weights |
| `device` | `str \| torch.device` | `"cpu"` | Target device |
| `dtype` | `torch.dtype` | `torch.bfloat16` | Weight precision |
| `quantize_int8` | `bool` | `False` | Enable INT8 quantization (torchao) |
| `context` | `int` | `3000` | Context window size in tokens |
| `cpu_offload` | `bool` | `False` | Enable CPU offloading (accelerate) |
| `max_gpu_memory` | `str` | `None` | GPU memory limit (e.g., "8GiB") |

### LMGen (Generation Wrapper)

```python
from moshi.models import LMGen

lm_gen = LMGen(
    lm,
    device=device,

    # Sampling parameters
    use_sampling=True,       # False for greedy decoding
    temp=0.8,                # Audio temperature
    temp_text=0.7,           # Text temperature
    top_k=250,               # Audio top-k
    top_k_text=25,           # Text top-k

    # Frame configuration
    sample_rate=24000,
    frame_rate=12.5,
    audio_silence_frame_cnt=6,  # 0.5s silence after prompts

    # Voice prompt caching
    save_voice_prompt_embeddings=False,  # Set True to cache embeddings

    # CUDA graphs (disable if using CPU offload)
    disable_cuda_graphs=False,
)
```

### Streaming Initialization

```python
# Enter streaming mode (call once at startup)
mimi.streaming_forever(batch_size=1)
other_mimi.streaming_forever(batch_size=1)
lm_gen.streaming_forever(batch_size=1)
```

### Voice and Text Prompt Configuration

```python
# Load voice prompt (WAV or cached .pt file)
lm_gen.load_voice_prompt("path/to/voice.wav")
# Or load pre-cached embeddings (faster)
lm_gen.load_voice_prompt_embeddings("path/to/voice.pt")

# Set text/system prompt
system_prompt = "<system> You are a helpful assistant. <system>"
lm_gen.text_prompt_tokens = text_tokenizer.encode(system_prompt)
```

### System Prompt Injection

```python
# Reset streaming state before new conversation
mimi.reset_streaming()
other_mimi.reset_streaming()
lm_gen.reset_streaming()

# Inject all system prompts (voice + silence + text + silence)
# Synchronous version:
lm_gen.step_system_prompts(mimi)

# Async version (for real-time use):
await lm_gen.step_system_prompts_async(mimi, is_alive=check_connection)

# Reset mimi after prompt injection
mimi.reset_streaming()
```

### Main Processing Loop

```python
# Process one audio frame (1920 samples = 80ms)
# audio_chunk: torch.Tensor of shape [1, 1, 1920]
codes = mimi.encode(audio_chunk)  # Returns [1, 8, 1]

# Step through each code frame (usually 1 per audio frame)
for c in range(codes.shape[-1]):
    tokens = lm_gen.step(codes[:, :, c:c+1])

    if tokens is None:
        # Still in warmup/delay phase
        continue

    # tokens shape: [1, dep_q+1, 1] = [1, 17, 1]
    # tokens[:, 0, 0] = text token
    # tokens[:, 1:9, :] = agent audio codebooks (8 streams)
    # tokens[:, 9:17, :] = user audio codebooks (echoed back)

    # Decode agent audio
    agent_pcm = mimi.decode(tokens[:, 1:9])  # [1, 1, 1920]

    # Get text token
    text_token = tokens[0, 0, 0].item()
    if text_token not in (0, 3):  # Skip padding tokens
        text = text_tokenizer.id_to_piece(text_token)
        text = text.replace("▁", " ")  # SentencePiece underscore
```

### Reset for New Conversation Window

```python
def reset_for_new_window(mimi, other_mimi, lm_gen):
    """Reset all streaming state for a fresh conversation window."""
    mimi.reset_streaming()
    other_mimi.reset_streaming()
    lm_gen.reset_streaming()
```

---

## 3. Memory Budget

### GPU Memory by Configuration (16GB GPU)

| Configuration | Context | PersonaPlex VRAM | Available for Secondary LLM |
|---------------|---------|------------------|----------------------------|
| BF16, 3000 tokens | ~4 min | ~14-15GB | ~1GB (insufficient) |
| INT8, 3000 tokens | ~4 min | ~8-9GB | ~6-7GB |
| **INT8, 750 tokens** | ~1 min | ~7-8GB | ~7-8GB |
| **INT8, 187 tokens** | ~15s | ~6-7GB | ~8-9GB |
| INT4, 750 tokens | ~1 min | ~5-6GB (needs impl) | ~9-10GB |
| INT4, 187 tokens | ~15s | ~4-5GB (needs impl) | ~10-11GB |

### Recommended Configuration

For a 16GB GPU running both PersonaPlex and a secondary LLM:

```python
# Conservative: More context, less LLM headroom
lm = loaders.get_moshi_lm(
    moshi_path,
    device=device,
    quantize_int8=True,
    context=750,  # ~1 minute
)
# Leaves ~7-8GB for secondary model

# Aggressive: Less context, more LLM headroom
lm = loaders.get_moshi_lm(
    moshi_path,
    device=device,
    quantize_int8=True,
    context=187,  # ~15 seconds
)
# Leaves ~8-9GB for secondary model
```

### Context Window to Duration Mapping

| Context (tokens) | Duration (seconds) | Duration (readable) |
|-----------------|-------------------|---------------------|
| 187 | 15 | ~15 seconds |
| 375 | 30 | ~30 seconds |
| 750 | 60 | ~1 minute |
| 1500 | 120 | ~2 minutes |
| 3000 | 240 | ~4 minutes |

Formula: `duration_seconds = context_tokens / FRAME_RATE`

---

## 4. Control Loop Implementation

### Complete Control Loop Pseudocode

```python
import torch
import numpy as np
import sentencepiece
from moshi.models import loaders, LMGen

class PersonaPlexController:
    def __init__(self, config):
        self.config = config
        self.device = torch.device(config["device"])

        # Load models
        self._load_models()

        # State
        self.conversation_history = []
        self.current_text_buffer = []

    def _load_models(self):
        """Initialize PersonaPlex models."""
        cfg = self.config

        # Mimi codec
        self.mimi = loaders.get_mimi(cfg["mimi_weight"], self.device)
        self.other_mimi = loaders.get_mimi(cfg["mimi_weight"], self.device)

        # Tokenizer
        self.tokenizer = sentencepiece.SentencePieceProcessor(cfg["tokenizer"])

        # Moshi LM
        self.lm = loaders.get_moshi_lm(
            cfg["moshi_weight"],
            device=self.device,
            quantize_int8=cfg.get("quantize_int8", True),
            context=cfg.get("context_window", 187),
        )
        self.lm.eval()

        # LMGen wrapper
        self.lm_gen = LMGen(
            self.lm,
            device=self.device,
            temp=cfg.get("audio_temp", 0.8),
            temp_text=cfg.get("text_temp", 0.7),
            top_k=cfg.get("audio_topk", 250),
            top_k_text=cfg.get("text_topk", 25),
            sample_rate=24000,
            frame_rate=12.5,
        )

        # Enter streaming mode
        self.mimi.streaming_forever(1)
        self.other_mimi.streaming_forever(1)
        self.lm_gen.streaming_forever(1)

        # Warmup
        self._warmup()

    def _warmup(self):
        """Warmup CUDA graphs and streaming state."""
        frame_size = 1920
        for _ in range(4):
            chunk = torch.zeros(1, 1, frame_size, device=self.device)
            codes = self.mimi.encode(chunk)
            _ = self.other_mimi.encode(chunk)
            for c in range(codes.shape[-1]):
                tokens = self.lm_gen.step(codes[:, :, c:c+1])
                if tokens is not None:
                    _ = self.mimi.decode(tokens[:, 1:9])
        if self.device.type == 'cuda':
            torch.cuda.synchronize()

    def start_conversation_window(self, voice_prompt_path: str, system_prompt: str):
        """Initialize a new conversation window with prompts."""
        # Reset streaming state
        self.mimi.reset_streaming()
        self.other_mimi.reset_streaming()
        self.lm_gen.reset_streaming()

        # Load voice prompt
        if voice_prompt_path.endswith('.pt'):
            self.lm_gen.load_voice_prompt_embeddings(voice_prompt_path)
        else:
            self.lm_gen.load_voice_prompt(voice_prompt_path)

        # Set system prompt
        wrapped_prompt = f"<system> {system_prompt} <system>"
        self.lm_gen.text_prompt_tokens = self.tokenizer.encode(wrapped_prompt)

        # Inject prompts
        self.lm_gen.step_system_prompts(self.mimi)

        # Reset mimi after injection
        self.mimi.reset_streaming()

        # Clear text buffer
        self.current_text_buffer = []

    def process_audio_frame(self, audio_chunk: np.ndarray) -> tuple[np.ndarray, str]:
        """
        Process one audio frame (1920 samples).

        Args:
            audio_chunk: Float32 numpy array of shape (1920,)

        Returns:
            (output_audio, text_token) - Output audio (1920 samples) and any text
        """
        # Convert to tensor
        chunk = torch.from_numpy(audio_chunk).to(self.device)
        chunk = chunk.view(1, 1, -1)  # [1, 1, 1920]

        # Encode user audio
        codes = self.mimi.encode(chunk)

        output_audio = None
        text_output = ""

        for c in range(codes.shape[-1]):
            tokens = self.lm_gen.step(codes[:, :, c:c+1])

            if tokens is None:
                continue

            # Decode agent audio
            pcm = self.mimi.decode(tokens[:, 1:9])
            output_audio = pcm[0, 0].cpu().numpy()

            # Decode text
            text_token = tokens[0, 0, 0].item()
            if text_token not in (0, 3):
                text = self.tokenizer.id_to_piece(text_token)
                text = text.replace("▁", " ")
                text_output += text
                self.current_text_buffer.append(text)

        return output_audio, text_output

    def end_conversation_window(self) -> str:
        """End current window and return accumulated text."""
        full_text = "".join(self.current_text_buffer)
        self.conversation_history.append(full_text)
        return full_text

    def get_conversation_summary(self) -> str:
        """Get all conversation text for summarization."""
        return "\n---\n".join(self.conversation_history)


# Main control loop
async def main_control_loop(controller, audio_io, secondary_llm, config):
    """
    Main control loop integrating PersonaPlex with a secondary LLM.
    """
    conversation_number = 0
    current_context = config["initial_system_prompt"]

    while True:
        conversation_number += 1

        # 1. Start new conversation window
        controller.start_conversation_window(
            config["voice_prompt_path"],
            current_context
        )

        # 2. Run conversation for configured duration
        window_duration = config["conversation_duration"]  # seconds
        frame_duration = 1.0 / 12.5  # 80ms
        frames_to_process = int(window_duration / frame_duration)

        for frame_idx in range(frames_to_process):
            # Read audio from input
            input_audio = await audio_io.read_frame()  # 1920 samples

            # Process through PersonaPlex
            output_audio, text = controller.process_audio_frame(input_audio)

            # Write audio to output
            if output_audio is not None:
                await audio_io.write_frame(output_audio)

        # 3. End window and get transcript
        transcript = controller.end_conversation_window()

        # 4. Play "thinking moment" audio
        thinking_audio = load_thinking_audio(config["thinking_audio_path"])
        await audio_io.play_clip(thinking_audio)

        # 5. Run secondary LLM for context compaction
        full_history = controller.get_conversation_summary()
        current_context = await secondary_llm.generate(
            CONTEXT_COMPACTION_PROMPT.format(
                history=full_history,
                base_persona=config["base_persona"],
            )
        )

        # Loop continues with new context
```

### Async Version with Connection Checking

```python
async def process_conversation_window_async(
    controller,
    audio_io,
    duration_seconds: float,
    is_alive_callback,
):
    """Process a conversation window with connection checking."""
    frame_duration = 1.0 / 12.5
    frames = int(duration_seconds / frame_duration)

    for _ in range(frames):
        if not await is_alive_callback():
            break

        input_audio = await audio_io.read_frame()
        output_audio, text = controller.process_audio_frame(input_audio)

        if output_audio is not None:
            await audio_io.write_frame(output_audio)

    return controller.end_conversation_window()
```

---

## 5. Audio Injection

### "Thinking Moment" Audio Clip

The thinking moment audio should be:
- Pre-recorded in the **same voice** as the PersonaPlex voice prompt
- Short duration (1-3 seconds)
- Natural filler phrases like:
  - "Let me think about that for a moment..."
  - "Hmm, that's an interesting question..."
  - "Give me a second to consider that..."

### Loading and Playing the Clip

```python
import sphn
import numpy as np

def load_thinking_audio(path: str, target_sr: int = 24000) -> np.ndarray:
    """Load a thinking moment audio clip."""
    pcm, sr = sphn.read(path)
    if sr != target_sr:
        pcm = sphn.resample(pcm, src_sample_rate=sr, dst_sample_rate=target_sr)
    return pcm[0] if pcm.ndim == 2 else pcm  # Ensure mono

async def play_thinking_clip(audio_io, thinking_audio: np.ndarray):
    """Play the thinking clip while LLM processes."""
    frame_size = 1920
    for i in range(0, len(thinking_audio), frame_size):
        frame = thinking_audio[i:i+frame_size]
        if len(frame) < frame_size:
            frame = np.pad(frame, (0, frame_size - len(frame)))
        await audio_io.write_frame(frame)
```

### Voice Prompt Caching

To speed up conversation window restarts, cache voice prompt embeddings:

```python
# First time: Load WAV and save embeddings
lm_gen.save_voice_prompt_embeddings = True
lm_gen.load_voice_prompt("voice.wav")
lm_gen.step_system_prompts(mimi)
# Creates "voice.pt" automatically

# Subsequent times: Load cached embeddings (faster)
lm_gen.load_voice_prompt_embeddings("voice.pt")
```

---

## 6. Context Compaction

### Summarization Prompt Template

```python
CONTEXT_COMPACTION_PROMPT = """You are a context manager for a real-time voice AI assistant.

## Base Persona
{base_persona}

## Recent Conversation History
{history}

## Your Task
Create a concise system prompt that:
1. Maintains the base persona
2. Incorporates relevant context from the conversation
3. Captures any commitments or ongoing topics
4. Stays under 200 words

## Output Format
Output ONLY the system prompt text, starting directly with the persona description.
Do not include any preamble or explanation.

System prompt:"""
```

### Integration with Secondary LLM

```python
async def compact_context(
    secondary_llm,
    conversation_history: str,
    base_persona: str,
) -> str:
    """Use secondary LLM to generate updated context."""
    prompt = CONTEXT_COMPACTION_PROMPT.format(
        history=conversation_history,
        base_persona=base_persona,
    )

    response = await secondary_llm.generate(
        prompt,
        max_tokens=300,
        temperature=0.3,  # Low temp for consistency
    )

    # Ensure proper formatting
    if not response.startswith("<system>"):
        response = f"<system> {response.strip()} <system>"

    return response
```

### Context Injection After Compaction

```python
def inject_new_context(controller, new_system_prompt: str):
    """Reset PersonaPlex and inject new context."""
    # The start_conversation_window method handles:
    # 1. Resetting streaming state
    # 2. Loading voice prompt
    # 3. Setting new system prompt
    # 4. Running prompt injection
    controller.start_conversation_window(
        controller.config["voice_prompt_path"],
        new_system_prompt
    )
```

---

## 7. Optimization Parameters

### Timing Constraints

| Operation | Typical Duration | Notes |
|-----------|-----------------|-------|
| Frame processing | ~80ms | 12.5 fps |
| `reset_streaming()` | ~1-5ms | All three models |
| Voice prompt injection | Varies | ~1s per 1s of prompt audio |
| Text prompt injection | ~10ms per token | Depends on prompt length |
| Full restart | ~1-3s | Reset + voice + text prompts |

### Minimizing Pause Time

1. **Pre-cache voice prompts** - Use `.pt` files instead of `.wav`
2. **Short system prompts** - Keep under 50 tokens if possible
3. **Reduce voice prompt length** - Use short voice samples (2-5 seconds)
4. **Overlap operations** - Play thinking audio during LLM inference

### Sampling Parameters

| Parameter | Recommended | Effect |
|-----------|-------------|--------|
| `temp` (audio) | 0.7-0.9 | Lower = more consistent voice |
| `temp_text` | 0.6-0.8 | Lower = more coherent responses |
| `top_k` (audio) | 200-300 | Higher = more variety |
| `top_k_text` | 20-30 | Higher = more vocabulary |

---

## 8. Configuration Schema

### config.yaml

```yaml
# PersonaPlex Configuration for Entropi Integration

# Model paths (auto-downloaded if not specified)
model:
  hf_repo: "nvidia/personaplex-7b-v1"
  moshi_weight: null        # Path or null for auto-download
  mimi_weight: null         # Path or null for auto-download
  tokenizer: null           # Path or null for auto-download

# Device and memory
runtime:
  device: "cuda"
  quantization: "int8"      # "none" | "int8" | "int4" (int4 not yet implemented)
  context_window: 187       # Tokens (~15s at 12.5 fps)
  cpu_offload: false        # Not recommended for real-time

# Voice configuration
voice:
  prompt_dir: "./voices"    # Directory containing voice prompts
  prompt_file: "NATM1.pt"   # Voice prompt filename
  thinking_audio: "./voices/thinking_moment.wav"

# Conversation loop
conversation:
  duration: 15.0            # Seconds per window
  initial_prompt: "You are a helpful and friendly assistant."

# Sampling parameters
sampling:
  audio_temp: 0.8
  text_temp: 0.7
  audio_topk: 250
  text_topk: 25
  use_sampling: true        # false for greedy

# Secondary LLM (entropi's choice)
secondary_model:
  # Configure based on available VRAM after PersonaPlex
  # With INT8 + context=187: ~8-9GB available
  model: "your-local-llm"
  max_tokens: 300
  temperature: 0.3
```

### Loading Configuration

```python
import yaml

def load_config(path: str) -> dict:
    with open(path) as f:
        return yaml.safe_load(f)

config = load_config("config.yaml")

# Context window conversion
context_tokens = config["runtime"]["context_window"]
context_seconds = context_tokens / 12.5
print(f"Context window: {context_tokens} tokens = {context_seconds:.1f} seconds")
```

---

## Appendix A: Text Token Special Values

| Token ID | Meaning | Action |
|----------|---------|--------|
| 0 | EPAD (End Pad) | Skip |
| 1 | BOS (Begin of Sequence) | Skip |
| 2 | EOS (End of Sequence) | Skip |
| 3 | PAD (Padding) | Skip |
| 4+ | Actual vocabulary | Decode with tokenizer |

```python
def decode_text_token(tokenizer, token_id: int) -> str | None:
    """Decode a text token, returning None for special tokens."""
    if token_id in (0, 1, 2, 3):
        return None
    text = tokenizer.id_to_piece(token_id)
    return text.replace("▁", " ")
```

---

## Appendix B: Audio Codec Details

### Mimi Codec Structure

- **Input**: 24kHz mono audio
- **Frame size**: 1920 samples (80ms)
- **Codebooks**: 8 parallel streams
- **Each codebook**: 2048 vocabulary tokens
- **Output**: 8 tokens per frame

### Token Layout in LMGen Output

```
tokens shape: [batch, 17, time]

Index 0:     Text token (32k vocab)
Index 1-8:   Agent audio codebooks (Mimi)
Index 9-16:  User audio codebooks (echoed)
```

---

## Appendix C: INT4 Quantization (Future)

INT4 quantization is not yet implemented but could provide additional memory savings:

```python
# Theoretical implementation (not yet available)
lm = loaders.get_moshi_lm(
    moshi_path,
    device=device,
    quantize_int4=True,  # Would require torchao INT4 support
    context=187,
)
```

Expected memory with INT4:
- ~4-5GB for PersonaPlex (context=187)
- ~10-11GB available for secondary model

To implement, the `loaders.py` quantization path would need to use `torchao.quantization.int4_weight_only()` instead of `int8_weight_only()`.

---

## Appendix D: Troubleshooting

### CUDA Out of Memory

1. Reduce context window: `context=187` or lower
2. Enable INT8 quantization: `quantize_int8=True`
3. Check for other GPU processes: `nvidia-smi`

### Slow First Inference

- CUDA graphs are compiled on first use
- Run warmup loop before real-time processing
- Voice prompt injection runs once per session (cache with `.pt`)

### Audio Quality Issues

- Ensure 24kHz sample rate
- Use mono audio (stereo will be converted)
- Check voice prompt quality matches desired output

### Text Decoding Gibberish

- Verify tokenizer path matches model
- Check for token ID out of range
- Ensure `<system>` tags are properly formatted
