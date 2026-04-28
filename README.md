# Entropic

> Local-first agentic inference engine — your models, your hardware, your control

## What Is Entropic?

Entropic is a **C inference engine** that turns a local GGUF model into a
multi-tier, tool-calling AI system. It runs entirely on your hardware — no
cloud, no API keys, no telemetry. You control the model, the prompts, the
tools, and the data.

The name comes from information theory: every handoff between human intent,
prompt, and model is a lossy translation. Information decays at each boundary.
Entropic is purpose-built to manage that decay — structured context management,
identity-based delegation, grammar-constrained output, and tool-augmented
reasoning minimize what gets lost along the way.

## Why Entropic?

**You want local AI that actually does things**, not just answers questions.

Most local inference tools give you a model and a chat loop. Entropic gives you
an **engine** — the infrastructure between your application and the model that
handles the hard parts:

| Problem | How Entropic Solves It |
|---------|----------------------|
| Model just generates text | Agentic loop: generate → parse tool calls → execute → re-generate |
| One model, one personality | Identity system: same model serves multiple roles with different prompts, tools, and constraints |
| Context gets stale | Auto-compaction: summarizes old context to stay within window |
| Output format unpredictable | Grammar constraints: GBNF grammars force structured output |
| Need tools but no cloud | MCP tool servers: filesystem, bash, git, web — all local, plugin architecture |
| Privacy concerns | Zero network calls. Everything stays on your machine. |

## Who Is It For?

Entropic is an engine, not an application. It's for developers building
AI-powered software that needs to run locally:

- **CLI/TUI tools** that use AI for code generation, analysis, or planning
- **Game engines** that need NPC dialogue or decision-making from a local model
- **Embedded systems** with on-device inference (CPU-only static build available)
- **Education platforms** running student-facing AI without cloud dependencies
- **Privacy-sensitive applications** where data cannot leave the device

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│  Your Application                                       │
│  C/C++ (direct linkage) · Python (ctypes wrapper)       │
├─────────────────────────────────────────────────────────┤
│  librentropic.so — C API                                │
│                                                         │
│  ┌─────────┐ ┌────────┐ ┌──────┐ ┌───────┐ ┌────────┐ │
│  │ Engine  │ │Inference│ │ MCP  │ │Config │ │Storage │ │
│  │ Loop    │ │Backend │ │Servers│ │Loader │ │Backend │ │
│  │         │ │        │ │      │ │       │ │        │ │
│  │ Context │ │ Prompt │ │ Tool │ │ YAML  │ │ SQLite │ │
│  │ Routing │ │ Cache  │ │ Auth │ │ Layer │ │ Audit  │ │
│  │ Delegate│ │ Grammar│ │Plugin│ │ Merge │ │ Session│ │
│  └─────────┘ └────────┘ └──────┘ └───────┘ └────────┘ │
├─────────────────────────────────────────────────────────┤
│  llama.cpp (CUDA / Vulkan / CPU)                        │
└─────────────────────────────────────────────────────────┘
```

Pure C at all `.so` boundaries. No C++ ABI crossing. Any language that can call
C functions can use the engine.

## Quick Start

For consumer install paths (tarball or `pip install entropic-engine`),
read **[docs/getting-started.md](docs/getting-started.md)** — it covers C/C++ direct
linking and the Python wrapper end-to-end.

For contributors building from source:

### Prerequisites

- Linux (tested on Ubuntu 24.04)
- cmake 3.21+, C++20 compiler
- NVIDIA GPU with 16GB+ VRAM (or CPU-only for smaller models)
- Python 3.10+ (for invoke task runner and pre-commit hooks)

### Build

```bash
git clone --recurse-submodules https://github.com/tvanfossen/entropic.git
cd entropic

python3 -m venv .venv
.venv/bin/pip install -e ".[dev]"     # invoke + pre-commit + gcovr + ruff + mypy
.venv/bin/pre-commit install

# CUDA build (default)
inv build --clean

# CPU-only build
inv build --cpu
```

### Run an Example

```bash
inv example -n pychess     # Multi-tier chess (C++)
inv example -n explorer    # Interactive REPL (C++)
inv example -n headless    # Minimal C harness
```

## Usage

### C API

```c
#include <entropic/entropic.h>

