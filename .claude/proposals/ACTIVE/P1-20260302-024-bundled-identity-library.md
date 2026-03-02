---
version: 1.0.0
type: proposal
schema_version: 1
id: P1-20260302-024
title: "Bundled identity library: grammar-constrained cognitive primitives"
priority: P1
component: identities
author: tvanfossen
author_email: vanfosst@gmail.com
created: 2026-03-02
updated: 2026-03-02
tags: [identities, grammar, tui, library, pipeline]
completed_date: null
scoped_files:
  - "src/entropic/prompts/**"
  - "src/entropic/data/prompts/**"
  - "src/entropic/data/grammars/**"
  - "src/entropic/config/schema.py"
  - "src/entropic/tui/**"
depends_on: [P1-20260226-022]
blocks: []
---

# Bundled Identity Library: Grammar-Constrained Cognitive Primitives

## Problem Statement

The current bundled identities (normal, thinking, simple, code) are too broad,
overlap in scope, and don't use grammar at all. This produces mediocre output
from local models because:

1. **Broad scope = weak output.** A "normal" identity tries to handle everything
   from code review to casual chat. Local 7-14B models perform significantly
   better when focused on a single narrow task.

2. **No grammar = unreliable structure.** Without output constraints, models
   produce inconsistent formats, ramble, and mix concerns. Grammar enforcement
   forces structured, predictable output that downstream pipeline stages can
   parse.

3. **No pipeline composition.** Current identities are terminal — each handles
   a request end-to-end. Real tasks decompose into stages: plan → search →
   write → validate → test. The engine supports `auto_chain` but bundled
   identities don't use it.

4. **Identities are engine assets, not TUI features.** Bundled identities ship
   with `entropic-engine` for any consumer to use. The TUI is the reference
   implementation that exercises every identity — not a coding-only app, but
   a full test bed proving the engine works.

## Design Principles

### Identity = Contract

Each identity is a unit of composition with a defined contract:

| Component | Purpose |
|-----------|---------|
| System prompt | Focus the model on ONE cognitive operation |
| Grammar (.gbnf) | Enforce output format — the identity's output schema |
| `allowed_tools` | Restrict tool access to what this identity needs |
| `auto_chain` | Define what identity runs next in a pipeline |
| `max_output_tokens` | Keep output small — narrow scope, narrow output |
| `temperature` | Tuned per cognitive mode |

### When an Identity Deserves to Exist

An identity is justified when **at least two** of these hold:

1. **Output format** differs from all other identities → different grammar
2. **Tool set** differs → different `allowed_tools`
3. **Model size** can shrink → run on smaller/faster model
4. **Behavioral mode** is fundamentally different → different prompt + temperature
5. **Pipeline position** — it's a stage that feeds into another identity

### Narrow Scope, Narrow Context

Each identity operates on a **focused slice** — single method, single finding,
single question. Grammar enforces that scope. The pipeline composes identities
into complex behaviors. No single identity needs the full context of a file or
project — it gets exactly what the previous stage determined was relevant.

### Identity Swap vs Model Swap

Identity swap (prompt + grammar change on the same loaded model) is effectively
free. Model swap (unload + load different .gguf) costs 1-2s. Most identities
map to the same model file — the optimization is for identity and grammar, not
for model binary. Accuracy/quality is worth 1-2s of load time when a model swap
IS needed, but the architecture minimizes swaps by sharing model files across
identities.

### Constraint Is the Value Proposition

Local models cannot handle ambiguity the way frontier models can. The engine's
job is to translate ambiguous user requests into narrow, grammar-constrained
pipeline steps. Each identity receives a focused input and produces a focused
output. The chaining handles the complexity that no single local model can.

## Identity Taxonomy

### Release Tiers

Identities are organized into three release tiers:

- **Ship (13)**: Proven, address known failures, or have strong evidence.
  These compose into working pipelines.
- **Expansion (5)**: Reasonable but unproven. Promote based on evidence.
- **Documented (7+)**: Patterns consumers can build. Example grammars
  provided, not bundled as default identities.

### Ship: Infrastructure (3)

**`router`** — Classify input → identity.
- Grammar: Digit enum. Trivial.
- Model: 0.6B, always loaded.
- Proven. Ships today. Non-negotiable.

