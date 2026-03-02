---
version: 1.0.0
type: proposal
schema_version: 1
id: P2-20260219-019
title: "Engine Decomposition — Extract Cohesive Subsystems"
priority: P2
component: core
author: tvanfossen
author_email: vanfosst@gmail.com
created: 2026-02-19
updated: 2026-03-02
tags: [refactoring, engine, separation-of-concerns, architecture]
completed_date: null
scoped_files:
  - "src/entropic/core/engine.py"
  - "src/entropic/core/tool_executor.py"
  - "src/entropic/core/context_manager.py"
  - "src/entropic/core/response_generator.py"
  - "tests/unit/test_engine.py"
  - "tests/unit/test_tool_executor.py"
  - "tests/unit/test_context_manager.py"
depends_on: []
blocks: []
---

# Engine Decomposition — Extract Cohesive Subsystems

## Problem Statement

`engine.py` is 1,528 lines with 68 methods spanning 6 responsibilities:

| Category | Methods | Lines (est) |
|----------|---------|-------------|
| Tool execution + approval | 20 | ~400 |
| Streaming/generation | 11 | ~250 |
| Context management | 10 | ~200 |
| Directive processing | 9 | ~150 |
| Loop control | 6 | ~200 |
| Public API + state | 12 | ~200 |

The identity library (P1-024) will add interstitial tier support, pipeline
config, and model reuse detection to this file. Without decomposition, the
engine grows past 1,800+ lines and becomes unmaintainable.

**Why now, not later:** The identity library adds new methods to tool
execution (tier-allowed filtering), context management (interstitial
context handoff), and generation (model reuse detection). Adding these
to a 1,528-line file makes them hard to test. Extracting first means
identity library code goes into focused, testable modules.

## Current Method Inventory

### Tool Execution (20 methods → `ToolExecutor`)

| Method | Line | Description |
|--------|------|-------------|
| `_sort_tool_calls` | 934 | Sort so entropic.handoff is last |
| `_process_tool_calls` | 943 | Process calls, yield results, handle dupes |
| `_get_tool_call_key` | 1016 | Generate unique key for dedup |
| `_check_duplicate_tool_call` | 1021 | Check if dupe, return cached result |
| `_record_tool_call` | 1030 | Record for dedup tracking |
| `_create_duplicate_message` | 1036 | Create "skipped duplicate" message |
| `_execute_tool` | 1051 | Approval → start → execute → complete |
| `_check_tier_allowed` | 1097 | Reject tools not in tier's allowed_tools |
| `_check_tool_approval` | 1117 | Check approval, return denial if needed |
| `_do_execute_tool` | 1133 | Execute via ServerManager |
| `_log_tool_success` | 1169 | Log + notify success |
| `_handle_tool_error` | 1186 | Handle permission/exception errors |
| `_is_tool_approved` | 1222 | Auto-approve check |
| `_convert_approval_result` | 1242 | Convert callback result to bool |
| `_should_auto_approve` | 1252 | Check auto-approve list |
| `_get_approval_result` | 1260 | Get approval from callback |
| `_handle_approval_result` | 1268 | Handle ToolApproval + save to config |
| `_get_permission_pattern` | 1281 | Generate permission pattern |
| `_create_denied_message` | 1292 | Create denial message |
| `_create_error_message` | 1304 | Create error message |

**Cohesion:** High. These methods form a self-contained approval +
execution pipeline with clear inputs (tool call) and outputs (result
message). Only dependency on Engine is callbacks and ServerManager.

### Context Management (10 methods → `ContextManager`)

| Method | Line | Description |
|--------|------|-------------|
| `_get_max_context_tokens` | 388 | Get max context tokens from config |
| `_refresh_context_limit` | 396 | Update limit when model changes |
| `_filter_tools_for_tier` | 654 | Filter tools by tier's allowed_tools |
| `_create_assistant_message` | 907 | Create assistant message (prevent empty) |
| `_prune_tool_results` | 1319 | Replace old tool results with stubs |
| `_prune_old_tool_results` | 1352 | Auto-prune by TTL |
| `_inject_context_warning` | 1385 | Inject context usage warning |
| `_check_compaction` | 1453 | Check + perform compaction |
| `_reinject_context_anchors` | 1471 | Reinject anchors after compaction |

