# Entropic Roadmap: v1.7.1 → v2.0.0

Master roadmap for the Entropic engine. Each version has a corresponding
proposal in `.claude/proposals/ACTIVE/` with detailed implementation plans.

This document is the source of truth for version targeting and feature scope.
Individual proposals contain implementation details, examples, and validation
criteria.

---

## Current State (v1.7.1)

- Python engine: 878 unit tests, 42 model tests, 13/13 identity benchmarks
- 11 bundled identities (7 front office + 3 back office + 1 utility)
- Single model architecture: Qwen3.5-35B-A3B-UD-IQ3_XXS on 16GB Blackwell
- Lead delegates via pipeline/delegate (3 tools only)
- Minimal child tool: `entropic.todo` replaces `todo_write` god tool
- Bundled model registry (`bundled_models.yaml`) with download CLI
- Identity-aware benchmarks with tool injection

---

## v1.7.2 — TUI Separation

Extract the TUI into a standalone consumer repo (`entropic-tui`).

- TUI becomes an external dependency on `entropic-engine`
- Engine repo has no TUI imports or TUI-specific code paths
- Validates the engine's public API surface (identifies gaps)
- CLI commands (benchmark, download, init, setup-cuda) stay in engine
- TUI locked to v1.7.1 engine initially, tracks releases through 1.8.x

## v1.7.3 — Engine API Cleanup

Post-TUI-separation cleanup.

- Remove TUI-specific code from `app.py`
- Headless engine entry point only
- Stabilize `LibraryConfig` for external consumers
- Document public vs internal API boundaries
- Deprecation warnings on any internal APIs used externally

---

## v1.8.x — C++ Engine Port

Faithful translation of the Python engine to C++. No new features — port
what exists, validate parity. Each subsystem is independently verifiable.

### v1.8.0 — C++ Project Scaffold

- CMake project structure (`src/`, `include/`, `tests/`)
- llama.cpp as git submodule (direct linkage, no wrapper)
- Public C API header design (`entropic.h`) — interface sketch
- Build system: CUDA detection, platform support
- CI pipeline: GitHub Actions for CPU builds, lint, static analysis
- Doxygen setup with heavy commenting standard
- Concrete base class patterns established (C++ equivalent of Python ABCs)
- Expose llama.cpp config pass-through fields in schema:
  `cache_type_k`, `cache_type_v`, `n_batch`, `n_threads`,
  `tensor_split`, `flash_attn`, `reasoning_budget`

### v1.8.1 — Config + Prompts

- YAML config loader (yaml-cpp or equivalent)
- Config schema validation (mirrors current Pydantic schema)
- Prompt manager (YAML frontmatter parsing, identity loading)
- Constitution and app_context loading
- BundledModels registry (path resolution from keys)
- Unit tests for config loading + prompt parsing

### v1.8.2 — Inference Backend

- `InferenceBackend` concrete base class
- `LlamaCppBackend` — model load, generate, stream (direct C API)
- VRAM lifecycle state machine (COLD/WARM/ACTIVE)
- Chat adapter base class + Qwen35 adapter
- `reasoning_budget` exposed as PhaseConfig inference parameter
- KV cache quantization (`cache_type_k`, `cache_type_v`) as config fields
- Generation config: temperature, top_p/k, repeat_penalty, max_tokens
- Design interfaces to be architecture-agnostic (transformer, GDN/recurrent)
- Benchmark: tok/s parity with Python engine

### v1.8.3 — Host-Memory Prompt Caching

- Expose llama.cpp `-cram` (host-memory prompt cache)
- Cache identity/constitution/tool prefixes in RAM
- Hot-swap cached prefixes on role/identity change
- Validates task #51 (KV cache prefix injection) via native mechanism
- Configurable RAM limit for prompt cache

### v1.8.4 — Engine Loop