**`compactor`** — Summarize context when approaching limits.
- Grammar: Summary schema (key facts, preserved tool results, dropped content).
- Model: 0.6B-1.5B. Engine-internal, never user-facing.
- Addresses known problem. Grammar improves summary quality vs current
  free-form compaction.

**`pruner`** — Evaluate context relevance, produce prune decisions.
- Grammar: Prune decision schema (message indices, keep/drop, reason).
- Model: 0.6B-1.5B. Runs between chain stages.
- Addresses proven failure (PruneMessages directive unreliable). Quality
  needs validation — ship as experimental.
- See "The Pruner Identity" section for detailed design.

### Ship: Planning & Discovery (3)

**`planner`** — Decompose task into steps.
- Grammar: Plan schema (steps array with description, target files, dependencies).
- Tools: None. Plans, doesn't execute.
- Chains to: `searcher`, `tool_runner`, any generator.
- Core pipeline stage. Grammar forces step-by-step structure that downstream
  stages parse mechanically. Proven pattern from pychess thinker.

**`searcher`** — Find relevant code/info via read-only tools.
- Grammar: Search result schema (file path, line range, snippet, relevance).
- Tools: `filesystem.read_file`, `filesystem.glob`, `filesystem.grep` (read-only).
- Chains to: `planner`, any analyzer.
- Tool restriction prevents hallucinated writes during exploration. A model
  that can only read can't hallucinate destructive tool calls.

**`diagnoser`** — Root-cause an error.
- Grammar: Diagnosis schema (error, root cause, evidence trail, confidence, fix).
- Tools: `filesystem.*`, `bash.execute`, `diagnostics.*`.
- Chains to: `planner`, any generator.
- Distinct cognitive mode: error→cause direction vs planner's task→steps
  direction. Combining search + diagnosis + fix proposal in one identity is
  three cognitive tasks stacked — too broad for a local model.

### Ship: Generators (2)

**`code_writer`** — Write ONE function/method.
- Grammar: Code block grammar (language tag, code body, no prose).
- Tools: `filesystem.write_file`.
- Chains to: `code_validator`, `test_writer`.
- Grammar prevents prose-code mixing (chronic local model problem). Narrow
  scope (one function) means the model doesn't need full-file context.

**`test_writer`** — Write tests for a function.
- Grammar: Test code grammar (test function structure, assert required).
- Tools: `filesystem.write_file`.
- Chains to: `code_validator`.
- Adversarial cognitive shift from code_writer (find edge cases, failure
  modes). Pipeline: code_writer → test_writer → code_validator is a quality
  loop no single identity replicates.

### Ship: Validators (1)

**`code_validator`** — Verify code against spec/style.
- Grammar: Validation schema (pass/fail, findings array with line, severity,
  issue, suggestion).
- Tools: `filesystem.read_file`, `bash.execute` (run linter/tests).
- Chains to: `code_writer` (on fail — automated fix loop).
- Grammar-enforced structure enables automation: engine parses pass/fail,
  routes back to code_writer on failure. Without grammar, validation output
  is prose that can't drive decisions.

### Ship: Structuring (1)

**`extractor`** — Pull structured data from unstructured text.
- Grammar: Consumer-extensible. Bundled with common schemas (entity
  extraction, key-value extraction, fact extraction).
- Tools: None.
- Grammar's highest-value application. Research shows grammar-constrained
  extraction outperforms unconstrained and beats finetuned models. The
  output schema IS the identity's purpose.

### Ship: Execution (1)

**`tool_runner`** — Execute tool sequences from a plan.
- Grammar: Tool call grammar (tool_name enum from registered tools, arguments
  schema per tool).
- Tools: All registered.
- Chains to: Terminal or back to `planner`.
- Arguably the most important identity for local model reliability.
  Grammar-constrained tool calling makes it PHYSICALLY IMPOSSIBLE to produce
  prose where a tool call should be. Eliminates tool hallucination — the #1
  failure mode of local models doing tool use.

### Ship: Conversational (2)

**`conversational`** — Natural dialogue, general assistance.
- Grammar: None (free-form).
- Tools: Read-only.
- The fallback. Handles requests that don't fit elsewhere. Router must not
  over-route to this — test classification accuracy.

**`quick`** — Greetings, acknowledgments, one-liners.
- Grammar: None.
- Tools: None.
- Model: 0.6B (or infrastructure model). Sub-second response. Proven today
  as `simple` tier.

