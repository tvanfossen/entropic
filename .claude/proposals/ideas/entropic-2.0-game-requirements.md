# Entropic 2.0.0 Roadmap Context: First-Party Consumer Requirements

## Source

Distilled from design session on 2026-03-10. Full game proposal: `.claude/proposals/IDEAS/P2-20260310-001.md` (in self_assessment repo).

## What This Document Is

A first-party application (isometric turn-based CRPG) is planned that consumes Entropic as a library. This document captures engine-level requirements surfaced during game design. **Entropic is NOT a game engine.** It is a local-first agentic inference engine. Every feature here must be designed as a general-purpose engine capability. The game is one consumer alongside the TUI, Pathfinder Society Scribe, and future SDK customers.

This document is an **input to the 2.0.0 roadmap**, not the roadmap itself. The roadmap Claude should treat this as requirements from a demanding first-party consumer, weighed against other priorities.

## C++ Rewrite

The 2.0.0 release is a C++ rewrite of the Entropic engine. The Python codebase (30K+ LOC, 800+ tests) validates the architecture; C++ is the shipping product.

### Why C++

- First-party game integrates with Godot via GDExtension (C++ native)
- Eliminates Python runtime overhead — every MB of VRAM matters on consumer GPUs (8-12 GB target)
- Performance-critical inference paths (model loading, token generation, VRAM orchestration) benefit from native execution
- C/C++ API enables broader consumer ecosystem (game engines, embedded systems, robotics)

### Full Python Feature Inventory (Must Port)

Every subsystem below exists in the Python codebase and must have a C++ equivalent in 2.0.0. This is the rewrite scope.

#### Core Engine

| Subsystem | Key Classes/Modules | Notes |
|---|---|---|
| **Agentic execution loop** | `AgentEngine.run()`, `AgentState` enum | State machine: IDLE → PLANNING → EXECUTING → WAITING_TOOL → VERIFYING → COMPLETE/ERROR. Max iterations, consecutive error limits, idle timeout. |
| **Streaming generation** | `on_stream_chunk` callback | Per-token callback during generation. Consumers use this for real-time output display. |
| **Context management** | Message list, context anchors (keyed persistent messages) | Anchors survive compaction and tier changes. Re-injected automatically. |
| **Context compaction** | `CompactionConfig` | Summarizes context when approaching limits. See new requirement: compaction hooks. |
| **Delegation** | Child loops, depth tracking, parent/child conversation IDs | One model spawns a sub-task handled by another. Existing pattern is sound. |
| **Directives** | `_directives` key in tool results | Tool results can signal: tier_change, stop_processing, context_anchor injection. Engine processes these between tool calls. |
| **Interruption** | Thread-safe cancel/pause events | Async-safe interruption of inference or tool execution. |
| **Tier locking** | `ctx.locked_tier` | Prevents mid-task model switching once set. |
| **Loop config** | `LoopConfig` | max_iterations, max_consecutive_errors, max_tool_calls_per_turn, idle_timeout_seconds, stream_output, auto_approve_tools |
| **Metrics collection** | Per-loop timing, token counts | Collected during and after loop execution. |

#### Inference

| Subsystem | Key Classes/Modules | Notes |
|---|---|---|
| **Model orchestration** | `ModelOrchestrator` | Three-state lifecycle: COLD ↔ WARM ↔ ACTIVE. keep_warm config. Single asyncio lock guards GPU swaps. |
| **Backend deduplication** | Shared GGUF → shared backend instance | Multiple tiers on same .gguf share one backend. Avoids duplicate compute buffers (~1.1 GB each). |
| **Tier-based routing** | Router model (0.6B), classification prompt, tier_map, tier history | Router generates single digit, log-prob confidence, fallback to default on invalid output. ~100-300ms. |
| **Chat adapters** | `ChatAdapter`, `get_adapter()`, `register_adapter()` | Per-model chat template handling (Qwen vs Falcon vs etc.). Adapter parses tool calls from model output. |
| **GBNF grammar constraints** | Per-tier grammar files | Grammar-constrained generation. PyChess example demonstrates per-tier grammars. |
| **Generation config** | `GenerationConfig` | max_tokens, temperature, top_p, top_k, repeat_penalty defaults. Per-generation overrides. |
| **MoE model support** | Via llama.cpp | Mixture of Experts models (e.g. Qwen3 35A3B — 3B active, 35B total). Assume llama.cpp handles MoE correctly. VRAM accounting must reflect total params loaded, not just active params. |

#### MCP