**Cohesion:** Medium-high. Context limit tracking, pruning, compaction,
and anchor management are all context lifecycle operations. Tool filtering
is borderline (could live in ToolExecutor), but it operates on the context
window's tool availability.

### Directive Processing (9 methods — keep in Engine)

| Method | Line | Description |
|--------|------|-------------|
| `_register_directive_handlers` | 237 | Register handler callbacks |
| `_directive_stop_processing` | 257 | Handle stop_processing |
| `_directive_tier_change` | 267 | Handle tier_change |
| `_directive_clear_self_todos` | 313 | Handle clear_self_todos |
| `_directive_inject_context` | 327 | Handle inject_context |
| `_directive_prune_messages` | 338 | Handle prune_messages |
| `_directive_context_anchor` | 351 | Handle context_anchor |
| `_directive_notify_presenter` | 378 | Handle notify_presenter |
| `_process_directives` | 1079 | Process directives from tool result |

**Decision: Keep in Engine.** Directives are the engine's control plane —
they change tier, modify context, halt processing. They touch multiple
subsystems (tier change affects generation, prune affects context, stop
affects loop). Extracting them would create a class that needs references
to every other subsystem. The current handler registration pattern is
clean and self-contained.

### Streaming/Generation (11 methods → `ResponseGenerator`)

| Method | Line | Description |
|--------|------|-------------|
| `_log_model_output` | 611 | Log raw + parsed output |
| `_log_assembled_prompt` | 641 | Log complete prompt |
| `_should_auto_chain` | 671 | Check auto-chain trigger |
| `_try_auto_chain` | 686 | Attempt auto-chain handoff |
| `_build_formatted_system_prompt` | 708 | Build system prompt for adapter |
| `_lock_tier_if_needed` | 726 | Route + lock tier |
| `_notify_tier_selected` | 750 | Emit tier selection callback |
| `_generate_response` | 755 | Generate (streaming or non-streaming) |
| `_generate_streaming` | 771 | Streaming with interrupt/pause |
| `_generate_non_streaming` | 806 | Non-streaming generation |
| `_handle_pause` | 821 | Handle pause during generation |
| `_continue_with_injection` | 862 | Continue with injected context |

**Cohesion:** Medium. Generation + auto-chain + tier locking form a unit.
Pause/injection is tightly coupled to streaming. Logging is ancillary but
shares the same data.

## Proposed Architecture

```
Engine (core loop — ~300 lines)
├── owns LoopContext, state machine, public API
├── owns DirectiveProcessor (inline — 9 methods)
├── delegates to:
│   ├── ToolExecutor      (~400 lines, 20 methods)
│   │   └── owns: ServerManager ref, approval logic, dedup cache
│   ├── ContextManager    (~200 lines, 10 methods)
│   │   └── owns: context limits, pruning, compaction, anchors
│   └── ResponseGenerator (~250 lines, 11 methods)
│       └── owns: streaming, tier locking, prompt building, auto-chain
└── EngineCallbacks flows through all (passed at init)
```

### Answers to Open Questions

**Inner classes or top-level?** Top-level in `core/`. Each subsystem gets
its own file and test file. Import cycle avoided because subsystems don't
import Engine — Engine imports them.

**Does ToolExecutor own ServerManager?** Yes. Engine passes ServerManager
at construction. ToolExecutor holds the reference and uses it for
`execute_tool()` calls. Engine never calls ServerManager directly.

**Consumer customization?** Constructor injection. Engine takes optional
`tool_executor`, `context_manager`, `response_generator` params. Defaults
to standard implementations. Consumer subclasses the subsystem, not Engine.

```python
class Engine:
    def __init__(
        self,
        orchestrator: ModelOrchestrator,
        config: LibraryConfig,
        server_manager: ServerManager | None = None,
        tool_executor: ToolExecutor | None = None,
        context_manager: ContextManager | None = None,
        response_generator: ResponseGenerator | None = None,
    ):
        self._server_manager = server_manager or self._create_default_server_manager()
        self._tool_executor = tool_executor or ToolExecutor(
            self._server_manager, config, self._callbacks
        )
        self._context_manager = context_manager or ContextManager(
            config, self._orchestrator
        )
        self._response_generator = response_generator or ResponseGenerator(
            self._orchestrator, config, self._callbacks
        )
```