### Expansion Tier (5) — Promote Based on Evidence

| Identity | Grammar | Trigger to promote |
|----------|---------|--------------------|
| `guardrail` | Pass/fail + violations schema | First consumer needing production safety. Grammar-as-policy-definition pattern is genuinely useful. |
| `thinker` | Optional think format | When `enable_thinking` adapter path validated. Adapter-level behavior change (`/think` vs `/no-think`) justifies distinct identity. |
| `summarizer` | Summary schema (key points, details dropped) | When pipeline testing shows searcher→summarizer→planner value. Distinct from compactor: consumer-facing vs engine-internal. |
| `reviewer` | Review schema (issues, praise, verdict) | When code review pipeline demand emerges. Broader scope than code_validator (evaluates approach, not just correctness). |
| `reflector` | Uncertainty list schema (uncertain aspects, reasoning gaps) | When uncertainty-list approach validated with local models. NOT confidence scores (models hallucinate those). Structured uncertainty lists are harder to fake. |

### Documented Patterns (7+) — Not Bundled

These are documented with example grammar files for consumers to build.
The engine doesn't ship these as default identities.

| Identity | Why not bundled |
|----------|----------------|
| `teacher` | Consumer demand uncertain for coding-focused tool. Grammar value is real (lesson structure) but identity is too domain-dependent without consumer specialization. |
| `rubber_duck` | Niche interaction pattern. Grammar perfect (output MUST be questions). But opt-in only, and most users want answers not questions. |
| `doc_writer` | Narrow use case. Often just `code_writer` with a docstring grammar. |
| `writer` | Merges into `conversational`. Distinction (artifact vs dialogue) too thin for local models. |
| `rewriter` | Niche. Code refactoring is `code_writer`. Text editing is `conversational`. |
| `comparator` | N-candidate ranking doesn't fit local model economics (N+1 inference calls). |
| `translator` | Useful but niche. Most local models handle translation via prompting. |
| `verifier` | Real problem (hallucination detection) but uncertain if local models solve it — a 7B checking another 7B's facts is the blind leading the blind. |
| `critic` | Overlaps with `code_validator` and `reviewer`. Three review-adjacent identities is redundant. |
| `classifier` | Identical cognitive operation to `router`. Document that router IS a general classifier with custom grammars. |

## The Pruner Identity

### Why It Exists

The current `PruneMessages` directive is reactive — the model decides to prune
mid-generation, which is unreliable. Models don't reliably self-assess context
relevance while simultaneously performing their primary task.

A dedicated `pruner` identity solves this by:

1. **Running between chain stages** as an interstitial infrastructure step
2. **Grammar-constrained** to produce structured prune decisions
3. **Tiny model** (0.6B-1.5B) — fast, cheap, doesn't delay the pipeline
4. **Focused on ONE task** — "what in this context is no longer relevant?"

### Prune Decision Grammar (sketch)

```
root     ::= "{" ws actions ws "}"
actions  ::= "\"prune\":" ws "[" ws items ws "]"
items    ::= item ("," ws item)*
item     ::= "{" ws
               "\"message_index\":" ws number "," ws
               "\"reason\":" ws string "," ws
               "\"keep_summary\":" ws boolean
             ws "}"
           | empty
empty    ::= ""
```

### Pipeline Integration

```
stage_1 (planner) → pruner → stage_2 (searcher) → pruner → stage_3 (code_writer)
```

The pruner sees the accumulated context and produces prune decisions. The engine
applies them before the next stage loads. This keeps context lean across long
pipelines without relying on any single identity to self-manage context.

### Pruner vs. Compactor

| | Pruner | Compactor |
|---|---|---|
| **When** | Between chain stages | Context approaching limit |
| **Operation** | Remove specific messages | Summarize all messages |
| **Granularity** | Per-message keep/drop | Whole-context rewrite |
| **Frequency** | Every chain hop | Rare (threshold-based) |
| **Goal** | Keep context lean | Prevent context overflow |

They complement each other: pruner is preventive maintenance, compactor is
emergency intervention.

## Infrastructure Model Architecture

### Current State

The router model is hardcoded into a single role. It's always loaded but only
classifies. This wastes the always-loaded slot — a 0.6B model that could serve
multiple infrastructure identities sits idle between classification calls.