| Subsystem | Key Classes/Modules | Notes |
|---|---|---|
| **Server manager** | `ServerManager` (singleton) | Manages all servers — built-in (in-process) and external (stdio/socket). |
| **Built-in servers** | FilesystemServer, BashServer, GitServer, DiagnosticsServer, WebServer, EntropicServer | In-process MCP servers for core tool capabilities. |
| **External servers** | stdio and socket transports | From YAML config, .mcp.json discovery (project + global), runtime registration. |
| **Tool registry** | Merged across all servers | JSON tool definitions at `data/tools/{server_name}/{tool_name}.json`. |
| **Tool execution** | `ToolExecutor` | Validates permissions, checks tool existence, executes, handles errors, injects result into context. |
| **Permissions** | allow/deny/auto_approve with glob patterns | See new requirement: per-caller key authorization. |
| **Directives in tool results** | `_directives` key | Tools return directives that signal engine actions. |

#### Prompts

| Subsystem | Key Classes/Modules | Notes |
|---|---|---|
| **Prompt manager** | `PromptManager` | Loads and manages prompt files. |
| **Frontmatter parsing** | YAML frontmatter in markdown files | `PromptFrontmatter`, `IdentityFrontmatter`, `ConstitutionFrontmatter`, `AppContextFrontmatter` |
| **Tier identity** | `load_tier_identity()`, `TierIdentity` | Per-tier system prompts with personality, focus areas, few-shot examples. Built from config + identity markdown files. |
| **Constitution** | Shared system prompt across tiers | Behavioral guardrails. |

#### Configuration

| Subsystem | Key Classes/Modules | Notes |
|---|---|---|
| **Config loader** | `ConfigLoader`, `validate_config` | YAML config, Pydantic validation. C++ equivalent needs schema validation. |
| **Tier config** | `TierConfig` | path, adapter, context_length, gpu_layers, keep_warm, allowed_tools, auto_chain, routable |
| **Models config** | `ModelsConfig` | Tiers + router configuration. |
| **Routing config** | `RoutingConfig` | Router behavior parameters. |
| **MCP config** | `MCPConfig` | Server enables, external server definitions, filesystem config, timeouts. |
| **Permission save** | `save_permission()` | "Always allow" persisted to local config. |

#### Storage & Logging

| Subsystem | Notes |
|---|---|
| **Conversation persistence** | aiosqlite in Python. C++ needs equivalent — SQLite direct or similar. |
| **Logging** | Structured logging with model logger, display logger. |

#### Bundled Toolchains

| Toolchain | Notes |
|---|---|
| **Benchmark runner** | `entropic benchmark run <model.gguf>` — measures load/swap/inference timing, VRAM usage. Must port. |
| **LSP client** | Diagnostics integration for code analysis tools. |

### Public API Surface (Must Preserve)

The Python `__init__.py` exports define the public API contract. The C++ API must expose equivalent functionality:

- **Core types:** GenerationResult, Message, ModelBackend, ModelTier, ToolCall, ToolProvider, ToolResult
- **Engine:** AgentEngine, AgentState, EngineCallbacks, LoopConfig
- **Config:** ConfigLoader, validate_config, all config types
- **Orchestration:** BackendFactory, ModelOrchestrator, RoutingResult
- **Adapters:** ChatAdapter, get_adapter, register_adapter
- **MCP:** BaseMCPServer, BaseTool, InProcessProvider, ServerManager, ServerResponse, ToolRegistry, load_tool_definition
- **Prompts:** PromptManager, load_tier_identity, parse_prompt_file, all frontmatter types
- **Logging:** setup_logging, setup_model_logger, setup_display_logger
- **Python bindings:** pybind11 or similar wrapping the C++ API for existing Python consumers (TUI, Scribe)

---

## New Engine Features (v2.0.0)

Features that don't exist in the current Python codebase but are required for 2.0.0.

### 1. Per-Caller MCP Authorization with Runtime Key Grants (P1)

**Current state:** Global or per-tier allow/deny/auto_approve with glob patterns in static config.

**Required:** Per-caller (per-identity, per-agent) MCP access control with runtime-grantable keys.

**Engine design:**
- Each caller has a discrete set of authorized MCP keys, not just tier-level globs
- Keys support access levels (READ, WRITE) per tool/resource
- Keys are grantable and revocable at runtime via MCP tool calls (one caller grants keys to another)
- Engine enforces key boundaries — a caller cannot invoke tools or access resources outside its key set
- Key sets are serializable (persist across sessions, saveable as part of application state)

**General-purpose value:**
- Multi-user TUI: different users have different tool access
- Robotics: different subsystems have different actuator/sensor permissions
- Multi-agent workflows: scoped tool access per agent role
- Any deployment where "who can do what" needs to be dynamic, not static config

### 2. LoRA Adapter Hot-Swapping (P1)

**Current state:** Entropic loads/unloads entire models per tier. llama-cpp-python supports LoRA but Entropic doesn't expose adapter management.