## Constraints

- **Public API unchanged.** `Engine.run()`, `Engine.set_callbacks()`,
  `Engine.pause/resume/interrupt` signatures preserved.
- **EngineCallbacks flows through all.** Subsystems receive callbacks at
  construction (or via setter when `set_callbacks()` is called).
- **LoopContext passed by reference.** Subsystems receive `ctx` per-call,
  not at construction. Avoids stale state.
- **No premature abstraction.** Concrete classes, no ABCs. If a consumer
  needs to override, they subclass the concrete class.
- **Directive handlers stay in Engine.** They're the control plane.

## Acceptance Criteria

- [ ] `ToolExecutor` extracted to `src/entropic/core/tool_executor.py`
      with all 20 tool execution methods
- [ ] `ContextManager` extracted to `src/entropic/core/context_manager.py`
      with all 10 context management methods
- [ ] `ResponseGenerator` extracted to `src/entropic/core/response_generator.py`
      with all 11 generation methods
- [ ] Engine reduced to ~300 lines: loop control, directives, public API
- [ ] Constructor injection works: custom subsystems can replace defaults
- [ ] All existing unit tests pass without modification
- [ ] All existing model tests pass without modification
- [ ] New unit tests: ToolExecutor tested in isolation (mock ServerManager)
- [ ] New unit tests: ContextManager tested in isolation (mock orchestrator)
- [ ] `engine.py` imports subsystems, subsystems do NOT import Engine

## Implementation Plan

### Phase 1: Extract ToolExecutor

Largest and most cohesive extraction. Low risk — tool execution has clear
boundaries (input: tool call, output: result message).

1. Create `src/entropic/core/tool_executor.py`
2. Move all 20 tool execution methods
3. ToolExecutor `__init__` takes: `server_manager`, `config`, `callbacks`
4. Engine creates ToolExecutor, delegates `_process_tool_calls`
5. Run pre-commit — all tests must pass

### Phase 2: Extract ContextManager

Medium cohesion. Context limits, pruning, compaction, anchors.

1. Create `src/entropic/core/context_manager.py`
2. Move all 10 context management methods
3. ContextManager `__init__` takes: `config`, `orchestrator`
4. Engine creates ContextManager, delegates context operations
5. Run pre-commit — all tests must pass

### Phase 3: Extract ResponseGenerator

Generation, streaming, tier locking, auto-chain.

1. Create `src/entropic/core/response_generator.py`
2. Move all 11 generation methods
3. ResponseGenerator `__init__` takes: `orchestrator`, `config`, `callbacks`
4. Engine creates ResponseGenerator, delegates generation
5. Run pre-commit — all tests must pass

### Phase 4: Constructor Injection + Tests

1. Add optional params to Engine `__init__` for subsystem injection
2. Write isolated unit tests for each subsystem
3. Verify consumer can subclass and inject custom subsystem
4. Run pre-commit — all tests must pass

## Risks & Considerations

- **Method interdependencies.** Some methods call across categories (e.g.,
  `_execute_iteration` calls tool execution + context management +
  generation). These call sites stay in Engine — Engine orchestrates,
  subsystems execute.
- **Callback threading.** Callbacks are set after construction via
  `set_callbacks()`. Subsystems need to receive updated callbacks.
  Solution: subsystems hold a reference to `self._callbacks` dict,
  which Engine updates in place.
- **LoopContext mutation.** Multiple subsystems mutate `ctx.messages`,
  `ctx.locked_tier`, etc. This is intentional — LoopContext is the
  shared mutable state of the loop. Document clearly.

## Implementation Log

{Entries added as work progresses}

## References

- Adversarial analysis of P1-011 (original identification)
- Identity library (P1-024) will add methods to engine — decompose first
- Engine method inventory: 68 methods across 1,528 lines (as of 2026-03-02)