### Decision: Prove First, Architect Later

**Phase 1**: Build the pruner as a regular tier that swaps onto the main model
slot. Measure: does a 0.6B make good prune decisions? A 1.5B? What's the swap
cost in real pipelines?

**Phase 2 (if evidence supports it)**: Add a dedicated infrastructure model slot
separate from the router. Router stays always-loaded and isolated (proven). The
infrastructure model (also always-loaded) serves pruner and compactor identities
with zero swap cost. Three VRAM slots: router + infra + main.

**Why not unified immediately**: The adversarial analysis showed that routing
(pattern matching, 0.6B sufficient) and pruning (relevance reasoning, may need
1.5B+) have different cognitive demands. Forcing them onto one model means
either the model is too big for routing or too small for pruning. And a single
point of failure across three critical systems is an unnecessary risk.

**Why not independent tasks**: Model swap cost (~2s) for every prune call makes
interstitial pruning impractical. The whole point of the pruner is running
between chain stages cheaply.

## Pipeline Compositions

### Bug Fix Pipeline
```
diagnoser → [pruner] → planner → [pruner] → code_writer → code_validator
                                                  ↑              │
                                                  └── (fail) ────┘
```

### New Feature Pipeline
```
planner → [pruner] → searcher → [pruner] → code_writer → code_validator
                                                 → test_writer
```

### Code Review Pipeline (with expansion identities)
```
searcher → [pruner] → reviewer → code_writer (if needed)
```

### Latency Analysis

Most identities share the same model file. Identity swap = prompt+grammar change
= effectively instant. Model swap = 1-2s (only between different model files).

Bug fix pipeline on a single 7B model:
```
diagnoser (3s)
→ pruner on infra/swap (1-2s)
→ planner (3s, 0s swap — same model as diagnoser)
→ code_writer (3s, 0s swap — same model)
→ code_validator (2s, 0s swap — same model)
= ~12-14s total
```

14 seconds for a diagnosed, planned, written, and validated fix. Competitive
with single-shot generation that produces unreliable output in 5 seconds.

## MCP Exposure: Engine as Claude's Assistant

When exposed via MCP to a frontier model (Claude), the engine serves a
different role. Claude handles ambiguity and reasoning. The engine handles
reliable local execution.

**High-value identities for MCP mode:**
- `tool_runner` — Claude tells it what tools to call, engine executes reliably
- `code_writer` — Claude tells it what to write, engine produces grammar-constrained code
- `searcher` — Claude tells it what to find, engine returns structured results
- `code_validator` — Claude tells it to validate, engine returns structured pass/fail
- `extractor` — Claude tells it what to extract, engine returns structured data

**Low-value identities for MCP mode:**
- `planner`, `diagnoser`, `thinker` — Claude is strictly better at reasoning
- `conversational` — Claude IS the conversation

This doesn't change the identity set — autonomous operation still needs planners
and diagnosers. But it validates that the executor identities are the
highest-priority ones for the MCP use case.

## Reference Model Mapping (16GB VRAM)

Target hardware: 16GB VRAM GPU, 32GB+ system RAM. One main model in VRAM at
a time. Others warm in RAM via `use_mlock=True` (P2-20260302-025). Router
always co-resident.

### Model → Identity Assignments

| Model | VRAM | Identities | Swap behavior |
|-------|------|------------|---------------|
| Qwen3 0.6B | ~0.5GB (always loaded) | `router`, `quick`, `pruner` | Never swaps. Always co-resident with main model. |
| Qwen3.5 35A3B Q2 | ~8-10GB | `planner`, `code_writer`, `test_writer`, `code_validator`, `tool_runner`, `extractor`, `conversational` | Primary. 7 identities, zero swap cost between them (prompt+grammar only). |
| Qwen3 8B | ~5GB | `searcher`, `diagnoser`, `compactor` | Secondary. Swaps in for pipeline stages needing focused search/diagnosis. |
| Falcon H1R 7B | ~5GB | `thinker` | Reasoning specialist. Swaps in rarely for deep reasoning only. |

### Why This Mapping

**Qwen3.5 35A3B (primary):** MoE, 9/256 active experts per token (~3B active).
Strong instruction following, good tool calling. Gets the identities that need
highest quality output and reliable tool dispatch. 7 identities with zero swap
cost between them — prompt+grammar change only.

