---
version: 1.0.0
type: proposal
schema_version: 1
id: P1-20260213-012
title: "Per-Tier Model Fine-Tuning Pipeline"
priority: P1
component: inference
author: tvanfossen
author_email: vanfosst@gmail.com
created: 2026-02-13
updated: 2026-02-13
tags: [fine-tuning, qlora, tool-calling, training, inference]
completed_date: null
scoped_files:
  - src/entropi/inference/
  - src/entropi/data/
  - .entropi/session_model.log
  - .entropi/config.local.yaml
  - scripts/
  - tests/model/
depends_on: []
blocks: []
---

# Per-Tier Model Fine-Tuning Pipeline

## Problem Statement

Small local models (7B-14B) exhibit a **thinking→action translation gap**: the model's
chain-of-thought correctly reasons about which tool to call, but the emitted tool call
diverges from its own reasoning. Observed in Qwen3 14B during smoke testing — model's
`<think>` block says "I should call entropi.todo_write first" then emits `bash.execute`.

This gap cannot be solved at any runtime layer:
- **Engine enforcement** — engine must remain tool/model/tier agnostic
- **Adapter enforcement** — conflates format translation with behavioral policy
- **Tool enforcement** — tools don't control invocation order
- **Prompt engineering** — already tried; model reads, acknowledges, and ignores the instruction

Fine-tuning is the only approach that addresses the root cause: training the model to
maintain coherence between its reasoning and its actions.

## Research Summary

### Evidence That Fine-Tuning Works for Tool Use