- `AgentEngine` (agentic loop, state machine)
- `ResponseGenerator` (system prompt assembly, tier-filtered tools)
- `ContextManager` (message list, anchors, compaction trigger)
- `DirectiveProcessor` (typed directives from tool results)
- Interrupt/pause (thread-safe)
- Loop config (max_iterations, max_consecutive_errors, max_tool_calls_per_turn)
- Metrics collection (per-loop timing, token counts)
- Headless test: prompt in → response out

### v1.8.5 — Tool System

- `MCPServerBase` concrete base class
- `ToolRegistry` + `ToolExecutor`
- `PermissionManager` (allow/deny, glob patterns)
- Built-in servers: filesystem, bash, git, diagnostics, web, entropic
- LSP client embedded in diagnostics (diagnostic diffs in write/edit responses)
- Duplicate detection + error feedback (circuit breaker)
- Investigate llama.cpp autoparser for tool call parsing
  - If viable: simplify adapter parsing, reduce GBNF dependency for tool calls
  - If not: port existing adapter XML/JSON parsing
- Tool-calling loop test: generate → parse → execute → generate

### v1.8.6 — Delegation + Identity

- `DelegationManager` (child loops, worktree isolation)
- Pipeline execution (multi-stage sequential, shared worktree)
- Identity system (allowed_tools filtering, phases, PhaseConfig)
- `TodoTool` (child), delegate/pipeline/complete tools (lead)
- Auto-chain (depth 0 → lead, depth > 0 → complete)
- Pipeline scope enforcement ("stay within your role")
- Delegation test: lead → eng → qa pipeline end to end

### v1.8.7 — External MCP

- External MCP server support (stdio/SSE transport)
- `.mcp.json` discovery (project + global)
- Runtime MCP registration/deregistration
- Server health detection + configurable reconnection with backoff
- Dynamic tool registration refresh (server declares new tools mid-session)
- Tool call routing to disconnected servers returns structured error

### v1.8.8 — Storage + Logging

- SQLite conversation persistence
- Structured logger (session.log, model.log equivalents)
- Delegation storage (parent/child conversation IDs)
- Permission persistence (save_permission)

### v1.8.9 — C API Stabilization

- `entropic.h` finalized
- Engine lifecycle: create, configure, run, interrupt, destroy
- Tool registration: register_tool, register_server
- Identity management: load_identity, get_identity
- Hook registration: register_hook
- Error handling contract (error codes + callbacks)
- Thread safety guarantees documented
- API versioning scheme (semver on C API)
- Compile + link from external project verified

---

## v1.9.0 — Python Engine Deprecated

- Auto-generated Python wrapper (ctypes/cffi from `entropic.h`)
  - Generation script in CMake build, always in sync with C API
  - No manual wrapper maintenance
- CLI commands ported to use C engine via wrapper
- Benchmark runner ported to use C engine
- Python engine code marked deprecated
- Model tests passing against C engine

### v1.9.1 — Hook Architecture

- `HookRegistry` (pre/post at every stage transition)
- Model lifecycle: on_model_load, on_model_unload, on_adapter_swap, on_vram_pressure
- Generation: on_pre_generate, on_stream_token, on_post_generate
- Context: on_context_assemble, on_pre_compact, on_post_compact
- Tool: on_pre_tool_call, on_post_tool_call, on_permission_check
- Directives: on_directive, on_custom_directive
- Agentic loop: on_loop_start, on_loop_iteration, on_loop_end, on_state_change
- Delegation: on_delegate, on_delegate_complete
- Error: on_error
- Pre-hooks can modify or cancel. Post-hooks inspect and transform.
- Failing hooks logged and skipped — never crash the engine.

### v1.9.2 — LoRA Adapter Hot-Swapping

- Load base model once, swap LoRA adapters without unloading
- Adapter lifecycle: load, unload, swap
- Multiple adapters warm in RAM, one active on GPU (mirrors COLD/WARM/ACTIVE)
- Adapter metadata accessible for routing
- Tier config supports `adapter_path`
- Target swap latency: ~50-200ms

### v1.9.3 — Grammar Registry + Per-Generation Override