**Qwen3 8B (secondary):** Dense 8B, fast for focused tasks. Gets identities
where the task is narrow and well-defined (search, diagnose, summarize). Swap
from primary costs ~0.5-1s with mlock (warm RAM), ~2-3s cold.

**Falcon H1R 7B (thinker only):** Trained on long reasoning traces. 2x
inference throughput from Mamba2 architecture. Measurably better math/code
reasoning than other 7B models. But: poor tool calling, tendency to overthink,
no structured output benchmarks. Disqualified from every role except thinker.
One identity, swaps in rarely.

**Qwen3 0.6B (infrastructure):** Always loaded alongside main model.
0.5GB + 10GB = 10.5GB peak VRAM. Handles routing, quick responses, and
pruning decisions.

### VRAM Budget

```
Peak: 0.5GB (router) + 10GB (35A3B) = 10.5GB of 16GB
Swap: 0.5GB (router) + 5GB (8B or Falcon) = 5.5GB of 16GB
Headroom: 5.5-10.5GB free for KV cache + OS
```

### Pipeline Latency With Warm Loading

Bug fix pipeline (mlock-warm swaps):
```
router (0.6B, loaded)     →  ~0.5s inference
diagnoser (8B, warm)      →  ~0.5s swap + 3s inference
pruner (0.6B, loaded)     →  ~1s inference
planner (35A3B, warm)     →  ~0.5s swap + 3s inference
code_writer (35A3B, same) →  0s swap + 3s inference
code_validator (35A3B)    →  0s swap + 2s inference
                          ─────────────────────────────
                          ~14s total, 2 warm swaps
```

## Identity Contracts (Grammar Sketches)

Each identity's grammar defines its output contract. Consumers can override
grammars, but bundled defaults enforce these structures:

### Infrastructure Identities

**`router`**: Digit enum (existing, proven)
```
root ::= [0-9]
```

**`compactor`**: Summary schema
```
root    ::= "{" ws summary ws "}"
summary ::= "\"key_facts\":" ws array "," ws
             "\"preserved_tool_results\":" ws array "," ws
             "\"dropped\":" ws array
```

**`pruner`**: Prune decision (see "The Pruner Identity" section)

### Planning & Discovery

**`planner`**: Step array
```
root  ::= "{" ws "\"steps\":" ws "[" ws step ("," ws step)* ws "]" ws "}"
step  ::= "{" ws
            "\"description\":" ws string "," ws
            "\"target_files\":" ws array "," ws
            "\"dependencies\":" ws array
          ws "}"
```

**`searcher`**: Search result array
```
root    ::= "{" ws "\"results\":" ws "[" ws result ("," ws result)* ws "]" ws "}"
result  ::= "{" ws
              "\"file\":" ws string "," ws
              "\"line_start\":" ws number "," ws
              "\"line_end\":" ws number "," ws
              "\"snippet\":" ws string "," ws
              "\"relevance\":" ws string
            ws "}"
```

**`diagnoser`**: Diagnosis schema
```
root ::= "{" ws
           "\"error\":" ws string "," ws
           "\"root_cause\":" ws string "," ws
           "\"evidence\":" ws array "," ws
           "\"confidence\":" ws ("\"high\"" | "\"medium\"" | "\"low\"") "," ws
           "\"fix\":" ws string
         ws "}"
```

### Generators

**`code_writer`**: Code block only (no prose)
```
root ::= "```" language "\n" code "\n```"
language ::= "python" | "c" | "cpp" | "javascript" | "typescript" | "yaml"
code ::= [^\x60]+
```

**`test_writer`**: Test code block (same grammar as code_writer, different
prompt ensures test structure)

### Validators

**`code_validator`**: Pass/fail + findings
```
root     ::= "{" ws
               "\"verdict\":" ws ("\"pass\"" | "\"fail\"") "," ws
               "\"findings\":" ws "[" ws (finding ("," ws finding)*)? ws "]"
             ws "}"
finding  ::= "{" ws
               "\"line\":" ws number "," ws
               "\"severity\":" ws ("\"error\"" | "\"warning\"" | "\"info\"") "," ws
               "\"issue\":" ws string "," ws
               "\"suggestion\":" ws string
             ws "}"
```

