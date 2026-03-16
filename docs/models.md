# Models

> Model tiers, identity system, and VRAM management

## Architecture Overview

Entropic uses an **identity-based single-model architecture**. One model is loaded
at a time. Each tier maps to an identity prompt (role persona) with specific focus,
tools, and inference parameters.

```
Consumer Config (.entropic/config.local.yaml)
    │
    ▼
┌──────────────────────────────────────────────┐
│              Model Orchestrator               │
│                                               │
│  ┌──────────┐  ┌──────────┐  ┌───────────┐  │
│  │   lead   │  │   eng    │  │    qa     │  │
│  │ (primary)│  │ (primary)│  │ (primary) │  │
│  └──────────┘  └──────────┘  └───────────┘  │
│       All share the same model file          │
│       Identities change the persona          │
└──────────────────┬───────────────────────────┘
                   │
                   ▼
            ┌─────────────┐
            │  GGUF Model │  ◄── Single model loaded
            │  (35B-A3B)  │      Identity swaps are free
            └─────────────┘
```

**Key insight:** Identity swaps are zero-cost (just prompt changes). Model swaps
are expensive (VRAM load/unload). The identity system maximizes a single model's
utility by giving it different personas.

## Bundled Identities

| Identity | Role Type | Focus | auto_chain |
|----------|-----------|-------|------------|
| **lead** | front_office | Orchestrate, delegate, plan | — |
| **eng** | front_office | Implement code, debug | lead |
| **qa** | front_office | Test, review, verify | lead |
| **arch** | front_office | System design, architecture | lead |
| **ux** | front_office | User experience design | lead |
| **ui** | front_office | Interface implementation | lead |
| **analyst** | front_office | Requirements analysis | lead |
| **devops** | front_office | Infrastructure, deployment | lead |
| **compactor** | back_office | Context compaction | — |
| **scribe** | back_office | Conversation summarization | — |
| **benchmark_judge** | utility | Quality assessment | — |

Front-office roles auto-chain back to lead on completion. Back-office and utility
roles are engine-invoked (not delegated to by lead).

## Identity Frontmatter

Inference behavior lives in identity frontmatter (not in config):

| Field | Type | Description |
|-------|------|-------------|
| `name` | str | Identity name (matches tier name) |
| `focus` | list[str] | Role focus areas |
| `examples` | list[str] | Example prompts this role handles |
| `auto_chain` | str/null | Tier to chain to on completion |
| `allowed_tools` | list[str]/null | Tool visibility filter |
| `bash_commands` | list[str]/null | Bash command allowlist (null = all) |
| `max_output_tokens` | int | Max tokens per generation |
| `temperature` | float | Sampling temperature |
| `repeat_penalty` | float | Repetition penalty |
| `enable_thinking` | bool | Allow `<think>` blocks |
| `model_preference` | str | Model selection preference |
| `interstitial` | bool | Engine-invoked between turns |
| `routable` | bool | Available for automatic routing |
| `role_type` | str | front_office / back_office / utility |
| `explicit_completion` | bool | Require `entropic.complete` to finish |
| `phases` | dict/null | Named phase configurations |

## Phases

Roles can define multiple phases with different inference parameters:

```yaml
phases:
  default:
    temperature: 0.4
    max_output_tokens: 4096
    enable_thinking: true
    repeat_penalty: 1.1
    bash_commands: null        # null = inherit from identity level
  execute:
    temperature: 0.3
    max_output_tokens: 2048
    enable_thinking: false
    repeat_penalty: 1.1
    bash_commands:             # Override: specific commands only
      - pytest
      - npm test
```

Phase transitions via `entropic.phase_change(phase="execute")`.

## Optional Router

Routing is **disabled by default** (single-model, lead routes via delegation).
When enabled, a small classification model routes prompts to tiers:

```yaml
models:
  router:
    path: ~/models/gguf/Qwen3-0.6B-Q8_0.gguf
    adapter: router
    context_length: 4096
    gpu_layers: -1
```