- `GrammarRegistry` — named grammars loadable by key
- Runtime grammar loading without restart
- Per-generation grammar override (same model, different grammar per call)
- Dynamic grammar from in-memory GBNF string (not just file paths)
- Grammar validation API
- One model's output can define grammar constraints for another's generation

### v1.9.4 — Per-Caller MCP Authorization

- Per-identity MCP access control with runtime-grantable keys
- Each identity has a discrete set of authorized MCP keys
- READ/WRITE access levels per tool
- Grantable/revocable at runtime via API (one caller grants keys to another)
- Key sets serializable
- Engine enforces boundaries — caller cannot invoke tools outside its key set

### v1.9.5 — Structured Audit Log

- Complete JSONL log of all MCP tool calls
- Fields: timestamp, caller_id, tool_name, params, result, directives
- Appendable per session
- Replay API: `replay_log(path)` re-executes tool call sequence
- Filtering by caller_id, tool_name, time range

### v1.9.6 — Dynamic Identity/Tier Creation

- Create, configure, destroy identities at runtime (not just static config)
- API: `create_identity(name, system_prompt, grammar_id, mcp_keys, adapter_path?)`
- `update_identity(...)`, `destroy_identity(...)`
- Created identities participate in tier routing
- Bounded by configurable `max_identities`

### v1.9.7 — Time-Based Generation Cap + GPU Resource Profiles

- Per-tier wall-clock time cap alongside token limits
- Throughput tracking, auto-adapting generation length across hardware
- GPU resource profiles: `n_batch`, `n_threads` knobs exposed
- Profiles: maximum, balanced, background, minimal

### v1.9.8 — Constitutional Validation Pipeline

- Post-generation grammar-constrained critique pass
- Structured JSON output evaluating constitutional compliance
- Revision loop: if violations found, re-generate with feedback
- Configurable: which identities get validation, which rules apply
- Consumer can disable for latency-sensitive use cases

### v1.9.9 — Compaction Hooks

- Compaction invokable programmatically per identity context
- Pre/post compaction callbacks
- Consumer can register custom compactor replacing or wrapping default
- Default summarization remains fallback

### v1.9.10 — Perplexity / Log-Probability Evaluation API

- Evaluate arbitrary token sequences against loaded model
- `compute_perplexity(model_id, tokens)`, `get_logprobs(model_id, tokens)`
- Return per-token log-probabilities without generating
- Evaluation-only, no KV cache mutation
- Works on any ACTIVE model

### v1.9.11 — Vision / Multimodal Support

- Multimodal message support via content arrays (text + image)
- mmproj loading for vision-capable models (Qwen3.5 mmproj files)
- Backward-compatible text-only fallback
- Image preprocessing pipeline
- Adapter support for vision content formatting

### v1.9.12 — Diagnose + Self-Inspection

- Engine introspection tool: model can read its own configuration,
  loaded identities, tool definitions, and recent action history
- Doxygen-generated documentation accessible for architecture understanding
- `/diagnose` command: model self-reflects on previous actions,
  identifies reasoning errors, flags potential engine-related root causes
- Exposed as an entropic internal tool

### v1.9.13 — Backend Abstraction

- `InferenceBackend` interface finalized for future backends
- `LlamaCppBackend` as reference implementation
- Interface designed for:
  - GDN/recurrent architectures (Falcon-H1 family)
    - GDN CUDA/Vulkan/Metal kernels merged in llama.cpp
    - https://github.com/ggml-org/llama.cpp/pull/20340
  - NPU backends (Axera AX630C via Pulsar2/AXCL)
  - Multi-sequence/batched inference (foundation for 3.0.0 fan-out)
- Architecture-agnostic capability queries

### v1.9.14 — Hardening

- Stress testing (long sessions, context pressure, rapid interrupts)
- Memory leak analysis (valgrind, AddressSanitizer)
- Edge cases: malformed tool calls, model crashes, OOM recovery
- Concurrent access patterns verified
- Performance regression tests vs Python engine

### v1.9.15 — Migration Cleanup

