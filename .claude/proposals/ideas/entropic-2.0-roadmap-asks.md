# Entropic 2.0.0 Roadmap Asks

Input document for the 2.0.0 roadmap. Derived from two first-party consumer design sessions (game application, IoT test agent) but framed as general-purpose engine capabilities. Application-specific logic is excluded — these are engine features only.

Full context: `entropic-2.0-game-requirements.md`, `P2-20260310-001.md`, `P2-20260311-001.md` in this directory.

---

## P0 — C++ Rewrite

Entropic 2.0.0 is a C++ rewrite. The Python codebase (30K+ LOC, 800+ tests) validates the architecture. Every subsystem below must have a C++ equivalent.

**Note:** The TUI is NOT part of the engine library in 2.0.0. It becomes a separate consumer repo. The engine is a C/C++ library with public headers. Consumers link against it.

### Core Engine
- Agentic execution loop + state machine (AgentEngine, AgentState)
- Streaming generation callbacks (on_stream_chunk)
- Context management (message list, context anchors, keyed persistent messages)
- Context compaction (configurable, invokable)
- Delegation (child loops, depth tracking, parent/child conversation IDs)
- Directives system (tier_change, stop_processing, context_anchor from tool results)
- Interruption (thread-safe cancel/pause)
- Tier locking (prevent mid-task model switching)
- Loop config (max_iterations, max_consecutive_errors, max_tool_calls_per_turn, idle_timeout, auto_approve)
- Metrics collection (per-loop timing, token counts)

### Inference
- Model orchestration: three-state lifecycle (COLD/WARM/ACTIVE), keep_warm, single lock guarding GPU swaps
- Backend deduplication: shared GGUF → shared backend instance, avoids duplicate compute buffers
- Tier-based routing: router model classification, tier_map, tier history, log-prob confidence, fallback
- Chat adapters: per-model template handling, tool call parsing per adapter
- GBNF grammar constraints: per-tier grammar files, grammar-constrained generation
- Generation config: max_tokens, temperature, top_p/k, repeat_penalty, per-generation overrides
- MoE model support via llama.cpp (assume llama.cpp handles correctly; VRAM accounting must reflect total params)

### MCP
- Server manager (singleton, manages built-in + external servers)
- Built-in servers (filesystem, bash, git, diagnostics, web, entropic)
- External servers (stdio + socket transport, YAML config, .mcp.json discovery, runtime registration)
- Tool registry (merged across all servers, JSON tool definitions)
- Tool executor (permission validation, execution, error handling, result injection)
- Permissions (allow/deny/auto_approve with glob patterns — extended by per-caller keys in P1)
- Directives in tool results

### Prompts
- Prompt manager, prompt file loading
- YAML frontmatter parsing (PromptFrontmatter, IdentityFrontmatter, ConstitutionFrontmatter, AppContextFrontmatter)
- Tier identity loading (per-tier system prompts, personality, focus, examples)
- Constitution (shared behavioral guardrails)

### Configuration
- Config loader + schema validation (YAML, Pydantic-equivalent in C++)
- Tier config, models config, routing config, MCP config, generation config, compaction config
- Permission persistence (save_permission)

### Storage & Logging
- Conversation persistence (SQLite or equivalent)
- Structured logging (model logger, display logger)

### Bundled Toolchains
- Benchmark runner (`entropic benchmark run`)
- LSP client (diagnostics integration)

### Public API
- C/C++ public API + headers exposing all above
- Python bindings (pybind11 or equivalent) preserving existing Python consumer compatibility

---

## P1 — New Engine Features

### Per-Caller MCP Authorization with Runtime Key Grants

Per-caller (per-identity) MCP access control with runtime-grantable keys. Each identity has a discrete set of authorized MCP keys, not just tier-level globs. Keys support READ/WRITE access levels. Grantable/revocable at runtime via MCP tool calls (one caller grants keys to another). Key sets are serializable. Engine enforces boundaries — a caller cannot invoke tools outside its key set.

**General-purpose value:** Multi-agent deployments with scoped tool access, multi-user systems, robotics subsystem permissions.

### LoRA Adapter Hot-Swapping

Load a base model once, swap LoRA adapters without unloading the base. Adapter lifecycle: load, unload, swap. Multiple adapters warm in RAM, one active on GPU (mirrors COLD/WARM/ACTIVE). Adapter metadata accessible for routing. Tier config supports adapter_path. Target swap latency: ~50-200ms.

**General-purpose value:** Any deployment with multiple roles/personas on a shared base model.

### Per-Generation Grammar Override + Registry

Same model, different grammar per generation call. Generation call accepts optional grammar parameter overriding tier default. Runtime grammar loading without restart. Grammar validation API. Grammar registry: named grammars loadable by key, registered at runtime.

### Dynamic Identity/Tier Creation at Runtime