The router uses raw text continuation (no chat template) with optional GBNF grammar.

## VRAM Budget

With 16GB VRAM and Qwen3.5-35B-A3B (MoE, 3B active parameters):

### Single Model Operation (Default)

```
Qwen3.5-35B-A3B weights   ~7 GB   (MoE: only active experts loaded)
KV cache (32K context)     ~2 GB
CUDA overhead              ~0.5 GB
────────────────────────────────
Total                      ~9.5 GB
Headroom                   ~6.5 GB
```

### With Router (Optional)

```
Primary model weights      ~7 GB
Qwen3-0.6B weights         ~0.6 GB
KV cache                   ~2 GB
CUDA overhead              ~0.5 GB
────────────────────────────────
Total                      ~10 GB
Headroom                   ~6 GB
```

## Model Configuration

Hardware/load-time params in config, inference params in identity frontmatter:

### Config (`ModelConfig` / `TierConfig`)

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `path` | Path | required | Path to GGUF model file |
| `adapter` | str | `qwen2` | Adapter: qwen35, qwen3, qwen2, falcon, smollm3, router, generic |
| `context_length` | int | 16384 | Maximum context window (512–131072) |
| `gpu_layers` | int | -1 | GPU layers (-1 = all) |
| `keep_warm` | bool | false | Pre-warm at startup, deactivate (not unload) on swap |
| `use_mlock` | bool | true | Lock model pages in RAM |
| `logits_all` | bool | false | Compute logits for all positions |
| `allowed_tools` | list/null | null | Tool visibility filter (null = all) |
| `identity` | path/false/null | null | Custom identity file (null = bundled) |
| `grammar` | path/null | null | GBNF grammar file |
| `auto_chain` | bool/null | null | Override identity auto_chain |
| `routable` | bool/null | null | Override identity routable flag |

## Adapters

| Adapter | Models | Features |
|---------|--------|----------|
| `qwen35` | Qwen3.5-35B-A3B MoE | `<think>` blocks, `<tool_call>` tags |
| `qwen3` | Qwen3-* | `<think>` blocks, `<tool_call>` tags |
| `qwen2` | Qwen2.5-* | `<tool_call>` tags |
| `falcon` | Falcon-H1R-* | `<think>` blocks, `<tool_call>` tags |
| `smollm3` | SmolLM3-* | `<tool_call>` tags |
| `router` | Classification models | Raw text continuation, no chat template |
| `generic` | Any | Basic ChatML |

## Performance

Expected token speeds on RTX PRO 4000 (16GB VRAM):

| Model | Prompt Eval | Generation |
|-------|-------------|------------|
| Qwen3.5-35B-A3B Q4_K_M | ~800 t/s | ~30-40 t/s |
| Qwen3-14B Q4_K_M | ~1200 t/s | ~40-55 t/s |
| Falcon-H1R-7B Q8_0 | ~2000 t/s | ~60-80 t/s |
| Qwen3-0.6B Q8_0 | ~8000 t/s | ~400-500 t/s |

## Recommended Models

### Current Default

| Use Case | Model | Adapter | Download |
|----------|-------|---------|----------|
| Primary (all roles) | Qwen3.5-35B-A3B Q4_K_M | qwen35 | `unsloth/Qwen3.5-35B-A3B-GGUF` |
| Router (optional) | Qwen3-0.6B Q8_0 | router | `bartowski/Qwen3-0.6B-GGUF` |

### Alternatives by VRAM Budget

**8GB VRAM:**
- Qwen3-8B Q4_K_M or SmolLM3-3B Q8_0
- Reduce context_length to 8192

**12GB VRAM:**
- Qwen3-14B Q4_K_M
- Context up to 16384

**16GB VRAM (recommended):**
- Qwen3.5-35B-A3B Q4_K_M (MoE — 3B active, fits in 16GB)
- Context up to 32768

**24GB+ VRAM:**
- Higher quantization (Q6_K, Q8_0) for better quality
- Larger context windows