entropic_handle_t h = NULL;
entropic_create(&h);
entropic_configure_dir(h, ".myapp");  // Layered config resolution

// Streaming generation with full engine pipeline
entropic_run_streaming(h, "What is 2+2?", on_token, NULL, NULL);

// Conversation persists across calls
entropic_run_streaming(h, "Explain your reasoning", on_token, NULL, NULL);

// Manage conversation
size_t count;
entropic_context_count(h, &count);   // Message count
entropic_context_clear(h);           // New session

entropic_destroy(h);
```

### Python Wrapper (ctypes)

The Python package is a thin ctypes binding over the C ABI — no OOP
wrapper. See [docs/getting-started.md](docs/getting-started.md) for the full
walkthrough including `entropic install-engine` (downloads the matching
`librentropic.so` from GitHub Releases).

```python
import ctypes
from entropic import (
    entropic_create, entropic_configure_dir,
    entropic_run_streaming, entropic_destroy,
)

handle = ctypes.c_void_p()
entropic_create(ctypes.byref(handle))
entropic_configure_dir(handle, b".myapp")

@ctypes.CFUNCTYPE(None, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p)
def on_token(tok, n, ud):
    print(tok.decode("utf-8", errors="replace"), end="", flush=True)

entropic_run_streaming(handle, b"What is 2 + 2?", on_token, None, None)
entropic_destroy(handle)
```

### Configuration

Configuration loads in layers (highest priority wins):

1. **Compiled defaults** — struct initializers built into the engine
2. **Consumer defaults** (`default_config.yaml` in CWD) — shipped with your app
3. **Global** (`~/.entropic/config.yaml`) — user machine-wide settings
4. **Project local** (`{project_dir}/config.local.yaml`) — per-project overrides
5. **Environment** (`ENTROPIC_*` variables)

Minimal config:

```yaml
models:
  lead:
    path: primary           # Resolves via bundled model registry
    adapter: qwen35
    context_length: 16384
  default: lead

routing:
  enabled: false

mcp:
  enable_entropic: true     # Internal tools (delegate, complete, etc.)
  enable_filesystem: false
  enable_bash: false

permissions:
  auto_approve: true        # Skip tool approval prompts