### Structuring

**`extractor`**: Consumer-extensible. Bundled default: entity extraction
```
root     ::= "{" ws "\"entities\":" ws "[" ws (entity ("," ws entity)*)? ws "]" ws "}"
entity   ::= "{" ws
               "\"type\":" ws string "," ws
               "\"value\":" ws string "," ws
               "\"source\":" ws string
             ws "}"
```

### Execution

**`tool_runner`**: Tool call grammar (dynamically generated from registered
tools — the grammar enum is built at runtime from `list_tools()`)
```
root      ::= tool_call+
tool_call ::= "<tool_call>" ws "{" ws
                "\"name\":" ws tool_name "," ws
                "\"arguments\":" ws object
              ws "}" ws "</tool_call>"
tool_name ::= "\"filesystem.read_file\"" | "\"filesystem.write_file\"" | ...
```

### Conversational / Quick

**`conversational`** and **`quick`**: No grammar (free-form output).

## Allowed Tools Per Identity

| Identity | Tools | Rationale |
|----------|-------|-----------|
| `router` | None | Classification only |
| `compactor` | None | Summarizes context, no external actions |
| `pruner` | `entropic.prune_messages` | Only prune directive |
| `planner` | None | Plans, doesn't execute |
| `searcher` | `filesystem.read_file`, `filesystem.glob`, `filesystem.grep` | Read-only |
| `diagnoser` | `filesystem.*`, `bash.execute`, `diagnostics.*` | Full read + diagnostic |
| `code_writer` | `filesystem.write_file`, `filesystem.edit_file` | Write only |
| `test_writer` | `filesystem.write_file` | Write only |
| `code_validator` | `filesystem.read_file`, `bash.execute` | Read + run linter/tests |
| `extractor` | None | Pure transformation |
| `tool_runner` | All registered | Executes plans |
| `conversational` | `filesystem.read_file`, `filesystem.glob`, `filesystem.grep` | Read-only |
| `quick` | None | Sub-second response |

## Consumer Configuration

### Identity-as-Tier Model

Each bundled identity becomes a tier name. The consumer's config maps
identities to model files:

```yaml
models:
  default_model: ~/models/gguf/qwen3.5-35a3b-q2.gguf
  tiers:
    router:
      path: ~/models/gguf/qwen3-0.6b-q4.gguf
    quick:
      path: ~/models/gguf/qwen3-0.6b-q4.gguf
    pruner:
      path: ~/models/gguf/qwen3-0.6b-q4.gguf
    searcher:
      path: ~/models/gguf/qwen3-8b-q4.gguf
    diagnoser:
      path: ~/models/gguf/qwen3-8b-q4.gguf
    # All other identities use default_model
```

Identities not explicitly configured inherit `default_model`. This means
a consumer with ONE model path gets all 13 identities working.

### Override Mechanism

Any identity attribute can be overridden per consumer:

```yaml
tiers:
  code_writer:
    grammar: my_custom_grammar.gbnf   # Override bundled grammar
    max_output_tokens: 512             # Override default
    allowed_tools:                     # Override default tool set
      - filesystem.write_file
      - filesystem.edit_file
      - bash.execute                   # Consumer adds bash access
```

Bundled defaults come from identity frontmatter. Config is overrides only.

### Identity Frontmatter Format

Each identity `.md` file carries metadata that the prompt manager reads:

```yaml
---
name: code_writer
grammar: grammars/code_writer.gbnf
auto_chain: code_validator
allowed_tools:
  - filesystem.write_file
  - filesystem.edit_file
max_output_tokens: 256
temperature: 0.3
enable_thinking: false
model_preference: primary    # primary | secondary | infrastructure | any
---

{system prompt content}
```

`model_preference` is advisory — tells the router which model file this
identity runs best on. Not enforced; consumer config overrides.

## Router Expansion

### Classification Strategy

Current router classifies 4-5 categories. Expanding to 7+ (the 13 identities
group into 7 top-level routing categories):

| Category | Identities routed to | Trigger patterns |
|----------|---------------------|------------------|
| 0: quick | `quick` | Greetings, thanks, one-word responses |
| 1: plan | `planner` | "How should I...", task decomposition |
| 2: search | `searcher` | "Find...", "Where is...", "Show me..." |
| 3: diagnose | `diagnoser` | Error messages, "Why does...", stack traces |
| 4: write_code | `code_writer` | "Write a function...", "Implement..." |
| 5: validate | `code_validator` | "Check...", "Review...", "Is this correct..." |
| 6: general | `conversational` | Everything else |