**Required:** Load a base model once, swap LoRA adapters without unloading the base model.

**Engine design:**
- `ModelBackend` gets adapter lifecycle: `load_adapter(path)`, `unload_adapter()`, `swap_adapter(path)`
- Adapter metadata (name, target base model, rank, size) accessible for routing/scheduling
- Tier config supports `adapter_path` alongside `model_path`
- Multiple adapters can be warm in RAM while only one is active on GPU — mirrors COLD/WARM/ACTIVE pattern for adapters
- Adapter swap latency target: ~50-200ms (RAM to GPU transfer of adapter weights only)

### 3. Per-Generation Grammar Override (P1)

**Current state:** Grammars are configured per-tier in static config.

**Required:** Same model, different grammar per generation call. A single model instance may need to produce dialogue (one grammar), then a tool call (different grammar), then a combat action (different grammar) — all without reloading.

**Engine design:**
- Generation call accepts optional grammar parameter that overrides tier default
- Runtime grammar loading (no restart required)
- Grammar validation API (is this grammar parseable? tokenizer-compatible?)
- Grammar registry: named grammars loadable by key, registered at runtime

### 4. MCP Tool Call Structured Audit Log (P2)

**Current state:** Tool calls are logged but not in a structured, replayable format.

**Required:** Complete audit trail of all MCP tool calls in a structured, appendable, replayable format.

**Engine design:**
- Structured log format (JSON lines or equivalent): timestamp, caller_id, tool_name, params, result, directives
- Appendable log file per session/conversation
- `replay_log(path)` API: re-execute a tool call sequence against current server state
- Filtering: by caller_id, tool_name, time range
- Consumers use this for: debugging, state reconstruction, reproducibility, undo/replay workflows

### 5. Perplexity / Log-Probability Evaluation (P2)

**Current state:** Log-probs extracted for router classification confidence only.

**Required:** Evaluate arbitrary token sequences against a loaded model and return per-token log-probabilities without generating new tokens.

**Engine API:**
- `compute_perplexity(model_id, token_sequence) -> float`
- `get_logprobs(model_id, token_sequence) -> vector<float>`
- Evaluation-only mode — no generation, no KV cache mutation
- Must work on any loaded model (ACTIVE state), any tier

### 6. Compaction Hooks (P2)

**Current state:** Compaction auto-triggers when context approaches limits. No consumer visibility or control.

**Required:** Compaction is invokable by consumers, with hook/callback registration for pre and post compaction events, and support for custom compactor registration.

**Engine design:**
- `compact(identity_id)` — programmatically invoke compaction on a specific identity's context
- `on_pre_compact(callback)` — hook before compaction runs (consumer can inspect/modify input)
- `on_post_compact(callback)` — hook after compaction completes (consumer can inspect/modify result)
- `register_compactor(compactor)` — consumer provides a custom compaction implementation that replaces or wraps the default
- Default compactor (existing summarization approach) remains the fallback

### 7. Constitutional Training Toolchain (P2)

**Current state:** No training/fine-tuning tooling ships with Entropic. Benchmark runner and judge toolchain exist as bundled tools.

**Required:** A bundled toolchain (similar to `entropic benchmark`) for constitutional synthetic data generation and LoRA fine-tuning. This is not the engine's core inference path — it's a companion tool that ships with the distribution.

**Toolchain scope:**
- **Data generation:** Use a loaded model (teacher) to generate synthetic training data with grammar constraints and constitutional filtering rules
- **Constitutional filtering:** Rule-based rejection of outputs that violate constitutional constraints (configurable per use case — not game-specific)
- **LoRA fine-tuning:** Orchestrate adapter training on generated data against a base model
- **Validation:** Evaluate fine-tuned adapter quality against a test set

**Interface:** CLI (`entropic train ...`) and programmatic API. Consumers provide: teacher model, seed data, constitutional rules, base model for fine-tuning. Toolchain orchestrates the pipeline.

### 8. Dynamic Identity/Tier Creation at Runtime (P1)

**Current state:** Tiers are defined statically in config YAML at startup. The set of identities is fixed for the session.

**Required:** Create, configure, and destroy identities at runtime. The game's BBEG creates NPC identities during world generation — each needs a runtime-created identity with system prompt, grammar, MCP key set, and optional LoRA adapter. The tester benefits for adding product variants or procedure variants without restart.

**Engine API:**
- `create_identity(name, system_prompt, grammar_id, mcp_keys, adapter_path?) -> identity_id`
- `update_identity(identity_id, ...)` — modify an existing identity's config
- `destroy_identity(identity_id)` — unload adapter, release keys, remove from tier registry
- Identity count bounded by config (max_identities) to prevent unbounded resource allocation
- Created identities participate in tier routing same as statically-configured tiers