```

### Session Logging

When using `entropic_configure_dir()`, the engine automatically creates:

| File | Contents |
|------|----------|
| `{project_dir}/session.log` | Engine operations — config, routing, timing, errors |
| `{project_dir}/session_model.log` | Full user/assistant exchanges — streamed in real time |

## Features

Comprehensive engine capability inventory grouped by domain, with the
primary source file for each item so the README doubles as a navigation
map. For the historical "how we got here" narrative see
[docs/roadmap.md](docs/roadmap.md); for the layering behind these
features see [docs/architecture-cpp.md](docs/architecture-cpp.md).

### Agentic Loop

- Generate → parse tool calls → execute → re-generate, with explicit
  state machine (IDLE → GENERATING → EXECUTING → VERIFYING → COMPLETE) — `src/core/engine.cpp::loop, execute_iteration`, `include/entropic/core/engine_types.h::AgentState`
- Streaming generation with cancel-token plumbed end-to-end — `src/core/response_generator.cpp::generate_streaming`
- Per-iteration hook lifecycle: `ON_LOOP_ITERATION`, `ON_CONTEXT_ASSEMBLE`, `PRE_GENERATE` (cancellable), `POST_GENERATE` (revisable), `PRE_TOOL_CALL`, `POST_TOOL_CALL` — `src/core/engine.cpp::execute_iteration, dispatch_post_generate`
- `LoopMetrics` surfaced via `last_loop_metrics()` and per-tier accessors (iterations, tool calls, tokens, duration, errors) — `include/entropic/core/engine_types.h::LoopMetrics`
- Iteration count + budget exposed to the model each turn as a system reminder so identity prompts can teach budget-aware rules the model can actually enforce — `src/core/response_generator.cpp::inject_engine_state_reminder`
- Anti-spiral primitive: warns the model after N consecutive calls of the same tool; threshold via `LoopConfig.max_consecutive_same_tool` — `src/mcp/tool_executor.cpp::update_anti_spiral_tracking`
- Synthetic completion forced when iteration cap is hit, with terminal-reason metadata for caller diagnostics — `src/core/engine.cpp::loop` (the post-while terminal-reason block)
- Per-identity overrides for `max_iterations` and `max_tool_calls_per_turn` from frontmatter — `src/facade/entropic.cpp::cache_per_tier_frontmatter`, `include/entropic/core/engine_types.h::effective_max_iterations`

### Identity System & Delegation

- Multi-tier identity-based delegation: same model, different per-identity prompt + tools + grammar + delegation targets — `src/core/identity_manager.cpp`, `src/core/delegation.cpp`
- Tier locks at first generation; children run the engine loop independently with their own context and locked tier — `src/core/response_generator.cpp::lock_tier_if_needed`
- `entropic.delegate` (single child) and `entropic.pipeline` (multi-stage) directives — `src/mcp/servers/entropic_server.cpp`, `src/core/directives.cpp`
- `entropic.complete` for explicit child-completion summaries — `src/mcp/servers/entropic_server.cpp`
- Delegation depth tracking + ancestor-tier stack for cycle detection and context-bleed isolation — `include/entropic/core/engine_types.h::delegation_depth, delegation_ancestor_tiers`
- Single-delegate relay: skip lead re-synthesis when only one delegate fired and the parent identity allows passthrough — `src/core/engine.cpp::finalize_delegation_result, relay_partial_result`
- Budget-exhausted child relay: validate-and-relay partial summaries with `[partial — budget_exhausted]` prefix and verdict-tagged metadata, instead of silently dropping — `src/core/engine.cpp::relay_partial_result`, `src/core/delegation.cpp::build_child_result`
- Conversation parent/child ID linking through SQLite storage — `src/storage/database.cpp`, `include/entropic/core/engine_types.h::parent_conversation_id`
- Worktree session isolation for delegation — `src/core/worktree.cpp`, `include/entropic/core/worktree.h`

### Validation

- Constitutional validator with critique-revise sub-loop and explicit `ValidationVerdict` enum (passed / revised / rejected_max_revisions / skipped) — `src/core/constitutional_validator.cpp`, `include/entropic/types/validation.h`
- Per-tier `validation_rules` from identity frontmatter; constitution is background context when per-tier rules exist — `src/core/constitutional_validator.cpp::build_critique_prompt, set_tier_rules`
- Validator can revise content via `POST_GENERATE` hook (Path A: structured JSON, Path B: full message-context revision) — `src/core/constitutional_validator.cpp::handle_hook, attempt_revision`
- Validation rule visibility on retry: when validator rejects, the next turn's system reminder surfaces the violated rule text — `src/core/engine.cpp::capture_validation_feedback`, `src/core/response_generator.cpp::inject_engine_state_reminder`
- `ON_COMPLETE` hook for summary validation; can reject and inject feedback as a user-role message tagged `[CITATION VALIDATION] …` — `src/core/engine.cpp::fire_complete_hook`
- Per-identity validation enable/disable overrides — `src/core/constitutional_validator.cpp::set_identity_validation`

### Tools / MCP

Built-in MCP servers (each shipped as a plugin):

| Server | Tools | Source |
|--------|-------|--------|
| `entropic` | delegate, pipeline, complete, diagnose, inspect, context_inspect | `src/mcp/servers/entropic_server.cpp` |
| `filesystem` | read_file, write_file, edit_file, glob, grep | `src/mcp/servers/filesystem.cpp` |
| `bash` | execute | `src/mcp/servers/bash.cpp` |
| `git` | status, diff, log, commit, branch, checkout | `src/mcp/servers/git.cpp` |
| `diagnostics` | diagnostics, check_errors | `src/mcp/servers/diagnostics.cpp` |
| `web` | web_fetch, web_search | `src/mcp/servers/web.cpp` |

External MCP servers connect at runtime via stdio or SSE transport:

```c
entropic_register_mcp_server(h, "chess",
    "{\"command\":\"python3\",\"args\":[\"chess_server.py\"]}");