- Remove deprecated Python engine code
- `entropic-engine` PyPI package ships C library + auto-generated Python wrapper
- CLI fully functional from C engine
- `examples/` ported to C engine API (hello-world, pychess)
- README updated for 2.0.0 architecture

---

## v1.10.0 — Test Architecture Revamp

- Test framework selection (GoogleTest, Catch2, or Ceedling — BDD-style preferred)
- Conventional test mapping: test files mirror source structure
- Doxygen-driven enhancement: parse annotations to map tests to API functions
- Git diff integration: select relevant tests based on changed files
- CI pipeline: GitHub Actions for unit/API tests (CPU)
- Model tests remain developer-run with checked-in reports
- Manual integration tests (test1-test6 style) remain manual

## v1.10.1 — Test Maturity

- Coverage targets established
- Regression suite from manual test findings (test1-test6 patterns)
- CI gates: no merge without passing tests
- Doxygen-driven test discovery operational

---

## v2.0.0 — Release

- License: LGPL-3.0 + commercial dual license
  - Named exception: BISSELL Homecare, Inc. — perpetual permissive use
  - Prior Apache 2.0 releases remain Apache 2.0 (irrevocable)
  - CLA required for external contributors
- Stable C API (`entropic.h`, versioned)
- Three distribution channels:
  1. PyPI wheel with bundled `.so` (CUDA, purpose-built)
  2. apt/deb package for C++ consumers (game engine, embedded)
  3. Source build from git clone (static linking)
- Auto-generated Python wrapper
- Doxygen API reference published
- Benchmark results published (perf + quality)
- `examples/` fully ported and validated against C engine
- GitHub release + PyPI publish

**Post-2.0.0 (not blocking release):**
- `entropic-tui` ported to C engine Python wrapper (separate repo, own timeline)

---

## v2.1.0 — Fine-Tuning Pipeline

- Bundled toolchain: `entropic train` CLI
- QLoRA + DPO from session logs
- Synthetic data generation with grammar constraints + constitutional filtering
- LoRA adapter training orchestration
- Adapter quality validation against test set
- Programmatic API alongside CLI

---

## Deferred to 3.0.0 Roadmap

Planned after 2.0.0 ships. Each item includes context and references
for future planning sessions.

### SSM / Mamba Hybrid State Management

Extract/restore recurrent hidden state for hybrid Mamba-Transformer models.
Serialize to disk for persistent memory across sessions.

- GDN (Gated Delta Networks / Falcon-H1) kernel support already merged in llama.cpp
  - CUDA, Vulkan, Metal kernels: https://github.com/ggml-org/llama.cpp/pull/20340
  - Speculative decoding incompatible with recurrent models: https://github.com/ggml-org/llama.cpp/pull/19270
- Prompt caching for recurrent models fixed upstream: https://github.com/ggml-org/llama.cpp/pull/19045
- Backend abstraction (1.9.13) designed to accommodate this
- Context: memory notes on SSM vectoring from 2026-03-17 session

### Fan-Out Routing

Router classifies input → routes to MULTIPLE identities executing in parallel.
Requires parallel sequences/batched inference in the backend.

- llama.cpp batch API exists but Entropic uses single-sequence through 2.0.0
- Fan-out generates in parallel, results collected by coordinator
- Hooks: `on_fan_out`, `on_fan_in`
- Context: `.claude/proposals/ideas/entropic-3.0-robotics-roadmap-asks.md` (Section 1)

### Coordination / Safety Interlock Layer

After fan-out, validate combined outputs don't conflict before actuation.

- CoordinatorConfig: model-based, rule-based, or hook-based validation
- Output: `{approved, modifications, reason}`
- Context: `.claude/proposals/ideas/entropic-3.0-robotics-roadmap-asks.md` (Section 2)

### MoE Expert-Level VRAM Caching (P3-023)

Expert LRU cache in GPU VRAM with activation-aware placement.
Optimize 256-expert MoE models.

- Context: `.claude/proposals/ideas/P3-20260226-023-moe-expert-vram-caching.md`

### Multi-Agent Concurrent Instances

