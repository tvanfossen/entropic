# Models

> Model tiers, routing, and VRAM management

## Architecture Overview

Entropic uses a multi-model architecture with task-specialized routing:

```
User Input
    │
    ▼
┌─────────────────┐
│  MICRO (0.6B)   │ ◄── Always loaded, classifies tasks
└────────┬────────┘
         │
         ├── SIMPLE/REASONING ──► NORMAL (7B)
         ├── CODE ──────────────► CODE (7B)
         └── COMPLEX ───────────► THINKING (14B)
```

## Model Tiers

| Tier | Model | Size | Purpose |
|------|-------|------|---------|
| **THINKING** | Qwen3-14B Q4_K_M | ~9 GB | Complex reasoning, architecture, planning |
| **NORMAL** | Falcon-H1R-7B Q8_0 | ~7 GB | General tasks, explanations, discussions |
| **CODE** | Falcon-H1R-7B Q8_0 | ~7 GB | Code generation, debugging, refactoring |
| **MICRO** | Qwen3-0.6B Q8_0 | ~0.6 GB | Task classification (always loaded) |

## Task Routing

The MICRO model classifies each input into one of four categories:

| Classification | Routes To | Examples |
|---------------|-----------|----------|
| **SIMPLE** | NORMAL | "hello", "thanks", greetings |
| **CODE** | CODE | "write a function", "fix this bug" |
| **REASONING** | NORMAL | "explain this", "what is X" |
| **COMPLEX** | THINKING | "design a system", "analyze architecture" |

### Routing with GBNF Grammar

The MICRO model uses a GBNF grammar constraint to ensure valid classification:

```
root ::= "CODE" | "REASONING" | "SIMPLE" | "COMPLEX"
```

This guarantees the output is exactly one of the four categories.

## VRAM Budget

With 16GB VRAM, the typical usage:

### Normal Operation

```
Falcon-H1R-7B weights     ~7 GB
Qwen3-0.6B weights        ~0.6 GB
KV cache (32K context)    ~2 GB
CUDA overhead             ~0.5 GB
────────────────────────────────
Total                     ~10 GB
Headroom                  ~6 GB
```

### Thinking Mode

```
Qwen3-14B weights         ~9 GB
Qwen3-0.6B weights        ~0.6 GB
KV cache (16K context)    ~2.5 GB
CUDA overhead             ~0.5 GB
────────────────────────────────
Total                     ~12.6 GB
Headroom                  ~3.4 GB
```

## Model Swapping

Models are loaded on-demand and swapped as needed:

1. **MICRO** is always loaded (tiny footprint)
2. **NORMAL/CODE** swap dynamically (usually same model)
3. **THINKING** loads only when needed (for COMPLEX tasks or `/think on`)

Swap time is typically 2-5 seconds depending on model size.

## Adapters

Each model family uses a specific adapter for chat formatting and tool calling:

| Adapter | Models | Features |
|---------|--------|----------|
| `qwen3` | Qwen3-*, Qwen2.5-* | `<think>` blocks, `<tool_call>` tags |
| `falcon` | Falcon-H1R-* | `<think>` blocks, `<tool_call>` tags |
| `generic` | Any | Basic tool call format |

## Performance

Expected token speeds on RTX PRO 4000 (16GB VRAM):

| Model | Prompt Eval | Generation |
|-------|-------------|------------|
| Qwen3-14B Q4_K_M | ~1200 t/s | ~40-55 t/s |
| Falcon-H1R-7B Q8_0 | ~2000 t/s | ~60-80 t/s |
| Qwen3-0.6B Q8_0 | ~8000 t/s | ~400-500 t/s |

## Thinking Mode

Toggle deep reasoning with `/think on`:

- **Normal mode** (`/think off`): Uses 7B model for faster responses
- **Thinking mode** (`/think on`): Uses 14B model with `<think>` blocks for deeper reasoning

The model uses `<think>...</think>` blocks to show its reasoning process before providing the final answer.

## Recommended Models

### Current Defaults

| Tier | Recommended Model | Download |
|------|-------------------|----------|
| THINKING | Qwen3-14B-Q4_K_M | `bartowski/Qwen_Qwen3-14B-GGUF` |
| NORMAL | Falcon-H1R-7B-Q8_0 | `brittlewis12/Falcon-H1R-7B-GGUF` |
| CODE | Falcon-H1R-7B-Q8_0 | Same as NORMAL |
| MICRO | Qwen3-0.6B-Q8_0 | `bartowski/Qwen3-0.6B-GGUF` |

### Alternatives

For different VRAM budgets:

**12GB VRAM:**
- Reduce context lengths
- Use Q4_K_M quantization for all models

**24GB+ VRAM:**
- Use larger context windows
- Consider Q6_K or Q8_0 quantization for better quality