**General-purpose value:** Any multi-agent deployment where agents are spawned/retired dynamically — not just games. Robotics (new sensor module added at runtime), multi-user systems (new user session), workflow orchestration (new task role).

### 9. Dynamic Grammar Generation from String (P1)

**Current state:** Grammars loaded from GBNF files on disk, referenced by path in tier config.

**Required:** Register grammars from in-memory strings, not just file paths. The game's BBEG generates "laws of the universe" which are grammar schemas — produced by the model during world generation via `world.set_law` MCP tool. These grammars must be registered with the engine immediately and usable for subsequent generation calls.

**Engine API:**
- `register_grammar(name, gbnf_string) -> grammar_id` — from string (new)
- `register_grammar(name, path) -> grammar_id` — from file (existing pattern)
- `unregister_grammar(grammar_id)` — remove from registry
- Grammar validation on registration (parseable? tokenizer-compatible?)
- Registered grammars usable in per-generation grammar override and identity config

### 10. Dynamic MCP Tool Registration Refresh (P2)

**Current state:** MCP tools discovered at startup via `tools/list`. Tool set is fixed for the session.

**Required:** Support servers declaring new tools mid-session. The game's world MCP server exposes new tools as the world is built — BBEG creates a location, new location-specific tools appear. The engine's ToolRegistry must refresh when a server signals updated capabilities.

**Engine design:**
- `ToolRegistry.refresh(server_id)` — re-query a server's `tools/list` and update the registry
- Optionally: servers can push notifications when their tool set changes (MCP protocol supports this)
- Loaded models see updated tool lists on their next generation call
- Tools removed by a server are cleaned up from the registry (pending tool calls complete first)

### 11. MCP Server Health / Reconnection (P2)

**Current state:** External MCP server disconnection behavior is undefined.

**Required:** Resilience for external MCP servers that may disconnect mid-session (hardware flakiness for the tester, game world server crash for the game).

**Engine design:**
- Detect disconnection (transport-level health check)
- Configurable reconnection with backoff (per-server config)
- Surface disconnection/reconnection events through hook system (`on_server_disconnect`, `on_server_reconnect`)
- Tool calls to disconnected servers return structured error (not crash), holdable via hook for retry

### 12. Batched Inference — Multiple Sequences per Forward Pass (P1)

**Current state:** Entropic processes one sequence at a time through a loaded model. Each generation call is a single prompt → single response.

**Required:** Process multiple independent sequences through the same model weights in a single forward pass. Each sequence maintains its own context, KV cache, and (for hybrid models) SSM state.

**Engine API:**
- `batch_generate(model_id, sequences: vector<GenerationRequest>) -> vector<GenerationResult>`
- Each `GenerationRequest` carries its own messages, grammar, generation config, and sequence ID
- Results are correlated by sequence ID
- Batch size bounded by available VRAM (KV cache / SSM state per sequence)
- `get_max_batch_size(model_id) -> int` — query how many concurrent sequences fit given current VRAM

**General-purpose value:**
- Multi-user TUI: concurrent users on same model without sequential bottleneck
- Multi-agent workflows: parallel agent reasoning in one forward pass
- Robotics: multiple sensor interpretation streams processed simultaneously
- Any deployment where throughput matters more than single-sequence latency

### 13. Per-Sequence State Isolation (P1)

**Current state:** Model state (KV cache) is tied to the single active sequence. No mechanism to maintain multiple independent conversation states on one loaded model.

**Required:** Each sequence in a batch (or across sequential calls) maintains its own isolated state — KV cache for transformer layers, SSM recurrent state for hybrid Mamba layers.

**Engine API:**
- `create_sequence(model_id) -> sequence_id` — allocate isolated state for a new sequence
- `destroy_sequence(sequence_id)` — free state
- `get_sequence_state(sequence_id) -> bytes` — serialize state (KV cache + SSM state) for persistence
- `set_sequence_state(sequence_id, state: bytes)` — restore state
- `list_sequences(model_id) -> vector<SequenceInfo>` — query active sequences and their memory usage
- State serialization to/from disk for session persistence

**General-purpose value:**
- Multi-user: each user has persistent conversation state on shared model
- Multi-agent: each agent maintains independent memory/context
- Robotics: per-subsystem state on shared inference model
- Session resume: serialize state, reload later without replaying full context

**Relationship to Feature #12 (Hybrid SSM State Management):** This supersedes #12 by generalizing it. SSM state management becomes a subset of per-sequence state isolation — the engine manages all per-sequence state uniformly (KV cache + SSM state), not just SSM state as a special case. Feature #12 is absorbed into this requirement.

