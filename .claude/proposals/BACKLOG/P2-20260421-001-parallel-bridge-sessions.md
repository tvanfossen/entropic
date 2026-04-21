---
id: P2-20260421-001
title: "Parallel Bridge Sessions with VRAM-Aware Admission Control"
priority: 2
status: BACKLOG
created: 2026-04-21
updated: 2026-04-21
version_target: TBD
author: "@architect"
---

## Summary

Enable the external MCP bridge to run multiple independent agentic sessions
concurrently, sharing loaded model weights but with isolated KV caches and
conversation state. VRAM budget enforcement prevents over-subscription.

## Motivation

A 16GB VRAM GPU can fit 4–6 concurrent 4B Q8_0 contexts (~4GB shared weights +
~500MB KV cache each). An external client like Claude Code could spin off
parallel `entropic.ask` calls to different sessions — e.g., one researching
docs, another analyzing code structure, a third writing tests — all running
simultaneously on the local GPU. The engine manages VRAM admission so the
client doesn't need to know hardware limits.

## Current State

- `entropic_run` / `entropic_run_streaming` hold `api_mutex` — serialized
  per-handle. Multiple concurrent calls block on each other.
- One conversation per handle. The bridge shares a single message history.
- `ModelOrchestrator` tracks loaded models but doesn't enforce a VRAM budget.
- `vram_reserve_mb` exists in config but is advisory only.
- llama.cpp supports multiple `llama_context` per `llama_model` — weights are
  shared read-only, KV caches are per-context.

## Design

### Session Multiplexer

The bridge gains a session concept:

```
entropic.ask { "prompt": "...", "session": "research-1" }
entropic.ask { "prompt": "...", "session": "code-analysis" }
entropic.ask { "prompt": "...", "session": "test-writer" }
```

- Omitted `session` → default shared session (backward compatible)
- New session IDs auto-create on first use
- Each session owns: conversation history, KV cache context, active tier state
- Sessions share: loaded model weights, grammar registry, tool servers

### VRAM Budget Enforcement

```
total_vram          = query from GPU at init
model_vram          = sum of loaded model sizes (from GGUF metadata)
kv_per_session      = context_length × n_layers × d_model × 2 × kv_type_size
reserve             = config.vram_reserve_mb
available           = total_vram - model_vram - reserve
max_sessions        = floor(available / kv_per_session)
```

On `entropic.ask` with a new session ID:
- If `active_sessions < max_sessions` → allocate context, proceed
- Else → return error: "VRAM budget exhausted (N/M sessions active)"

### Locking Model

Replace per-handle `api_mutex` with per-session locks:
- Session creation/destruction: handle-level lock (brief)
- Session execution: session-level lock (long, allows parallel runs)
- Model loading/unloading: handle-level lock (rare)

### Bridge Protocol Changes

New tools:
- `entropic.session_create { "session": "name" }` — pre-allocate
- `entropic.session_destroy { "session": "name" }` — release KV cache
- `entropic.session_list` — active sessions + VRAM usage
- `entropic.ask` gains optional `"session"` field

### KV Cache Lifecycle

- Created lazily on first `entropic.ask` to a session
- Destroyed on `session_destroy` or bridge shutdown
- Idle timeout (configurable) — sessions unused for N minutes release KV cache
- KV cache can be saved/restored via `llama_state_seq_get/set_data` for
  session hibernation (v1.8.3 decision)

## Constraints

- Each session runs one turn at a time (per-session serialization)
- Sessions within a handle share the same config, tools, and identities
- Maximum sessions bounded by VRAM, not arbitrary limit
- Session creation is fast (KV alloc only, no model load)

## Dependencies

- llama.cpp multi-context support (already available)
- VRAM query API (ggml provides this)
- KV cache size calculation (available from model metadata)

## Open Questions

- Should sessions have independent identity/tier assignments, or inherit
  from the handle config? Independent would allow "researcher session" vs
  "writer session" with different tool access.
- Should the bridge auto-scale sessions or require explicit creation?
- Idle timeout value — configurable per-session or global?

## Verification

1. Start engine with 4B model on 16GB GPU
2. Create 4 sessions via bridge
3. Send concurrent `entropic.ask` to all 4 — verify parallel execution
4. Attempt 5th session — verify VRAM rejection with clear message
5. Destroy a session — verify 5th session now succeeds
6. Verify session isolation — conversation in session A invisible to B