Pipeline-internal identities (`pruner`, `compactor`, `test_writer`,
`tool_runner`, `extractor`) are NOT routed to — they're reached via
`auto_chain` from other identities.

### 0.6B Capacity Risk

7 categories is within 0.6B capacity for digit classification with
clear prompt engineering. The router doesn't need to understand the
request — just pattern-match surface features.

**Failure definition**: Classification accuracy < 80% on a 100-prompt
test suite. Measured per-category (a category consistently misclassified
fails even if overall accuracy is high).

**Escalation path**:
1. Prompt refinement (reword category descriptions)
2. Few-shot examples in prompt (3-5 per category)
3. Hierarchical routing: coarse 3-way split → fine 2-3 way split
4. Upgrade to 1.5B (last resort — increases VRAM co-residency budget)

### Router Test Suite

Create `tests/model/test_router_classification.py` with 100+ prompts:
- 15+ per category, balanced
- Edge cases: ambiguous prompts that could go multiple ways
- Adversarial: prompts that look like one category but are another
- Minimum per-category accuracy: 80%
- Overall accuracy: 85%

## Interstitial Pruner Design

### Engine Changes

```python
# New TierConfig field
class TierConfig(BaseModel):
    interstitial: bool = False  # Not routable, runs between chain hops

# New pipeline config
class PipelineConfig(BaseModel):
    interstitials: list[str] = []  # Tier names to run between hops
    # e.g., ["pruner"] → pruner runs after every auto_chain hop
```

### Engine Loop Integration

```
Current:  tier_A generates → auto_chain → tier_B generates
Proposed: tier_A generates → [interstitials] → tier_B generates
```

The interstitial step:
1. Engine detects auto_chain trigger
2. Before switching to target tier, runs each interstitial in order
3. Interstitial generates with its own identity/grammar
4. Engine applies interstitial output (e.g., prune decisions → PruneMessages)
5. Then proceeds to target tier

Interstitials are optional. Empty `interstitials` list = current behavior.

### Directive Mapping

Pruner output → `PruneMessages` directive:
```python
# Pruner grammar output: {"prune": [{"message_index": 3, ...}]}
# Engine parses → PruneMessages directive → removes messages
```

## Engine Changes Required

### Config Schema

The current `TierConfig` already supports most needed fields (`identity`,
`grammar`, `auto_chain`, `allowed_tools`, `enable_thinking`, temperature,
`max_output_tokens`). New fields needed:

| New field | Where | Purpose |
|-----------|-------|---------|
| `interstitial: bool` | TierConfig | Mark tier as non-routable |
| `model_preference: str` | TierConfig | Advisory model assignment |
| `interstitials: list[str]` | PipelineConfig (new) | Interstitial tier order |
| `default_model: Path` | ModelsConfig | Fallback for unconfigured tiers |

### Prompt Manager Changes

The prompt manager must:
1. Discover bundled identity files from package data
2. Parse identity frontmatter for defaults
3. Merge with consumer config overrides (config wins)
4. Build grammar file paths (bundled or consumer-provided)

### Router Changes

1. Expand classification prompt from 4-5 to 7 categories
2. Digit grammar stays (proven, trivial)
3. Category descriptions derived from identity frontmatter `name` + first
   paragraph of system prompt

| Concern | Current state | Needed |
|---------|---------------|--------|
| Router classification | 4-5 categories | 7 top-level (see Router Expansion) |
| VRAM management | Swap on tier change | Model reuse critical (P1-022) |
| Grammar file management | 1 grammar (pychess) | 10+ grammar files bundled in package |
| Identity file management | 4 identity .md files | 13 identity .md files |
| Config defaults | Consumer defines all tiers | Identity frontmatter + default_model |

## Acceptance Criteria

### Per-Identity
- [ ] All 13 shipped identities have: system prompt (.md) with frontmatter,
      grammar (.gbnf where applicable), and allowed_tools list
- [ ] Each grammar-constrained identity (10 of 13) produces valid output
      matching its grammar on 5+ test prompts (model test)