Multiple independent agent loops in one Entropic instance.
Each with own identity set and MCP scope.

### Voice Integration

Hook points exist from 1.9.1. Full STT/TTS/S2S implementation deferred.
Engine provides hooks only — consumers provide models and audio I/O.

### Physical Actuator Safety Constraints

Tool-level safety constraints enforcing physical limits regardless of model output.
`ToolSafetyConfig`: min/max ranges, rate-of-change limits, forbidden combinations.
Emergency stop: `engine.emergency_halt()`.

- Context: `.claude/proposals/ideas/entropic-3.0-robotics-roadmap-asks.md` (Section 4)

### NPU Backend (AxeraBackend)

AX630C, Pulsar2 toolchain, .axmodel format (NOT GGUF, NOT llama.cpp).
Subclass of `InferenceBackend` interface designed in 1.9.13.

- Context: `.claude/proposals/ideas/entropic-3.0-robotics-roadmap-asks.md`

### Speculative Decoding

Draft model proposes tokens, large model validates. Can double throughput.
Blocked by single-GPU VRAM constraint — needs multi-GPU or smaller primary model.

- llama.cpp supports it, auto-disables for recurrent models
- https://github.com/ggml-org/llama.cpp/pull/19270

### Fused MoE Gate+Expert Weights

7-9% prompt processing speedup. Model conversion flag (`--fuse_gate_up_exps`).
Investigate whether unsloth quants already include this.

- https://github.com/ggml-org/llama.cpp/pull/19139

### Model Candidate Investigation

Benchmarking roadmap for SSM/hybrid and specialized models when
evaluation framework is mature.

- Context: `.claude/proposals/ideas/P2-20260302-030-model-candidate-investigation.md`

---

## Deprecation Plan

All existing proposals in `ACTIVE/` and `ideas/` are absorbed into this
roadmap. Before work begins, they move to `.claude/proposals/.old/` for
reference. They are NOT deleted — just archived.

### Files to move to `.old/` (when work begins)

**From ACTIVE/:**
- `P1-20260226-022-vram-orchestration.md` → absorbed into v1.8.2
- `P1-20260302-028-tui-overhaul.md` → absorbed into v1.7.2 + post-2.0.0
- `P2-20260225-020-gpu-resource-management.md` → absorbed into v1.9.7
- `P2-20260225-021-time-based-generation-cap.md` → absorbed into v1.9.7
- `P2-20260302-026-runtime-mcp-registration.md` → absorbed into v1.8.7

**From ideas/:**
- `entropic-2.0-roadmap-asks.md` → absorbed into this roadmap (all items)
- `entropic-2.0-game-requirements.md` → context doc, absorbed into 3.0.0 deferred
- `entropic-3.0-robotics-roadmap-asks.md` → context doc, absorbed into 3.0.0 deferred
- `P1-20260213-012-per-tier-model-finetuning.md` → absorbed into v2.1.0
- `P1-20260302-031-qwen35-vision-support.md` → absorbed into v1.9.11
- `P1-20260309-033-native-inference-layer.md` → absorbed into v1.8.0–v1.8.9
- `P2-20260204-005-tool-abandonment-detection.md` → dropped (1.7.0 sufficient)
- `P2-20260209-010-diagnostic-lsp-integration.md` → absorbed into v1.8.5
- `P2-20260302-030-model-candidate-investigation.md` → dropped (ongoing, not versioned)
- `P2-20260309-032-constitutional-validation.md` → absorbed into v1.9.8
- `P2-20260309-034-tui-repo-separation.md` → absorbed into v1.7.2
- `P2-20260310-001.md` → context doc (game consumer), absorbed into 3.0.0 deferred
- `P2-20260311-001.md` → context doc (IoT consumer), absorbed into 3.0.0 deferred
- `P3-20260226-023-moe-expert-vram-caching.md` → absorbed into 3.0.0 deferred

New individual proposals will be created in `ACTIVE/` for each versioned
milestone as work begins, with full implementation plans and validation criteria.
