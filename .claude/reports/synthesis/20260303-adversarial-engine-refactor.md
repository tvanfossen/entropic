# Adversarial Analysis: Engine Lazy Factory / Thin Delegation Removal

**Date:** 2026-03-03
**Scope:** Proposed removal of lazy factories (`_ensure_response_generator`, `_ensure_context_manager`) and 6 thin delegation methods from `engine.py`, replacing with eager construction + constructor injection + direct subsystem calls.

---

## Verdict: DO IT — with one constraint

Remove lazy factories and all thin delegations. Add eager construction. Do NOT add constructor injection params (CFQ002 blocker). Tests mock at the subsystem level.

---

## Critical Blockers

### CFQ002: Constructor injection params are dead on arrival

Explorer confirmed: `AgentEngine.__init__` is currently 5 params (including self). Adding `context_manager` and `response_generator` = 7. CFQ002 hard limit is 6. Pre-commit fails.

**Resolution:** No constructor injection params. Subsystems are eagerly created in `__init__` and can be replaced post-construction via direct attribute assignment (`engine._context_manager = custom_cm`). This is less discoverable than constructor injection but sufficient for test and consumer use cases. Revisit if CFQ002 limit can be raised or a `SubsystemOverrides` dataclass is introduced.

---

## Findings by Persona

### Explorer (factual)

- All 6 thin delegations are called only from within `engine.py` — no cross-file callers.
- Only **2 of 6** are actually mocked in tests: `_check_compaction` (engine.py:348, test_engine_feedback_roles.py:159) and `_inject_context_warning` (engine.py:266, test_engine_feedback_roles.py:160).
- ResponseGenerator and ContextManager dependencies are ALL available at `__init__` time — eager construction has zero ordering constraints.
- ToolExecutor **must remain lazy**: `server_manager` is None at init, set async in `run()`.
- Consumer construction patterns (pychess, app.py, hello-world, model tests) are all unaffected by removing lazy factories.

### KISS (complexity)

- `_ensure_response_generator()` and `_ensure_context_manager()` are cargo-cult lazy init — "for consistency with other subsystems" (engine.py:119 comment) is not a technical reason.
- 6 thin delegations are cognitive speed bumps with zero functional value. Reading `_execute_iteration` requires 2 extra jumps to understand `await self._check_compaction(ctx)` → delegation → subsystem.
- Direct calls (`await self._context_manager.check_compaction(ctx)`) are immediately readable with type-checked subsystem signatures.
- **Simplest form: eager init + direct calls + no injection params.** Tests mock subsystems directly.

### DRY (duplication)

- 4 of the 6 delegations are never mocked: `_generate_response`, `_refresh_context_limit`, `_prune_old_tool_results`, `_prune_tool_results`. Pure noise — identical behavior to calling the subsystem directly.
- 2 delegations are mocked but tests can be updated to mock the subsystem: `engine._context_manager = MagicMock()`.
- Hook dataclasses (`ToolExecutorHooks`, `ContextManagerHooks`) — coincidentally similar, not worth abstracting. Keep in owning subsystems.
- The `or ContextManager(...)` injection idiom scales cleanly to 4-5 subsystems without abstraction.

### Negative (failure modes)

- **Inconsistent lazy/eager** after the change: ToolExecutor stays lazy, RG/CM go eager. This is justified (ToolExecutor has a genuine constraint), but the **asymmetry must be documented clearly** in __init__ so future developers understand the rule.
- Test mocking loses granularity: `engine._check_compaction = AsyncMock()` becomes `engine._context_manager.check_compaction = AsyncMock()`. Slightly more verbose. NOT more complex — this is mocking at the correct abstraction level.
- "Silent construction cost" concern: `AgentEngine(orchestrator)` builds RG and CM. Both constructors are cheap (just store refs). Not a real issue.
- `assert self.server_manager is not None` at engine.py:557 is stripped by `-O`. Should be a proper RuntimeError. Orthogonal to this change but worth fixing.

### Positive (extensibility)

- Constructor injection (even via post-construction assignment) enables: custom compaction strategies, tight test isolation, observability instrumentation, staged initialization. The architecture supports this today once lazy factories are removed.
- Direct calls improve stack traces: 2 frames instead of 3 to reach the root cause.
- Static analysis improves: `self._context_manager: ContextManager` (non-nullable after eager init) vs `ContextManager | None` (lazy).
- Ideal engine role: **orchestrator, not coordinator**. Every delegation method removed brings it closer to pure choreography.

### Security (attack surface)

- No new attack surface vs current pattern. Constructor injection makes the existing surface explicit, which is a net positive.
- `after_compaction` hook runs before generation — if a malicious custom ContextManager controls this hook, it can inject messages. Document the security boundary in constructor docstring.
- Replace `assert self.server_manager is not None` (engine.py:557) with explicit `RuntimeError` — assert is stripped by `-O`.
- **Approved** with these two documentation/hardening items.

---

## Recommended Changes

### Remove (4 things)
1. `_ensure_response_generator()` — engine.py:513-530
2. `_ensure_context_manager()` — engine.py:532-544
3. All 6 thin delegation methods — engine.py:282-284, 546-548, 601-611, 655-657
4. `self._response_generator: ResponseGenerator | None = None` and `self._context_manager: ContextManager | None = None` field declarations (replaced with non-nullable types)

### Add (2 things)
1. Eager construction of ResponseGenerator and ContextManager directly in `__init__`
2. Explicit `RuntimeError` to replace `assert self.server_manager is not None` at engine.py:557

### Update (1 thing)
1. Tests that mock `engine._check_compaction` / `engine._inject_context_warning` → mock the subsystem instead: `engine._context_manager = MagicMock()`

### Do NOT add
- Constructor injection params for subsystems (CFQ002 violation)

---

## Net Effect

| Metric | Before | After |
|--------|--------|-------|
| engine.py lines | 716 | ~620 |
| Methods removed | — | 8 (_ensure × 2, thin delegations × 6) |
| Type safety | `ContextManager \| None` | `ContextManager` (non-nullable) |
| Test mock depth | Engine method level | Subsystem level |
| CFQ002 | Pass | Pass (no new params) |
| Stack trace depth | 3 frames | 2 frames |

---

## Consistency Rule (document in code)

```
# Subsystem initialization rule:
# - ResponseGenerator, ContextManager: EAGER (all deps available at __init__)
# - ToolExecutor: LAZY (server_manager is None at init, set async in run())
```

This asymmetry is justified, but must be explicit so future subsystems follow the right pattern.