- [ ] Each identity's allowed_tools is enforced (unit test)

### Router
- [ ] Router classifies 7 categories with >= 80% per-category accuracy
      on 100+ prompt test suite (model test)
- [ ] Fallback: ambiguous prompts route to `conversational` (not error)

### Pipelines
- [ ] At least 3 pipeline compositions work end-to-end (model test):
      bug fix, new feature, code review
- [ ] Interstitial pruner runs between chain stages (model test)
- [ ] Interstitial config is optional (empty = current behavior)

### Infrastructure
- [ ] Grammar files bundled with package (`src/entropic/data/grammars/`)
- [ ] Identity files bundled with package (`src/entropic/data/prompts/`)
- [ ] Prompt manager discovers bundled identities at runtime
- [ ] Consumer config overrides bundled defaults
- [ ] `default_model` config field provides single-path setup

### Documentation
- [ ] Consumer guide documents: identity library, pipeline composition,
      config override mechanism, grammar customization
- [ ] Each expansion identity documented with promotion criteria
- [ ] Documented patterns (7+) have example grammar files

### Regression
- [ ] All existing unit and model tests pass without modification

## Implementation Plan

### Phase 1: Core Identities + Grammars
- Write grammar files for all 10 grammar-constrained shipped identities
- Write identity .md files with frontmatter for all 13 shipped identities
- Bundle in package (`src/entropic/data/grammars/`, `src/entropic/data/prompts/`)
- Unit tests for grammar parsing (valid GBNF, correct structure)

### Phase 2: Router Expansion
- Expand router classification prompt for 7+ categories
- Model test coverage for all routing paths
- Fallback behavior when classification is ambiguous
- Validate 0.6B capacity; escalate to 1.5B if needed

### Phase 3: Pipeline Integration
- Wire auto_chain defaults from identity frontmatter
- End-to-end model tests for bug fix, new feature, and code review pipelines
- Latency benchmarks for multi-stage pipelines

### Phase 4: Pruner (Experimental)
- Build pruner as regular tier (swaps onto main model slot)
- Test prune decision quality at 0.6B, 1.5B, 3B
- Measure swap cost impact on pipeline latency
- If validated: add interstitial tier support to engine loop
- If swap cost problematic: propose dedicated infra model slot (Phase 2 of
  infrastructure architecture)

### Phase 5: Expansion Identities
- Promote `guardrail`, `thinker`, `summarizer`, `reviewer`, `reflector`
  based on evidence from Phases 1-4
- Each promotion requires: grammar file, identity prompt, model test, doc update

### Phase 6: TUI Overhaul
- Integrate all identities into TUI workflows
- Pipeline visualization (active identity, output, chain progress)
- Pipeline inspector mode for debugging chain behavior
- TUI becomes the full-featured reference implementation

## Risks & Considerations

- **Router capacity**: 0.6B model classifying 7+ categories may degrade.
  Mitigation: test first, escalate to 1.5B or hierarchical routing if needed.
- **VRAM pressure**: More identities means more potential model swaps. Most
  identities share the same model file — model reuse logic is critical.
  Identity swap (prompt+grammar) is free; only model swap costs time.
- **Grammar maintenance**: 10+ grammar files is significant surface area.
  Each grammar change needs corresponding test updates.
- **Pipeline latency**: Multi-stage pipelines add inference round-trips. Narrow
  scope + low max_output_tokens + shared model files keep each stage fast
  (2-3s). Total pipeline latency is competitive with single-shot generation
  that produces unreliable output.
- **Pruner quality**: Unproven that small models make good prune decisions.
  Built as experimental; validated before infrastructure architecture changes.
- **Consumer complexity**: 13 identities manageable with zero-config defaults.
  Identity frontmatter carries defaults; config is overrides only. Consumer who
  sets one model path gets all identities working.

## Implementation Log

{Entries added as work progresses}

## References

- Grammar + auto-chain interaction: P1-20260226-022 (VRAM orchestration proposal)
- Pychess example: demonstrates grammar + auto_chain + multi-tier pipeline
- Grammar-constrained extraction research: outperforms unconstrained and
  finetuned models (arxiv.org/abs/2305.13971)
- DSPy modules: decomposition and multi-chain comparison patterns
- CrewAI/AutoGen: multi-agent role specialization patterns