Create, configure, and destroy identities at runtime — not just from static config. API: `create_identity(name, system_prompt, grammar_id, mcp_keys, adapter_path?)`, `update_identity(...)`, `destroy_identity(...)`. Created identities participate in tier routing. Bounded by configurable max_identities.

**General-purpose value:** Any multi-agent deployment where agents are spawned/retired dynamically.

### Dynamic Grammar Generation from String

Register grammars from in-memory GBNF strings, not just file paths. One model's output can define the grammar constraints for another model's generation. Grammar validation on registration. Registered grammars usable in per-generation override and identity config.

### MCP Tool Call Structured Audit Log

Complete, structured, replayable log of all MCP tool calls. JSON lines format: timestamp, caller_id, tool_name, params, result, directives. Appendable per session. Replay API: `replay_log(path)` re-executes tool call sequence. Filtering by caller_id, tool_name, time range.

**General-purpose value:** Debugging, state reconstruction, reproducibility, evidence trails, undo/replay workflows.

### Systematic Hook/Callback Architecture

Every stage transition in the engine pipeline is a hook point. Pre-hooks can modify or cancel. Post-hooks can inspect and transform. Multiple callbacks per hook, chained by registration order. Failing hooks are logged and skipped — never crash the engine.

**Hook points:**

| Stage | Hooks |
|---|---|
| Model lifecycle | on_model_load, on_model_unload, on_adapter_swap, on_vram_pressure |
| Tier routing | on_route |
| Generation | on_pre_generate, on_stream_token, on_post_generate |
| Context | on_context_assemble, on_pre_compact, on_post_compact, on_anchor_inject |
| Tool execution | on_pre_tool_call, on_post_tool_call, on_permission_check |
| Directives | on_directive, on_custom_directive |
| Agentic loop | on_loop_start, on_loop_iteration, on_loop_end, on_state_change |
| Delegation | on_delegate, on_delegate_complete |
| Error | on_error |

### License Transition: LGPL-3.0 + Commercial Dual License

- Apache 2.0 → LGPL-3.0 for open source distribution
- Modifications to engine code must be released (upstreaming)
- Applications linking against the engine can be proprietary
- Commercial license for consumers who cannot comply with LGPL
- Named exception: BISSELL Homecare, Inc. — perpetual permissive use, no commercial license required
- CLA required for external contributors
- Prior Apache 2.0 releases remain Apache 2.0 (irrevocable)

---

## P2 — New Engine Features

### Compaction Hooks

Compaction invokable programmatically on a specific identity's context. Pre/post callbacks. Consumer can register custom compactor replacing or wrapping default. Default summarization remains fallback.

### Perplexity / Log-Probability Evaluation API

Evaluate arbitrary token sequences against a loaded model, return per-token log-probabilities without generating. `compute_perplexity(model_id, tokens)`, `get_logprobs(model_id, tokens)`. Evaluation-only, no KV cache mutation. Works on any ACTIVE model.

### Constitutional Training Toolchain (Bundled Tool)

Ships with Entropic like the benchmark runner. Not the core inference path — a companion tool.

- **Data generation:** Use a loaded teacher model to generate synthetic training data with grammar constraints and constitutional filtering
- **Constitutional filtering:** Configurable rejection rules
- **LoRA fine-tuning:** Orchestrate adapter training on generated data
- **Validation:** Evaluate adapter quality against test set
- **Interface:** CLI (`entropic train ...`) and programmatic API

### Dynamic MCP Tool Registration Refresh

Support servers declaring new tools mid-session. `ToolRegistry.refresh(server_id)` re-queries a server's tool list. Server-push notification support (MCP protocol compatible). Models see updated tool lists on next generation. Removed tools cleaned up after pending calls complete.

### MCP Server Health / Reconnection

Detect external MCP server disconnection. Configurable reconnection with backoff per server. Surface events through hooks (on_server_disconnect, on_server_reconnect). Tool calls to disconnected servers return structured error, holdable via hook for retry.

### Python Bindings

pybind11 or equivalent wrapping C++ API. Preserves compatibility for existing Python consumers (TUI repo, Pathfinder Society Scribe, future Python SDK users).

---

## P3 — Future (Not v2.0.0 Blockers)

### Hybrid SSM/Transformer State Management

Extract/restore recurrent hidden state for hybrid Mamba-Transformer models. Serialize to disk. Enables persistent memory across sessions. Blocked on upstream llama.cpp SSM support.

### Voice Integration Hooks

Callback entry points for pre/post inference audio processing (STT/TTS/S2S). Engine provides hooks only — consumers provide models and audio I/O. No specific voice model baked into engine.

### Multi-Agent Concurrent Instances

Multiple independent agent loops in one Entropic instance, each with own identity set and MCP scope. Future scope for both game (multiplayer) and tester (multi-device).
