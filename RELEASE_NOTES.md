# entropic v2.1.6

Patch release resolving three consumer-reported issues from the v2.1.5
verification pass. The headline item is **gh#33** — a session-killer
bug cluster where one failed delegation poisoned the rest of the
session and the recovery introspection tool crashed the engine. Also
bundled: **gh#31** (configure_dir's project_dir was being silently
ignored) and **gh#32** (two new builtin tools for delegation recall +
resume).

## Highlights

- **gh#33 (CRITICAL):** Three coupled bugs in the sandbox + delegation
  + inspect path. Per-delegation `SandboxManager` lifecycle is fixed
  (now engine-scoped); failed `create_sandbox` produces a typed
  consumer-visible error instead of opaque "(No response from
  delegate)"; `entropic.inspect` no longer crashes on empty/invalid
  args.
- **gh#31:** `AgentEngine::get_repo_dir` was caching CWD on first call
  and silently dropping the `project_dir` passed to
  `entropic_configure_dir`. Consumers whose launcher cwd differed from
  the target repo (the common case for wrapper CLIs and IDE plugins)
  snapshotted the wrong tree into the sandbox.
- **gh#32:** New `entropic.followup` and `entropic.resume_delegation`
  builtin tools give the lead structural recall + resume, instead of
  relying on attention to buried earlier turns. Required closing a
  pre-existing wiring gap: `StorageInterface` was declared on the
  engine but never populated by the facade — `create_delegation` and
  `save_conversation` were dead code throughout 2.1.x.

## Engine bug fixes

- **gh#31** — `36dddd7`: `AgentEngine::set_project_dir` is now called
  from `entropic_configure_dir`; `get_repo_dir` prefers the stored
  value and falls back to CWD only when no `project_dir` was supplied.
- **gh#33 bug 1** — `36dddd7`: `SandboxManager` lifted from
  per-delegation stack-local in `DelegationManager` to a long-lived
  engine member. `DelegationManager` takes a non-owning pointer.
  Eliminates the misleading "Session sandbox cleanup" log that fired
  after every delegation and the wasted re-snapshot of the entire
  project tree on every call.
- **gh#33 bug 2** — `36dddd7`: failed `create_sandbox` now returns
  `(DELEGATION FAILED: session sandbox unavailable)` so consumers can
  distinguish "could not spawn child" from "child ran but produced no
  content". Precondition checks extracted into a helper to keep
  `execute_delegation` under knots SLOC + return-count gates.
- **gh#33 bug 3** — `36dddd7`: `InspectTool::execute` guards the
  non-throwing JSON parse against `is_discarded()` / non-object args.
  A no-arg `entropic.inspect()` falls through to the full-state dump
  rather than throwing `nlohmann::json::type_error.306` and killing
  the engine.

## New features

### gh#32: entropic.followup + entropic.resume_delegation

Two new builtin tools surface delegation history as structurally
authoritative input to the lead, instead of relying on the model
attending to buried earlier turns:

```text
entropic.followup(query, max_results=3)
  → { results: [ { delegation_id, target_tier, summary,
                   completed_at }, ... ] }

entropic.resume_delegation(delegation_id, task, max_turns?)
  → DelegationResult
```

`followup` is read-only — it substring-matches `query` against the
stored delegation result summaries and returns the top-N most recently
completed records. `resume_delegation` loads the prior child
conversation snapshot from storage, seeds a fresh child loop with that
history, and runs the new task on top. The engine resolves the
original `target_tier` from storage so the lead doesn't have to.

Lead identities can teach the model: "before delegating a follow-up,
call `entropic.followup` with the user's question phrase. If a
relevant prior result returns with a citable file:line, answer
directly from it. If the answer is close but incomplete, call
`entropic.resume_delegation` with that delegation_id instead of a cold
re-delegation."

This required closing a pre-existing wiring gap: `AgentEngine` had a
`StorageInterface` member whose function pointers were never populated
by the facade. `create_delegation` and `save_conversation` were dead
code throughout 2.1.x — delegations weren't being persisted at all.
v2.1.6 wires `SqliteStorageBackend` into the engine via a set of
facade-side bridge trampolines so storage records actually accrue.

#### New ABI surface

```c
/* entropic_state_provider_t */
char* (*search_delegations)(const char* query, int max_results,
                            void* user_data);
char* (*load_delegation_conversation)(const char* delegation_id,
                                      void* user_data);

/* StorageInterface (engine-facing) */
bool (*load_delegation_with_messages)(
    const char* delegation_id,
    std::string& result_json,
    void* user_data);
```

`SqliteStorageBackend` gains `get_delegation_by_id` and
`search_delegations` queries. These are additions only — existing
storage callers are unaffected.

## Breaking changes

None. All changes are additive at the ABI surface and the existing
delegation contract is unchanged. Consumers that don't call
`entropic.followup` / `entropic.resume_delegation` and don't depend on
delegation storage records see no behavioral difference apart from the
three bug fixes.

## Distribution

- **CPU tarball:** `entropic-2.1.6-linux-x86_64-cpu.tar.gz`
  (sha256 in companion file)
- **CUDA tarball:** `entropic-2.1.6-linux-x86_64-cuda.tar.gz`
  (sha256 in companion file)
- **Python wrapper:** `pip install entropic-engine==2.1.6` then
  `entropic install-engine` to fetch the matching native engine.

## Known limitations

- `entropic.followup` v1 uses substring matching against stored
  delegation summaries. Semantic recall (small embedding model over
  summary text) is a follow-up — out of scope for 2.1.6.
- Per-conversation scoping of `followup` results is engine-wide; the
  parent-loop `conversation_id` plumbing remains absent in the run
  path. Results may include matches from sibling sessions. A v2.2
  pass will scope to the active conversation.
