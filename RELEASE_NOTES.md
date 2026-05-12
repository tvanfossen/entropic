# entropic v2.1.5

Critical-fix release. The headline item is **gh#29** — a data-corruption
class bug in the delegation isolation path that has been replaced with a
filesystem sandbox living entirely outside the user's project
directory. Five smaller regressions and ABI gaps are also bundled. No
behavioral changes to the engine's generation path — only to delegation
isolation, MCP stdio labeling, Python entrypoint, and the validation
retry surface.

## Highlights

- **gh#29 (CRITICAL):** Replace the git-worktree-based delegation
  manager with a filesystem `SandboxManager` rooted under
  `~/.entropic/sandbox/<session-id>/`. The engine no longer touches
  the user's `.git`, never switches branches, never auto-commits, and
  never merges to `develop`/`main`. Patches are delivered to the
  consumer via a new C ABI callback; the consumer applies them with
  the user's explicit consent (`entropic.helpers.apply_patch` on the
  Python side).
- **gh#30:** Consumer controls for the constitutional validation retry
  flow — disable auto-revision, gate retry/accept/override decisions,
  and read structured rejection metadata.
- **gh#22:** Restore the missing `entropic_context_get` binding plus
  the `HOOK_CALLBACK_CB` / `TOKEN_STREAM_CB` C-API-spelled aliases.
- **gh#20:** Fix two coupled interrupt bugs: the decode loop now
  observes the cancel flag and the engine preserves any content
  already streamed before interruption.
- **gh#19:** MCP stdio child stderr is labeled with the registered
  server name instead of `argv[0]` (no more `[/usr/bin/env]` lines
  injecting Rich BBCode into TUI consumers).
- **gh#18:** Add `python -m entropic` entrypoint.

## Engine bug fixes

- **gh#29** – `816722e` replaced `WorktreeManager` with
  filesystem `SandboxManager`; `c548de4` added the consumer-side
  delegation start/complete callbacks and `entropic.helpers.apply_patch`.
- **gh#20** – `7520a72` made the streaming token callback raise the
  cancel flag (interrupt latency drops from ~60 s to under one token)
  and preserved content past the interruption boundary instead of
  discarding it.
- **gh#19** – `42e4e5b` added `display_name_` to `StdioTransport`
  so the bracketed stderr label reflects the user-facing server name.
- **gh#18** – `42e4e5b` added `python/src/entropic/__main__.py`.
- **gh#22** – `dc9df47` bound the missing C symbol and added the
  CFUNCTYPE aliases.

## New features

### gh#29: filesystem delegation sandbox

The engine now isolates delegations via a filesystem snapshot, not a
git worktree. Layout:

```
~/.entropic/sandbox/<session-id>/
  base/                       Snapshot of the project at session start
                              (honors .gitignore via `git ls-files`
                              when the project is a git repo).
  d-<delegation-id>/          Per-delegation copy of base/ (or of a
                              prior delegation when chaining).
  pending/<delegation-id>.patch
                              Default-deny output for consumers that
                              register no completion callback.
```

The session directory is removed at engine destruction. Stale sessions
left by crashed processes are pruned at startup.

Delegation results are surfaced to consumers via two new C callbacks:

```c
typedef ent_decision_t (*ent_delegation_start_cb)(
    const ent_delegation_request_t* req, void* user_data);
typedef ent_decision_t (*ent_delegation_complete_cb)(
    const ent_delegation_result_t* res, void* user_data);

entropic_error_t entropic_set_delegation_callbacks(
    entropic_handle_t handle,
    ent_delegation_start_cb on_start,
    ent_delegation_complete_cb on_complete,
    void* user_data);
```

- `on_start` runs before the child loop and can return
  `ENT_DECISION_REJECT` to veto the delegation.
- `on_complete` receives the unified-diff patch text plus
  `files_touched[]`. Returning `ACCEPT` lets the engine discard the
  sandbox (consumer has taken responsibility for the patch).
  Returning `REJECT` — or registering no callback at all — causes the
  engine to drop the patch into `pending/<id>.patch` for inspection.

Python helper:

```python
from entropic import (
    EntDecision, EntDelegationResult, DELEGATION_COMPLETE_CB,
    entropic_set_delegation_callbacks,
)
from entropic.helpers import apply_patch

def on_complete(res_ptr, ud):
    res = res_ptr.contents
    patch = bytes(res.patch[:res.patch_len]).decode()
    if user_accepts_patch(patch):
        apply_patch(repo_path, patch)
        return EntDecision.ACCEPT
    return EntDecision.REJECT
```

### gh#30: validation retry controls

Four new ABI surfaces replace consumer-side workarounds around the
constitutional revision loop:

```c
entropic_error_t entropic_validation_set_auto_retry(handle, enabled);
entropic_error_t entropic_validation_resume_retry(handle);
entropic_error_t entropic_validation_accept_last(handle);
entropic_error_t entropic_set_attempt_boundary_cb(handle, cb, ud);
```

With auto-retry disabled, the engine stops after the first failing
critique and surfaces verdict `paused_pending_consumer` via
`entropic_validation_last_result()`. The consumer decides whether to
resume revision, accept the rejected attempt as-is
(`passed_consumer_override`), or restart the turn. The
`entropic_validation_last_result()` JSON now includes `attempt_n` and
per-violation `rule_id` / `rule_text` / `quote` / `severity` aliases for
structured rendering.

## Breaking changes

**Delegation auto-merge is gone.** Pre-2.1.5 the engine silently merged
delegation output into the user's `develop` branch on success. That is
the bug fixed by gh#29. Consumers that relied on the implicit merge
must now register `ent_delegation_complete_cb` (or its Python
equivalent) and call `apply_patch` (or their own logic) to materialize
the patch with the user's consent.

Single known impacted consumer: `bissell-llm-studio`. Migration is
mechanical — read the patch from the result struct, prompt the user,
hand the patch to `entropic.helpers.apply_patch`.

## Cleanup for repos already affected by gh#29

If an engine running v2.1.4 or earlier touched a repo, you may still
have stale worktrees and an engine-created `develop` branch. Manual
cleanup (no auto-cleanup — risk of false-positive on a legitimate
`develop`):

```bash
git worktree remove --force .worktrees/<hash>/delegation-*
git branch -D develop 2>/dev/null   # only if you didn't create it
for b in $(git branch --list 'delegation/*' | tr -d ' *+'); do
    git branch -D "$b"
done
rm -rf .worktrees/
```

The 2.1.5 engine will not recreate any of these.

## Distribution

- **CPU tarball:** `entropic-2.1.5-linux-x86_64-cpu.tar.gz`
  (sha256 in companion file)
- **CUDA tarball:** `entropic-2.1.5-linux-x86_64-cuda.tar.gz`
  (sha256 in companion file)
- **Python wrapper:** `pip install entropic-engine==2.1.5` then
  `entropic install-engine` to fetch the matching native engine.

## Known limitations

- gh#17 (adapter coverage expansion for Gemma 4 / Qwen 3.6 / Nemotron /
  Falcon) is intentionally not in 2.1.5 — see the issue thread for the
  scope.
- The validation `severity` field is hard-coded to `"error"` in this
  release; the soft-warning track will land alongside the next
  validator schema bump.