| Study | Result |
|-------|--------|
| [Small LMs for Agentic Tool Calling](https://arxiv.org/abs/2512.15943) | 350M param model SFT'd on tool data → 77.5% on ToolBench, beating ChatGPT-CoT (26%) |
| [Medium: Fine Tuning SLMs](https://medium.com/@dataenthusiast.io/fine-tuning-slms-on-agentic-tool-calling-an-experiment-ccbef62ac5c7) | 1B model: 10% → 79% tool calling success in 15 min on MacBook |
| [ToolRM](https://arxiv.org/html/2509.11963v1) | 14B reward model improved Qwen3-0.6B accuracy by +25 points, surpassing Qwen3-32B |

### Critical Risk: CoT Faithfulness Degradation

[On the Impact of Fine-Tuning on Chain-of-Thought Reasoning](https://arxiv.org/abs/2411.15382)
found that naive SFT can **degrade** thinking→action faithfulness in smaller models. The model
learns correct tool calls but disconnects them further from reasoning.

**Mitigation:** SFT + DPO pipeline (Relign approach). SFT establishes correct behavior, DPO
penalizes thinking/action divergence via preference pairs built from correct vs. hallucinated
tool invocations.

### Qwen3 Specifics

- Hermes-style tool calling is the recommended format (native in tokenizer_config.json)
- Unsloth supports Qwen3 fine-tuning: 2x faster, 70% less VRAM, 8x longer context
- Qwen3-14B baseline: 65.1 on Tau2-Bench for agentic tasks

### Training Data Requirements

- **14B models need less data** than smaller models (multiplicative scaling laws, Google DeepMind 2024)
- **Minimum viable:** 1,000-5,000 curated examples
- **Quality > quantity:** 1,000 curated examples outperform 10,000 mediocre ones
- **Reward model filtering:** Top 50% selected by ToolRM achieved higher accuracy with half the data

## Data Collection Strategy

### Source: Session Logs (Already Captured)

`session_model.log` already records the exact format needed for training:

```
[PROMPT] → full message history (system + user + assistant + tool results)
[RAW OUTPUT] → model's raw response including <think> blocks
[TOOL CALLS] → parsed tool calls with arguments
```

### Training Data Shape

Each training example = one turn:

```json
{
  "messages": [
    {"role": "system", "content": "...tier-specific system prompt..."},
    {"role": "user", "content": "...user message or tool result..."}
  ],
  "thinking": "<think>reasoning about next action</think>",
  "tool_calls": [
    {"name": "correct.tool", "arguments": {"key": "value"}}
  ]
}
```

### Collection Approaches (Ranked)

1. **Curated session logs** — Run entropi on real tasks, manually annotate correct vs.
   incorrect tool selections. Highest quality, lowest volume.

2. **Synthetic generation** — Use a larger model (e.g., via API) to generate correct
   thinking + tool call pairs given entropi's system prompts and tool definitions.
   Higher volume, requires validation.

3. **Self-play with correction** — Run entropi, capture failures (user denials, repeated
   tool calls, wrong-tool-first patterns), generate corrected versions as preference
   pairs for DPO.

### Per-Tier Training Sets

Each tier has distinct behavioral expectations:

| Tier | Training Focus |
|------|---------------|
| **thinking** | Todo-first investigation, multi-tool batching, structured analysis |
| **normal** | Balanced tool use, conversation + action |
| **code** | File operations, bash commands, edit workflows |
| **simple** | Minimal tool use, direct responses |

Fine-tuning should be **per-tier** — a single LoRA adapter per tier, swapped alongside
the tier's system prompt. This avoids behavioral contamination between tiers.

## Proposed Architecture

### Phase 1: Data Pipeline

```
session_model.log → extract_training_data.py → raw_examples/
                                                    │
                                        annotate (manual/automated)
                                                    │
                                                    ▼
                                            training_data/
                                            ├── thinking/
                                            ├── normal/
                                            ├── code/
                                            └── simple/
```

**Deliverable:** `scripts/extract_training_data.py` — parses session_model.log into
structured training examples, categorized by tier.

### Phase 2: Fine-Tuning Pipeline

```
training_data/{tier}/ → fine_tune.py → lora_adapters/{tier}/
                            │
                            ├── QLoRA (4-bit quantized base + LoRA)
                            ├── Unsloth for training acceleration
                            └── Hermes-style tool format
```

**Method:** QLoRA via Unsloth
- Base: Qwen3-14B-Q4_K_M (already downloaded)
- LoRA rank: 16-64 (tunable)
- Training: ~15-30 min per tier on RTX 4090
- Output: LoRA adapter files (~50-100MB per tier)

**Deliverable:** `scripts/fine_tune.py` — takes training data dir + base model, produces
LoRA adapter.

### Phase 3: Runtime Integration

```
config.local.yaml:
  models:
    thinking:
      model_path: models/Qwen3-14B-Q4_K_M.gguf
      lora_path: lora_adapters/thinking/   # <-- new
```

llama-cpp-python supports LoRA loading at runtime. The orchestrator loads the base model
once and swaps LoRA adapters on tier change (fast — no full model reload).

**Changes:**
- `ModelConfig` in schema.py: add optional `lora_path` field
- `LlamaCppBackend`: load LoRA adapter when specified
- Orchestrator: swap LoRA on tier change (if tier has one)

### Phase 4: Evaluation Loop

```
smoke_test_suite/ → run_evaluation.py → metrics/
                                            ├── tool_accuracy.json
                                            ├── thinking_action_coherence.json
                                            └── per_tier_scores.json
```

Automated evaluation against known-good test cases. Measures:
- Tool selection accuracy (did it call the right tool?)
- Thinking→action coherence (does the tool call match the reasoning?)
- Task completion rate (did it finish the assigned task?)

## Risk Assessment

| Risk | Severity | Mitigation |
|------|----------|------------|
| CoT faithfulness degradation | High | SFT + DPO pipeline, not SFT alone |
| General capability loss | Medium | 10% replay buffer of general data mixed in |
| Overfitting to entropi-specific patterns | Medium | Diverse task variety in training data |
| LoRA adapter doesn't load in llama-cpp | Low | Already supported in llama-cpp-python API |
| Training data insufficient quality | Medium | Start with manual curation, expand with synthetic |
| Per-tier LoRA swap latency | Low | LoRA swap is ~100ms, negligible vs. model load |

## Hardware Requirements

- **Training:** RTX 4090 (24GB VRAM) — sufficient for QLoRA on 14B
- **Inference:** Same hardware as current (LoRA adds negligible overhead)
- **Storage:** ~50-100MB per LoRA adapter

## Success Criteria

- [ ] Training data extraction from session_model.log works end-to-end
- [ ] At least 1,000 curated examples per tier
- [ ] Fine-tuned thinking tier calls todo_write before other tools (the original failing test case)
- [ ] No measurable degradation on general conversation quality
- [ ] LoRA adapters load and swap at runtime without model reload
- [ ] Automated evaluation shows improvement over base model on tool selection accuracy

## Implementation Order

1. **Phase 1** first — data pipeline is prerequisite for everything else and validates
   whether session logs contain sufficient signal
2. **Phase 2** — fine-tuning pipeline, starting with thinking tier only (highest-value,
   clearest training signal from smoke test failures)
3. **Phase 3** — runtime integration (config + orchestrator + backend changes)
4. **Phase 4** — evaluation loop (ongoing, runs after each training iteration)

## Open Questions

- **DPO data source:** Self-play with correction, or manual annotation of preference pairs?
- **Cross-model generalization:** If we switch from Qwen3 to another model family, do we
  retrain from scratch or can training data transfer?
- **Router model:** Should the router (Qwen3-0.6B) also be fine-tuned for better
  classification, or is prompt-based routing sufficient?
- **Continuous learning:** Should the pipeline support incremental fine-tuning as new
  session logs accumulate, or full retrain each time?

## References

- [Small Language Models for Efficient Agentic Tool Calling](https://arxiv.org/abs/2512.15943)
- [On the Impact of Fine-Tuning on Chain-of-Thought Reasoning](https://arxiv.org/abs/2411.15382)
- [ToolRM: Outcome Reward Models for Tool-Calling LLMs](https://arxiv.org/html/2509.11963v1)
- [Reducing Tool Hallucination via Reliability Alignment (Relign)](https://www.themoonlight.io/en/review/reducing-tool-hallucination-via-reliability-alignment)
- [Qwen3 Technical Report](https://arxiv.org/pdf/2505.09388)
- [Unsloth - Qwen3 Fine-tuning](https://unsloth.ai/docs/models/qwen3-how-to-run-and-fine-tune)
- [Microsoft: Fine-Tuning SLMs for Function Calling](https://techcommunity.microsoft.com/blog/azure-ai-foundry-blog/fine-tuning-small-language-models-for-function-calling-a-comprehensive-guide/4362539)
- [How Much Data is Enough Data?](https://arxiv.org/abs/2409.03454)