```

Cross-cutting machinery:

- Plugin architecture: external servers loaded via `dlopen` + `entropic_create_server()` factory at runtime — `src/mcp/server_manager.cpp`, `include/entropic/interfaces/i_mcp_server.h`
- Stdio + SSE transports for external clients — `src/mcp/transport_stdio.cpp`, `src/mcp/transport_sse.cpp`, `src/mcp/external_client.cpp`
- Reconnect policy + health monitoring for external MCP — `src/mcp/reconnect_policy.cpp`, `src/mcp/health_monitor.cpp`
- Per-identity `allowed_tools` authorization from frontmatter — `src/facade/engine_handle.h::tier_allowed_tools`, `src/prompts/manager.cpp`
- MCP key-set authorization for granular per-tool permissions — `src/mcp/mcp_key_set.cpp`, `src/mcp/mcp_authorization.cpp`
- Permission persist interface (allow/deny across sessions) — `src/mcp/permission_manager.cpp`, `src/storage/permission_persister.cpp`
- Tool-call history ring buffer for diagnostics and validator critique-prompt assembly — `src/mcp/tool_call_history.cpp`
- Duplicate tool-call detection with circuit breaker after N exact repeats — `src/mcp/tool_executor.cpp::check_duplicate, handle_duplicate`
- Typed `result_kind` on every `POST_TOOL_CALL` payload (`ok` / `ok_empty` / `error` / `rejected_duplicate` / `rejected_schema` / `rejected_precondition`) — `include/entropic/types/tool_result.h`
- Empty-vs-content success distinction (`ok_empty`) for pivot-on-empty rules — `src/mcp/tool_result_classify.cpp::is_effectively_empty`
- Tightened error detection (case-insensitive, bracketed, JSON-shaped `{"error": …}`) — `src/mcp/tool_result_classify.cpp::looks_like_tool_error`
- Tool result preview length cap with truncation marker (`max_tool_result_bytes`, default 16 KB; 0 disables) — `src/mcp/tool_result_classify.cpp::truncate_to_cap`, `src/mcp/tool_executor.cpp::apply_result_size_cap`
- UTF-8 sanitization on inbound tool results: invalid bytes replaced with U+FFFD before any downstream JSON serialization — `src/mcp/utf8_sanitize.cpp`, `include/entropic/mcp/utf8_sanitize.h`
- Path-traversal protection in the filesystem server — `src/mcp/servers/filesystem.cpp`
- Tier-scoped assembled-prompt inspection: `entropic.inspect identity <tier>` returns the full system prompt — `src/facade/entropic.cpp::sp_get_identities, build_assembled_prompt_for_tier`
- Tool argument JSON schema validation (required-fields + enum + type checks) — `src/mcp/tool_executor.cpp::check_required_fields, check_enum`

### Inference

- llama.cpp backends as compile-time variants: CUDA, Vulkan, CPU — `src/inference/llama_cpp_backend.cpp`, `src/inference/orchestrator.cpp`
- Native SASS kernels for Maxwell → Blackwell; PTX fallback for Jetson / embedded — see `docs/dist-README.md` for the arch matrix
- Multiple chat adapters (Qwen3.5, generic) pluggable via `AdapterRegistry` — `src/inference/adapters/`, `src/inference/adapter_manager.cpp`
- VRAM lifecycle state machine (COLD / WARM / ACTIVE) with explicit load / activate / unload boundaries — `src/inference/llama_cpp_backend.cpp`, `src/inference/orchestrator.cpp`
- Streaming generation with token observers — per-call `on_token` AND a global `set_stream_observer()` that fires for every token from every entry point — `src/core/response_generator.cpp::stream_token_callback`, `src/facade/entropic.cpp::entropic_set_stream_observer`
- Persistent stream observer: registered once, fires across `entropic_run_streaming()` reassignment so consumers don't lose events on inner loop iterations — `src/core/response_generator.cpp::stream_observer_`
- GBNF grammar constraints applied per-tier:

  ```gbnf
  root ::= analysis best-move tool-call
  analysis ::= "Analysis: " sentence "\n"
  best-move ::= "Best move: " uci "\n"
  tool-call ::= "<tool_call>" json "</tool_call>"
  ```

  Output is structurally correct by construction; no post-processing, no retries — `src/inference/grammar_registry.cpp`

- LoRA adapter hot-swap (`entropic_adapter_load`, `_unload`, `_swap`, `_state`, `_info`) — `src/facade/entropic.cpp` (entropic_adapter_*)
- Logprob evaluation API for downstream perplexity / scoring — `src/inference/llama_cpp_backend.cpp`
- Per-tier `GenerationParams`: temperature, top_p, top_k, repeat_penalty, max_tokens, seed, reasoning_budget — `src/inference/profile_registry.cpp`, `src/types/config.cpp`
- KV-cache prefix save/load via `llama_state_seq_get_data` / `llama_state_seq_set_data` (host-memory prompt cache); identity / constitution / tool prefixes hot-swap on identity change — `src/inference/prompt_cache.cpp`
- Multimodal support hooks (vision adapter slot wired in v1.9.11) — `src/inference/image_preprocessor.cpp`, `src/inference/adapters/qwen35_adapter.cpp`

### Configuration

- Layered config resolution (highest priority wins) — `src/config/loader.cpp`, `src/config/env_overrides.cpp`:

  1. Compiled defaults (struct initializers in the engine)
  2. Consumer defaults — `default_config.yaml` next to your binary
  3. Global — `~/.entropic/config.yaml`
  4. Project local — `{project_dir}/config.local.yaml`
  5. Environment — `ENTROPIC_*` variables

- Cross-field validation: routing references, default tier consistency, threshold ordering, identity references — `src/config/validate.cpp`
- Bundled model registry (`path: primary` resolves via `bundled_models.yaml` to a vetted GGUF) — `src/config/bundled_models.cpp`, `data/bundled_models.yaml`
- `entropic download primary` fetches into `~/.entropic/models/` with resume support — `src/cli/download.cpp`
- Bundled data file discovery: compile-time `DATA_DIR` define, overridable at runtime via `config_dir` — `src/config/data_dir.cpp`
- Per-identity frontmatter parsing: `allowed_tools`, `validation_rules`, `relay`, `max_iterations`, `max_tool_calls_per_turn` — `src/prompts/manager.cpp`, `include/entropic/prompts/manager.h::IdentityFrontmatter`

### Storage / Persistence

- SQLite-backed conversation storage with parent/child linkage — `src/storage/database.cpp`, `src/storage/backend.cpp`
- Pluggable `StorageInterface` for downstream backends — `include/entropic/interfaces/i_storage.h`, `src/storage/c_interface.cpp`
- Compaction strategies: token-budget triggers, custom compactor registration, snapshot-via-storage path — `src/core/compaction.cpp`, `src/core/compactor_registry.cpp`
- Audit log: structured per-action records of tool dispatches, permission decisions, identity transitions — `src/storage/audit_logger.cpp`, `src/storage/audit_entry.cpp`
- Session loggers: `session.log` (engine ops, routing, timing) + `session_model.log` (full transcripts streamed live) — `src/types/session_logger.cpp`

### Hooks (Extensibility)

20+ hook points across the loop lifecycle. All registration / dispatch lives in `src/core/hook_registry.cpp` and `include/entropic/types/hooks.h`.

- **Lifecycle**: `ON_LOOP_START`, `ON_LOOP_ITERATION`, `ON_LOOP_END` — fired in `src/core/engine.cpp::loop`
- **Generation**: `PRE_GENERATE` (cancellable), `POST_GENERATE` (content-revisable), `ON_STREAM_TOKEN` — `src/core/engine.cpp::execute_iteration`, `src/core/response_generator.cpp::stream_token_callback`
- **Tools**: `PRE_TOOL_CALL`, `POST_TOOL_CALL`, `ON_PERMISSION_CHECK` — `src/mcp/tool_executor.cpp::fire_pre_tool_hook, fire_post_tool_hook`
- **Delegation**: `ON_DELEGATE`, `ON_COMPLETE`, `ON_TIER_SELECTED` — `src/core/engine.cpp::fire_complete_hook`
- **Context**: `ON_CONTEXT_ASSEMBLE`, `ON_ADAPTER_SWAP` — `src/core/context_manager.cpp`, `src/inference/adapter_manager.cpp`

Hook semantics:

- Pre-hooks return non-zero to cancel the operation
- Post-hooks return modified content to revise the engine's view
- Info-level hooks are fire-and-forget
- Multiple callbacks per hook point with priority ordering
- Registration / deregistration at any time during engine lifetime — `src/facade/entropic_hooks.cpp`

### External MCP Bridge

- `entropic mcp-bridge` — runs the engine as a JSON-RPC MCP server over stdio, the primary integration point for Claude Code, VSCode, and other MCP-protocol clients — `src/cli/mcp_bridge.cpp`, `src/facade/external_bridge.cpp`
- `entropic mcp-connect --socket PATH` — client-side companion that attaches to a running engine's bridge socket — `src/cli/mcp_connect.cpp`
- Multi-client subscription: TUI + Claude Code can both receive `ask_complete` / progress events simultaneously — `src/facade/external_bridge.cpp::subscribe, broadcast_notification`
- Async ask via push notification + `ask_status` polling for long-running tasks — `src/facade/external_bridge.cpp::run_async_ask, handle_ask_status`
- Phase observer: VERIFYING → "validating" / "revising" sub-phases surfaced to bridge subscribers — `src/facade/external_bridge.cpp::attach_phase_observer, phase_observer_cb`
- Generation counter on the phase observer: in-flight stale callbacks are guaranteed no-ops after detach (race-safe under concurrent cancel) — `src/facade/external_bridge.cpp::observer_call_is_stale`, `include/entropic/mcp/external_bridge.h::observer_gen_`
- Cancel-on-clear semantics: `entropic.context_clear` interrupts and drains in-flight async tasks before returning — `src/facade/external_bridge.cpp::cancel_inflight_async_tasks`

### Distribution

- **Pure C ABI** at every `.so` boundary — opaque handles, error codes via enum, no C++ ABI crossing, no exceptions across the boundary — `include/entropic/entropic.h`, `src/facade/entropic.cpp`
- `find_package(entropic 2.1)` CMake support with imported target `entropic::entropic` — `cmake/entropic-config.cmake.in`, `CMakeLists.txt`
- Tarball layout: `bin/`, `lib/`, `include/`, `share/` — standard Unix prefix — see `docs/releasing.md`
- `pip install entropic-engine` — pure-Python ~50 KB ctypes wrapper + `entropic install-engine` subcommand that fetches the matching tarball from GitHub Releases (playwright pattern) — `python/src/entropic/`
- `$ENTROPIC_LIB` / `$ENTROPIC_HOME` env vars for custom install resolution — `python/src/entropic/_loader.py`
- `entropic` CLI subcommands: `mcp-bridge`, `mcp-connect`, `download`, `version`, plus wrapper-side `install-engine` — `src/cli/main.cpp`, `python/src/entropic/cli.py`
- Reference examples: `headless` (C), `pychess` (C++ multi-tier showcase), `explorer` (interactive REPL), `openai-server` (OpenAI-compat HTTP front-end with chat/completions, completions, models, models/{name}, health, SSE streaming) — `examples/`

### Observability

- Per-subsystem structured logging via `spdlog` (e.g. `mcp.tool_executor`, `core.response_generator`, `inference.orchestrator`) — `src/types/logging.cpp`
- `LoopMetrics` exposed through `last_loop_metrics()` and per-tier accessor maps — `include/entropic/core/engine.h::last_loop_metrics, per_tier_metrics`
- `ThroughputTracker` integration on `GenerationResult` — `src/inference/throughput_tracker.cpp`
- `entropic_throughput_tok_per_sec()` facade query — `src/facade/entropic.cpp`
- 20+ hook points doubling as telemetry consumption points — `src/core/hook_registry.cpp`
- `session.log` + `session_model.log` per project_dir — `src/types/session_logger.cpp`
- `doxygen-guard` enforces inline documentation on every function (`@brief`, exemption tag, `@version` bumped on body change) — `.doxygen-guard.yaml`, `.pre-commit-config.yaml`
- `knots` enforces code-quality budget per function (cognitive complexity ≤ 15, McCabe ≤ 15, nesting ≤ 4, SLOC ≤ 50, ABC ≤ 10, returns ≤ 3) — `.pre-commit-config.yaml`
- Per-library coverage gates via `gcovr` enforced by `inv check-coverage` — `tasks.py::check_coverage`, `.pre-commit-config.yaml`

### Quality / Testing

- 16 pre-commit hooks: trim whitespace, end-of-file, ruff, ruff-format, flake8, knots, doxygen-guard, build, unit tests, per-library coverage, plus standard pre-commit checks — `.pre-commit-config.yaml`
- 751 unit + regression tests (Catch2 v3 BDD style) — `tests/unit/`
- 30 model tests, GPU-required, developer-run; results attached to the GitHub Release as `model-results-vX.Y.Z.json` at each x.y.0 — `tests/model/`, `tasks.py::test`
- `tests/distribution-smoke-consumer/` exercises the `find_package(entropic)` consumer experience end-to-end — `tests/distribution-smoke-consumer/`
- Test gating per CLAUDE.md:

  | Gate | When | What runs |
  |---|---|---|
  | Pre-commit | Every commit | Unit tests (CPU, no GPU) |
  | Minor version | Each x.y.0 bump | Full model/benchmark suite (GPU) |
  | Patch version | Each x.y.z bump | Unit tests only |

### Privacy

- Zero network calls in the inference hot path
- All processing local; no telemetry collected
- Conversation data, prompts, and model outputs stay on the host
- Only outbound network call is the optional `web` MCP server's `web_fetch` / `web_search` tools, which the consumer opts into explicitly via config — `src/mcp/servers/web.cpp`

## Bundled Models

| Key | Model | Size | VRAM |
|-----|-------|------|------|
| `primary` | Qwen3.5-35B-A3B-UD-IQ3_XXS | 13.1 GB | 15+ GB |
| `mid` | Qwen3.5-9B-Q8_0 | 9.5 GB | 12+ GB |
| `lightweight` | Qwen3.5-4B-Q8_0 | 4.5 GB | 8+ GB |

Use `path: primary` (or `mid`, `lightweight`) in config — the engine resolves
to the full model path via the bundled registry.

## Build Presets

| Preset | Description | Use Case |
|--------|-------------|----------|
| `full` | CUDA, all servers, tests | Development workstation |
| `dev` | CPU, debug, tests | Fast iteration |
| `minimal-static` | CPU, static `.a`, minimal servers | Embedded consumer |
| `game` | CUDA, minimal MCP servers | Game engine integration |
| `coverage` | CPU, gcov instrumentation | Coverage analysis |

```bash
inv build --clean              # full (CUDA)
inv build --cpu                # dev (CPU)
inv test --cpu --no-build      # 751 unit + regression tests
inv test --model --no-build    # 30 model tests (GPU required)
```

## Examples

| Example | Demonstrates | Language |
|---------|-------------|----------|
| `headless/main.c` | Pure-C minimal harness — CI smoke target | C |
| `pychess/main.cpp` | Multi-tier pipeline, grammar, delegation, external MCP | C++ |
| `explorer/main.cpp` | Interactive C++ REPL for poking at the engine | C++ |
| `openai-server/src/main.cpp` | OpenAI-compat HTTP front-end (chat/completions, models, SSE streaming) | C++ |

Each example has its own `default_config.yaml` (consumer defaults) and
`.{name}/` directory (session logs, local config).

## Privacy

Entropic runs entirely on your local hardware. No data is sent to external
servers. No telemetry is collected. Your prompts, conversations, and model
outputs never leave your machine.

## Disclaimer

Entropic runs AI models locally on your hardware. AI-generated outputs may be
inaccurate, biased, or inappropriate. Users are solely responsible for
evaluating and using any generated content. This software does not provide
professional, legal, medical, or financial advice.

## Documentation

- [docs/getting-started.md](docs/getting-started.md) — install + first call
- [docs/architecture-cpp.md](docs/architecture-cpp.md) — library design
- [docs/roadmap.md](docs/roadmap.md) — version targeting
- [docs/contributing.md](docs/contributing.md) — dev setup, gates, branching
- [docs/releasing.md](docs/releasing.md) — release workflow
- [docs/security.md](docs/security.md) — vulnerability reporting

## License

LGPL-3.0-or-later, with a linking exception that lets applications
dynamically or statically link `librentropic.so` without being
themselves required to ship under LGPL.

- [`LICENSE`](LICENSE) — canonical LGPL-3.0 text
- [`NOTICE`](NOTICE) — project-specific additions: the linking
  exception, dual-license / version history, and third-party
  attribution

Versions prior to 2.0.0 were released under Apache-2.0; that
license continues to apply to the version you received.