### 14. Multi-Grammar Generation Pipeline (P1)

**Current state:** Each generation call uses one grammar (per-tier default or per-generation override from Feature #3). No mechanism to chain multiple grammar-constrained passes within a single logical turn.

**Required:** Define a pipeline of generation passes where each pass uses a different grammar and the output of each pass is available as context for subsequent passes. The engine orchestrates the chain; the consumer defines the grammar sequence and how outputs connect.

**Engine API:**
- `PipelineConfig` — ordered list of `PipelineStage` (grammar_id, generation_config, output_injection_mode)
- `output_injection_mode`: how this stage's output enters context for the next stage — `append_as_assistant`, `append_as_system`, `replace_last`, `custom_hook`
- `pipeline_generate(model_id, sequence_id, stages: vector<PipelineStage>) -> vector<GenerationResult>`
- Pipeline is atomic from the consumer's perspective — all stages complete or the pipeline fails
- Hooks: `on_pipeline_stage_complete` fires between stages, allowing consumer inspection/modification

**General-purpose value:**
- Chain-of-thought → structured output: reason freely (no grammar), then produce structured result (grammar-constrained)
- Multi-format output: generate analysis (text grammar), then action plan (JSON grammar), then summary (text grammar)
- Robotics: sensor interpretation (classification grammar) → action planning (command grammar) → safety validation (boolean grammar)
- Any workflow where a single model needs to produce multiple structured outputs that inform each other

### 15. Blackwell INT4/FP4 Tensor Core Targeting (P2)

**Current state:** Entropic/llama.cpp builds target generic CUDA architectures. No specific optimization for Blackwell's FP4/INT4 tensor cores (sm_120 for consumer, sm_100 for datacenter).

**Required:** Build and deployment configuration that targets Blackwell-native quantization formats for optimal inference performance on RTX 5xxx, RTX PRO Blackwell, and B-series datacenter GPUs.

**Engine design:**
- **Runtime detection only** — query GPU compute capability at startup, select optimal kernel path automatically. The engine NEVER enforces or mandates a specific CUDA architecture.
- Build system compiles for multiple architectures (fat binary or JIT). Consumer's hardware determines the code path, not build flags.
- Support MXFP4 and NVFP4 quantization formats when available in llama.cpp upstream
- Graceful degradation: FP4 on Blackwell, FP8/INT8 on Hopper/Ada, INT4/Q4 on older. Same model, best available precision for the hardware.

**General-purpose value:**
- Every consumer deploying on 2025+ NVIDIA hardware benefits from ~2× effective VRAM density and FP4 tensor core throughput
- Not game-specific — any Entropic deployment on Blackwell hardware

### 16. Vector Store Integration Hooks (P2)

**Current state:** No embedding or retrieval capability in the engine. Consumers needing RAG must implement it entirely outside the engine.

**Required:** Hook points for embedding generation and semantic retrieval, integrated into the context assembly pipeline. The engine provides the integration points; consumers provide the embedding model, vector store implementation, and retrieval logic.

**Engine design:**
- `on_pre_context_assemble` hook extended: consumer callback receives the current context and can inject retrieved documents before the message list is finalized
- `EmbeddingProvider` interface: consumer registers an embedding function (`embed(text) -> vector<float>`)
- `RetrievalProvider` interface: consumer registers a retrieval function (`retrieve(query_embedding, top_k) -> vector<Document>`)
- Engine calls `retrieve()` during context assembly if a provider is registered, injecting results via the hook
- Engine provides `embed()` as a utility for consumers to build their stores (can use the loaded model's embeddings or a separate model)
- No vector store implementation in the engine — consumer provides (FAISS, Annoy, SQLite-VSS, custom)

**General-purpose value:**
- RAG for any consumer (TUI with document retrieval, code search, knowledge bases)
- Robotics: retrieve relevant procedures/manuals based on current sensor state
- Multi-agent: shared knowledge base across agents with per-agent retrieval scoping
- The engine doesn't own the store — it owns the integration point. Consumers bring their own storage.

### 17. Inference Backend Abstraction (P1)

**Current state:** Inference is tightly coupled to llama.cpp / GGUF throughout the engine.

**Required:** `InferenceBackend` as a concrete base class that owns model lifecycle, state management, and capability queries. `LlamaCppBackend` is the first (and only 2.0.0) subclass. Future backends (Axera AXCL, RKNN, TFLite, ONNXRuntime) are subclasses added without engine refactoring.

**Design (concrete base class pattern — base holds 80%+ of logic):**

```cpp
class InferenceBackend {
public:
    // Base class implements: lifecycle management, state tracking,
    // capability caching, error handling, metrics, logging
    ModelHandle load_model(const ModelConfig& config);
    void unload_model(ModelHandle handle);
    GenerationResult generate(ModelHandle, SequenceId, const GenerationRequest&);
    std::vector<GenerationResult> batch_generate(ModelHandle, const std::vector<GenerationRequest>&);
    SequenceState get_sequence_state(SequenceId);
    void set_sequence_state(SequenceId, const SequenceState&);
    BackendCapabilities get_capabilities();

protected:
    // Subclasses override ONLY these:
    virtual RawModelHandle do_load(const std::string& path, const QuantConfig&) = 0;
    virtual void do_unload(RawModelHandle) = 0;
    virtual RawOutput do_inference(RawModelHandle, const TokenSequence&, const GrammarConstraint*) = 0;
    virtual QuantFormats supported_formats() = 0;
};

class LlamaCppBackend : public InferenceBackend { /* GGUF, CUDA/CPU/Vulkan */ };
// Future: class AxeraBackend : public InferenceBackend { /* .axmodel, AXCL/NPU */ };
// Future: class RKNNBackend : public InferenceBackend { /* .rknn, Rockchip NPU */ };
```

**Why P1 / 2.0.0 (not 3.0.0):**
- Designing the abstraction into the C++ rewrite from the start is trivial. Retrofitting it later requires refactoring every inference callsite.
- Only `LlamaCppBackend` ships in 2.0.0. The abstraction costs nothing — it's the same code, organized behind a base class.
- DRY/KISS: the right abstraction early prevents the wrong coupling later.
- Known future backends (Axera for robotics, potential RKNN, TFLite for mobile) confirm the abstraction isn't speculative.

**General-purpose value:**
- Heterogeneous deployment: desktop (CUDA), embedded (NPU), mobile (TFLite), cloud (TensorRT)
- Same engine, same identity/MCP/grammar/hooks stack, different hardware
- Consumer chooses backend at initialization, everything above it works identically

### 18. Hybrid SSM/Transformer State Management (P3 — FUTURE)

**Current state:** Entropic targets GGUF models via llama.cpp. Hybrid Mamba-Transformer models (Falcon H1R, Jamba, RWKV-6/7, Zamba) have partial/experimental llama.cpp support.

**Not a v2.0.0 blocker.** Track upstream llama.cpp progress. When mature:

- `get_recurrent_state(model_id) -> bytes` — extract hidden state after inference
- `set_recurrent_state(model_id, state)` — restore hidden state before next inference
- State serialization to/from disk
- Enables persistent memory across sessions without context window pressure

RWKV-6 at 1.6B is the closest available candidate today.

### 9. Voice Integration Hooks (P3 — FUTURE)

**Current state:** PersonaPlex integration exists in the Python voice module.

**Not a v2.0.0 blocker.** Entry points only:

- Pre-inference callback: audio input → consumer-provided processing (STT or S2S) → text/tokens for engine
- Post-inference callback: engine output → consumer-provided processing (TTS or S2S) → audio output
- Engine provides hooks. Consumers provide models and audio I/O. No specific voice model baked into the engine.

---

## Systematic Hook/Callback Architecture (P1)

Every stage transition in the engine's pipeline must be a hook point. Consumers customize behavior at any stage without forking the engine. This is what makes Entropic an engine rather than an application — the pipeline is defined by the engine, the behavior at each stage is defined by the consumer.

### Design Principles

- **All hooks are optional.** No registered callback = default engine behavior. Zero overhead when unused.
- **Pre-hooks can modify or cancel.** Return a modified input to change behavior, or return a cancellation signal to skip the stage.
- **Post-hooks can inspect and transform.** Receive the stage output, optionally transform it before it passes downstream.
- **Multiple callbacks per hook point.** Registration order defines execution order. Chain of responsibility pattern.
- **Hooks must not break the pipeline.** A failing hook callback is logged and skipped — it never crashes the engine. Consumer bugs don't take down inference.

### Hook Points by Pipeline Stage

#### Model Lifecycle

| Hook | Fires When | Pre-Hook Can | Post-Hook Receives |
|---|---|---|---|
| `on_model_load` | Model transitions COLD → WARM or WARM → ACTIVE | Cancel load, modify load params | Model ID, load timing, VRAM consumed |
| `on_model_unload` | Model transitions ACTIVE → WARM or WARM → COLD | Cancel unload (keep model loaded) | Model ID, VRAM freed |
| `on_adapter_swap` | LoRA adapter swap on loaded base model | Cancel swap, redirect to different adapter | Adapter ID, swap timing |
| `on_vram_pressure` | VRAM usage exceeds configurable threshold | Suggest which model to evict | Current VRAM state, loaded models |

#### Tier Routing

| Hook | Fires When | Pre-Hook Can | Post-Hook Receives |
|---|---|---|---|
| `on_route` | Router classifies input to a tier | Override tier selection, force a specific tier | Selected tier, confidence, router logprobs |

#### Generation (Inference)

| Hook | Fires When | Pre-Hook Can | Post-Hook Receives |
|---|---|---|---|
| `on_pre_generate` | Before inference call to model | Modify prompt/messages, change grammar, change generation params, cancel | Final message list, grammar, generation config |
| `on_stream_token` | Each token emitted during streaming generation | N/A (observation only, must not block) | Token, cumulative text, logprobs |
| `on_post_generate` | After generation completes | Transform output, inject additional content | Full generation result, timing, token counts |

#### Context Management

| Hook | Fires When | Pre-Hook Can | Post-Hook Receives |
|---|---|---|---|
| `on_context_assemble` | Before final message list is sent to model | Modify message list — inject, filter, reorder messages | Assembled message list, context token count |
| `on_pre_compact` | Before compaction runs | Modify compaction input, provide custom compactor | Messages to compact, compaction config |
| `on_post_compact` | After compaction completes | Transform compacted result | Compacted messages, tokens saved, summary |
| `on_anchor_inject` | Context anchor re-injected after compaction/tier change | Modify anchor content, cancel injection | Anchor key, content |

#### Tool Execution

| Hook | Fires When | Pre-Hook Can | Post-Hook Receives |
|---|---|---|---|
| `on_pre_tool_call` | Before MCP tool execution | Modify params, cancel execution, substitute result (mock) | Caller ID, tool name, params, caller's key set |
| `on_post_tool_call` | After MCP tool execution | Transform result, inject additional directives | Tool name, params, result, directives, timing |
| `on_permission_check` | Before allow/deny decision on tool call | Override decision (grant or deny) | Caller ID, tool name, caller's key set, default decision |

#### Directive Processing

| Hook | Fires When | Pre-Hook Can | Post-Hook Receives |
|---|---|---|---|
| `on_directive` | Engine encounters a directive in tool result | Cancel directive, modify params | Directive type, params |
| `on_custom_directive` | Engine encounters unrecognized directive type | Handle it (engine ignores unknown directives by default) | Directive type, params |

#### Agentic Loop

| Hook | Fires When | Pre-Hook Can | Post-Hook Receives |
|---|---|---|---|
| `on_loop_start` | AgentEngine.run() begins | Modify initial state, cancel | Loop config, initial messages |
| `on_loop_iteration` | Each iteration of the agentic loop | Cancel loop (early termination), modify state | Iteration number, current state, messages |
| `on_loop_end` | AgentEngine.run() completes or errors | N/A (observation only) | Final state, metrics, termination reason |
| `on_state_change` | AgentState transitions | Cancel transition | Previous state, new state |

#### Delegation

| Hook | Fires When | Pre-Hook Can | Post-Hook Receives |
|---|---|---|---|
| `on_delegate` | Parent loop spawns child loop | Modify child config, cancel delegation | Parent ID, child ID, delegation params |
| `on_delegate_complete` | Child loop returns to parent | Transform child result before parent sees it | Child ID, result |

#### Error Handling

| Hook | Fires When | Pre-Hook Can | Post-Hook Receives |
|---|---|---|---|
| `on_error` | Any recoverable error in the pipeline | Provide recovery action, substitute result, suppress error | Error type, context, stage where error occurred |

### Registration API (C++)

```cpp
// Type-safe callback registration
engine.hooks().on_pre_generate([](PreGenerateContext& ctx) {
    // Modify context before generation
    ctx.messages.push_back(injected_message);
    return HookResult::Continue;  // or HookResult::Cancel
});

engine.hooks().on_post_tool_call([](PostToolCallContext& ctx) {
    // Log every tool call
    audit_log.append(ctx.caller_id, ctx.tool_name, ctx.params, ctx.result);
    return HookResult::Continue;
});

engine.hooks().on_custom_directive("my_directive", [](DirectiveContext& ctx) {
    // Handle consumer-defined directive types
    handle_custom_directive(ctx.params);
    return HookResult::Consumed;
});
```

---

## Licensing Change

### Current State

Entropic is Apache 2.0. This is fully permissive — anyone can take the engine, add features, ship closed-source products, and owe nothing but a license notice. Engine improvements are not required to be upstreamed.

### Required Change: LGPL + Commercial Dual License

**LGPL-3.0** for the open source distribution:
- Modifications to engine library code must be released under LGPL (upstreaming requirement)
- Applications linking against the engine can be proprietary/closed-source
- Consumers must allow relinking (dynamic linking satisfies this)
- Well-understood in industry — Qt, GTK, many game-adjacent libraries use LGPL

**Commercial license** for consumers who cannot or will not comply with LGPL:
- Negotiated per-seat or per-product
- Grants rights to statically link, modify without disclosure, etc.
- Standard dual-license model (MySQL/MariaDB, Qt, etc.)

**Named exception: BISSELL Homecare, Inc.** receives perpetual, royalty-free, permissive use rights without requiring a commercial license. This is the sole exception to the dual-license model.

### Transition

- Apache 2.0 → LGPL-3.0 transition happens as part of 2.0.0 release
- All prior Apache 2.0 releases remain Apache 2.0 (irrevocable)
- New contributions to 2.0.0+ are LGPL-3.0
- CLA (Contributor License Agreement) required for external contributors to maintain dual-licensing rights

---

## VRAM Context

A demanding first-party consumer targets 8-12 GB consumer GPUs (RTX 3060/4070 class). The engine's VRAM orchestration must be tight:

- Zero Python runtime overhead in C++ build
- Backend deduplication must work (shared GGUF → shared compute buffers)
- COLD/WARM/ACTIVE lifecycle must enforce mutual exclusivity between expensive tiers
- LoRA adapter swaps must not leak VRAM
- MoE models: VRAM accounting reflects total params loaded, not just active params per token

Example consumer VRAM budget (12 GB):
```
Active gameplay:  Router 0.5 GB + NPC 1.0 GB + KV cache 1.0 GB + app 1.0 GB = 3.5 GB used
Heavy tier swap:  BBEG/DM MoE 2-3 GB replaces NPC tier (mutually exclusive)
Headroom:         Engine must never exceed consumer's GPU allocation
```

---

## Summary Priority Matrix

| Feature | Priority | v2.0.0? | Category |
|---|---|---|---|
| C++ port: model orchestration + VRAM lifecycle | P0 | Yes | Rewrite |
| C++ port: agentic execution loop + state machine | P0 | Yes | Rewrite |
| C++ port: GBNF grammar constraint system | P0 | Yes | Rewrite |
| C++ port: tier-based routing | P0 | Yes | Rewrite |
| C++ port: MCP tool framework (servers, registry, execution, directives) | P0 | Yes | Rewrite |
| C++ port: chat adapters (per-model template handling) | P0 | Yes | Rewrite |
| C++ port: context management (anchors, compaction, messages) | P0 | Yes | Rewrite |
| C++ port: prompt system (frontmatter, identity, constitution) | P0 | Yes | Rewrite |
| C++ port: config loader + validation | P0 | Yes | Rewrite |
| C++ port: streaming generation callbacks | P0 | Yes | Rewrite |
| C++ port: delegation + tier handoff + directives | P0 | Yes | Rewrite |
| C++ port: conversation persistence | P0 | Yes | Rewrite |
| C++ port: logging infrastructure | P0 | Yes | Rewrite |
| C++ port: benchmark runner toolchain | P0 | Yes | Rewrite |
| C/C++ public API + headers | P0 | Yes | Rewrite |
| Systematic hook/callback architecture | P1 | Yes | New feature |
| Per-caller MCP key authorization | P1 | Yes | New feature |
| LoRA adapter hot-swapping | P1 | Yes | New feature |
| Per-generation grammar override + registry | P1 | Yes | New feature |
| Dynamic identity/tier creation at runtime | P1 | Yes | New feature |
| Dynamic grammar generation from string | P1 | Yes | New feature |
| License transition: Apache 2.0 → LGPL-3.0 + commercial dual license | P1 | Yes | Governance |
| MCP tool call structured audit log | P1 | Yes | New feature |
| Perplexity/logprob evaluation API | P2 | Yes | New feature |
| Compaction hooks (invokable, pre/post callbacks, custom compactor) | P2 | Yes | New feature |
| Constitutional training toolchain | P2 | Yes | New bundled tool |
| Python bindings (pybind11 or equivalent) | P2 | Yes | Rewrite |
| Dynamic MCP tool registration refresh | P2 | Yes | New feature |
| MCP server health / reconnection | P2 | Yes | New feature |
| Batched inference — multiple sequences per forward pass | P1 | Yes | New feature |
| Per-sequence state isolation (supersedes #12) | P1 | Yes | New feature |
| Multi-grammar generation pipeline | P1 | Yes | New feature |
| Inference backend abstraction (concrete base class) | P1 | Yes | New feature |
| Blackwell INT4/FP4 runtime detection + optimization | P2 | Yes | New feature |
| Vector store integration hooks | P2 | Yes | New feature |
| Hybrid SSM state management | ~~P3~~ | Absorbed into per-sequence state isolation (#13) | Superseded |
| Voice integration hooks | P3 | No | Future — entry points only |
