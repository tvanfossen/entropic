_Last 10 releases. Older history: [OLD_NOTES.md](OLD_NOTES.md). Kept short
because `gh release create --notes-file` hits GitHub's 125,000-char release
body limit once this file accumulates full project history — see v2.9.3._

# entropic v2.13.1

Patch release — **`gpu_layers: auto` now reads the model instead of assuming
it.**

`auto` exists so that fitting a new model is a decision the engine makes
rather than a number an operator tunes. v2.13.0 shipped it making that
decision from almost no information: it priced every model as thirty layers,
charged `file_bytes / layers` per layer, and reserved a flat 2 GiB for
everything that was not weights. It knew nothing about the KV cache, the
draft head, or experts.

## Read this first — behaviour that changes without you asking

- **`gpu_layers: auto` will choose differently, and usually better.** It now
  reads the GGUF's real shape and solves the same footprint estimator the
  admission gate uses. Measured cases: the 26B-A4B at IQ2 goes from 26 of 30
  layers to fully resident — a configuration this project measured running at
  30 of 30 with headroom to spare — and a 42-layer model is no longer priced
  as if it had 30, which had it overstating per-layer cost by 40% and
  clipping a model that fits.
- **`auto` now derives an expert split for a MoE.** When everything does not
  fit, it moves experts host-side while keeping **all layers** on the card,
  and only drops layers when that is not enough. Moving a layer off takes its
  attention with it; v2.13.0 measured expert offload at 22.62 tok/s against
  18.86 for whole-layer offload of the same model.
- **`cpu_moe_layers` with `gpu_layers: auto` is still refused**, for a
  different reason. It used to be "auto cannot model expert placement". It
  can now, so the honest refusal is that setting both asks two things to
  decide one placement. Drop `cpu_moe_layers` and let auto choose, or set
  `gpu_layers` explicitly and keep your own split.
- **The budget gate now enforces on partially-offloaded tiers.** The
  estimator previously returned "unknown" for any partial offload, which left
  the gate open entirely (gh#142). With the real layer count it prices them
  exactly, so a configuration that cannot fit is refused rather than
  discovered as a failed allocation.

## Engine fixes

- The draft / MTP head is counted. It was priced nowhere — small in absolute
  terms (225 MiB for `mtp_a4b`), decisive on an 11 GiB card against a 9.3 GiB
  trunk.
- Compute-buffer headroom is held back by `auto`, scaled by `n_ubatch`
  because that is what the cost tracks, and counted only as the **excess**
  over `vram_reserve_mb` — whose documented purpose is covering exactly this.
  Charging both would have been the same double-count this release removes.
- Full residency is reported as the `-1` sentinel rather than a layer count
  equal to the model's. That sentinel is what `mlock_refused` exempts and
  what the estimator's full-vs-partial branch keys on, so a concrete count
  would have stripped the mlock exemption from working configurations and
  priced a fully-resident model through the partial path — omitting the
  embeddings and output head, an under-count.
- A model whose metadata cannot be read falls back to the previous
  weights-only estimate, so it behaves exactly as it did in v2.13.0.

## Notes

- `gpu_layers: "auto"` quoted — as JSON serialisation emits it — parses
  identically to the bare word. Now asserted rather than assumed.
- The `mlock` refusal is floor-gated at **10 GiB**: larger than that **and**
  larger than `RLIMIT_MEMLOCK` **and** not fully offloaded. `use_mlock`
  defaults to true, so an unfloored rule would refuse ordinary models.

## Distribution

- CPU tarball: `entropic-2.13.1-linux-x86_64-cpu.tar.gz` (sha256 in companion file)
- CUDA tarball: `entropic-2.13.1-linux-x86_64-cuda.tar.gz` (sha256 in companion file)
- Python wrapper: `pip install entropic-engine==2.13.1` then `entropic install-engine`

# entropic v2.13.0

Minor release — **nineteen issues, a llama.cpp bump, and the gates that found
the rest.**

The theme is contracts that were written down and never in force. A delegation
sandbox with no caller. A `max_turns` argument advertised to the model that
bound nothing. A bash timeout stored, logged and never applied. A grammar stem
that failed open. In each case the fix is the missing enforcement, not a
softened claim — which is why a minor release carries this many behaviour
changes a consumer meets without editing a single config key. Those lead.

## Read this first — behaviour that changes without you asking

Every item here changes what an unmodified consumer sees. Nothing in this
section requires a config edit to reach you.

- **A tool call that fails three times with identical arguments is now
  refused before dispatch.** Failures are counted per exact call (tool name +
  sorted arguments); the third identical attempt against an identical error
  comes back as a `rejected_anti_spiral` result carrying corrective text
  instead of running the tool again. A success clears that call's history, and
  a call that fails *once* is still retried — the transient-retry behaviour is
  unchanged. This reuses the existing `rejected_anti_spiral` kind rather than
  adding a tenth `result_kind`, so the nine strings consumers parse are
  untouched. Tune with `max_identical_failures` (default 3). Found by this
  release's own gate, where a delegated child re-issued one refused read four
  times and the test died on its timeout. (gh#160 fallout)
- **`concurrent_sessions` now defaults to `true`.** A run is guarded per
  session key, not per handle. Two callers on two sessions no longer serialize
  behind one another, and `ENTROPIC_ERROR_ALREADY_RUNNING` now means "this
  session is busy" rather than "this process is busy". Set
  `concurrent_sessions: false` to restore v2.12.x semantics exactly. (gh#158)
- **`delegation.isolation` is new and defaults to `none`, and under `none` no
  sandbox is constructed at all.** That is what every release since v2.1.5
  actually did — the swap callback had no caller, so children always ran
  against the real working directory. What went with it was a snapshot of the
  project tree diffed against itself, whose only product was a **0-byte patch**.
  A consumer whose `on_complete` fires today **stops seeing it**. Set
  `delegation.isolation: sandbox` to get the containment the header always
  described: child writes land in the sandbox and come back as a real patch.
  (gh#160)
- **`mcp.filesystem.allow_outside_root` is now tri-state — `true | false |
  optional` — and defaults to `optional`.** An access outside the root is
  routed to a new path-approval callback; **with no approver registered it is
  REFUSED**. The bundled default was previously `true`, which meant unconfined
  read *and* write. Register a callback with
  `entropic_set_path_approval_callback`, or set the key explicitly to `true`
  for the old behaviour. `outside_root_allow` / `outside_root_deny` path lists
  short-circuit the prompt in either direction; deny always wins.
- **A tier `grammar:` stem that does not resolve now fails at first use** with
  a typed error, instead of decoding unconstrained. A tier whose grammar file
  was missing or misnamed has been silently generating free text; it now says
  so. (gh#154)
- **A prompt that cannot fit its tier's context is refused** with
  `ENTROPIC_ERROR_EVAL_CONTEXT_FULL` instead of degrading silently. This
  release's own gate tripped it: 27 staged tools against a
  `context_length: 4096` tier.
- **`entropic.delegate`'s `max_turns` now bounds the child.** It was advertised
  to the model, written to the storage row, and read by nothing. A lead that
  already passes small values will now get correspondingly short children. When
  the model's value meets the operator's limit, the **stricter** wins — a model
  cannot raise a configured ceiling. (gh#182)
- **Identity frontmatter overrides now apply to top-level runs.**
  `max_iterations` and `max_tool_calls_per_turn` in a tier's identity
  frontmatter were honoured only for delegated children. They now bind a
  top-level run too, **when the tier is named**. A routed or default-tier lead
  still does not pick them up — the tier is chosen after the loop preamble;
  tracked as gh#185. (gh#183)
- **`entropic_state_save` / `_state_load` / `_get_logprobs` /
  `_compute_perplexity` / `_adapter_load` now lazy-load the model** through the
  residency gate instead of failing with "model not active" — and because a
  load must not land mid-generation, each claims the handle's turn first and
  can therefore return `ENTROPIC_ERROR_ALREADY_RUNNING` where it previously
  could not. (gh#157)
- **The bash tool enforces its timeout.** `mcp.bash.timeout_seconds` (new,
  default 30) is a wall-clock limit that now actually fires and kills the
  process group, where the old value was stored, logged and ignored. The tool
  header no longer claims to block dangerous commands, because it never did.
- **`include/entropic/core/engine_types.h` lost two struct members** —
  `ChildContextInfo::tools` and `LoopContext::all_tools`, both written and
  never read. Source-breaking **only** for a C++ consumer that constructed or
  touched those structs directly. The `.so` contract is pure C and is
  unaffected; `ENTROPIC_API_VERSION` is unchanged.

## Highlights

- **One handle, many repositories.** Named workspaces with per-workspace tool
  roots and per-workspace server instances, bound per session. (gh#166)
- **Concurrency that is deliberate rather than accidental.** Per-session run
  guard, serialized decode, per-session interrupt, and an audit of every piece
  of per-handle state that a second session could reach. (gh#158)
- **Delegation isolation that is actually wired**, with unique sibling ids and
  a restore target that is the parent's active root rather than the repo root.
  (gh#160)
- **A structured channel for parent context.** Explicit context seeds,
  `requires_context` tiers, and resume-by-target, so a child stops re-deriving
  facts the lead already holds. (gh#162)
- **VRAM you can hand back.** Deferred load, explicit release, and a load path
  that goes through the residency gate exactly once. (gh#157, gh#164, gh#148)
- **Outside-root file access asks**, and the answer can be written down.
- **llama.cpp b9886 → b11009** — `load_mode`, penalties `n_vocab`, speculative
  pos0.

## New C API

All additive; `ENTROPIC_API_VERSION` is unchanged.

- `entropic_release_model` — evict weights from VRAM without destroying the
  handle, keeping adapter registrations and sessions alive. (gh#164)
- `entropic_session_context_set` — the counterpart gh#144 shipped without:
  restore a session's conversation with lossless message serialization.
  (gh#165)
- `entropic_workspace_create`, `entropic_session_bind_workspace` — a named
  workspace with its own root and its own server instances. (gh#166)
- `entropic_interrupt_session` — interrupt one session rather than the handle.
  (gh#158)
- `entropic_set_path_approval_callback`, plus `ent_path_access_t` and
  `ent_path_approval_request_t` — the approver that `allow_outside_root:
  optional` consults.
- New error code `ENTROPIC_ERROR_MLOCK_LIMIT_EXCEEDED`: `use_mlock` that would
  pin more than `RLIMIT_MEMLOCK` allows, for a model too large for the card, is
  now refused instead of pinning pages nothing can reclaim. (gh#148)

## New configuration

| Key | Default | Notes |
|---|---|---|
| `models.defer_load` | `false` | Load the default tier on first use, not at `entropic_configure`. Opt-in, so first-token latency is unchanged unless you ask. (gh#157) |
| `models.tiers.<name>.cpu_moe_layers` | `0` (off) | **EXPERIMENTAL.** Keep a MoE layer's expert tensors on the CPU while attention and KV stay resident. Absent the key, no override array is built at all. (gh#153, gh#180) |
| `models.tiers.<name>.requires_context` | `false` | The tier refuses to run without an explicit context seed. (gh#162) |
| `delegation.isolation` | `none` | `none` \| `sandbox`. See above. (gh#160) |
| `mcp.filesystem.max_walk_entries` | `250000` | Explicit bound on the gitignore walk. (gh#161) |
| `mcp.filesystem.outside_root_allow` / `outside_root_deny` | empty | Path lists that short-circuit the approver. Deny beats allow. |
| `mcp.bash.timeout_seconds` | `30` | Now enforced. |

## Engine bug fixes

- **gh#148** — WARM state mapped the entire GGUF into host RAM regardless of
  `gpu_layers`, so the ACTIVE reload paid for the file twice. The model now
  loads **once, directly into its target residency**, and a `use_mlock` /
  `gpu_layers` combination that cannot be satisfied is refused with a typed
  error rather than pinning unreclaimable pages.
- **gh#149** — model-test skips stated a false reason (a missing GGUF) when the
  large-model gate fired. Every skip now carries its real reason. This release
  has **zero skips**; that is the point of having fixed the message.
- **gh#154** — a consumer could not ask whether a run actually decoded under a
  grammar. Per-generation metric records now carry the resolved grammar source,
  an unresolvable tier stem fails configure, and a stem that resolves to
  nothing fails loud at first use.
- **gh#156** — an unreadable prompt path is rejected at configure time, before
  the weights are loaded, rather than after the expensive half of startup has
  already been paid for.
- **gh#157** — `keep_warm: false` did not stop the default tier loading at
  startup. `models.defer_load` is the key that does, and fixing it exposed that
  `activate_default_tier` called the backend directly: the one load every
  consumer performs bypassed the residency gate, fired no `Loaded` event and
  recorded no footprint. Both paths now go through `get_model`. Adapter preload
  moved to the activation of the owning model (it previously skipped silently
  for every tier on a different GGUF and was never retried), and
  `entropic_model_has_vision` answers from config, because an unloaded tier
  answered a confident, wrong `0`.
- **gh#159** — a bare tool-call turn on a tier with `enable_thinking: false`
  was logged as an unterminated reasoning block. It is not a fault.
- **gh#161** — `filesystem.glob` took 87 s on a repo with vendored deps:
  `is_ignored` ran two regex per rule per path across 5061 rules. The matcher
  is pruned and the walk is explicitly bounded.
- **gh#163** — investigated, **no defect reproduced.** The reported symptom — a
  configured `app_context` path reported as "not configured" — did not occur at
  any layer: all three configure doors, global-only, global plus a project layer
  that never mentions the key, the v2.11.1 empty-tiers transplant, and both
  layers naming a path. The evidence tests shipped regardless, and the log line
  that made the report ambiguous now says which of three states the key is in
  (set, explicitly disabled, absent) rather than collapsing them.
- **gh#168** — `PRE_TOOL_CALL`'s `modified_json` was freed and discarded. It is
  the one hook path that advertised a modification channel and dropped it; the
  guard on applying it is pointer identity, not content.
- **gh#169 / gh#181** — a delegation that hit the iteration cap, or the
  thinking-budget hard cut, returned a **placeholder** where the child's real
  result belonged. Both terminals now *annotate* the run's last real output
  through the same helper, so a parent gets the work its child actually did.

## Defects this release's own gates found

None of these were reported. Each was found by a test written for something
else, which is the argument for the gates.

- **`entropic_context_usage` could abort the host process** on an unkeyed
  context read under concurrent sessions.
- **A lead plus exactly ONE worker had shipped with no delegation tools since
  v2.0.4.** `register_delegation_tools` skipped `delegate`, `pipeline` and
  `resume_delegation` when it was handed one tier — a guard written at v1.8.5,
  when the argument was every configured tier. v2.0.4 changed the argument to
  the delegation *targets*, with the source tier already removed, and the guard
  was never re-read against its new meaning. The canonical two-tier deployment
  therefore collected one target, tripped the guard, and put no delegation
  tools on the model's menu. Nothing said so — an absent tool is
  indistinguishable from a model that declined to call one. (gh#160, gh#162)
- **One workspace could read another workspace's files.** A workspace now
  confines its tools to its own root whatever the host allows. (gh#166)
- **`TodoTool::items_` corrupted the heap under concurrent sessions.** The
  read-before-write tracker and the todo list belong to the session, not to the
  server. (gh#158)
- **Four model tests were passing while proving nothing** — asserting on the
  transcript that contained the question rather than on the assistant's answer,
  or never reaching the code they claimed to test.

## Measured (gh#153)

A four-arm matrix with a same-config floor, because an A/B without a control
measures the harness as much as the subject.

| Configuration | Result |
|---|---|
| MTP, fully resident E4B QAT | **+42.8 % / +46.7 %** |
| MTP, partially resident 26B-A4B (18 of 30 layers) | **~0 %** — −0.92 % and +0.82 % across two runs, despite a *higher* accept rate |
| Expert offload (26B-A4B) | 18.2 tok/s, rising to 22.6 tok/s once the freed VRAM holds the experts again |
| MTP on top of expert offload | **+4.16 %** |
| MTP, **fully resident** 26B-A4B at IQ2 (30 of 30 layers) | **+16.33 %** — 52.46 → 61.05 tok/s against a 0.02 % floor |

The middle rows are the finding. Accept rate is not the predictor once the
model is not fully resident — a drafted token still costs a host-to-device
round trip for the layers that live in RAM. Expert offload matters here because
freeing VRAM is what lets the card hold experts again, and only then does MTP
pay at all. The performance assertion in the suite is now gated on **resident
fraction**, not on which test is running.

The last row is what that rule predicts, tested. The *same* 26B-A4B, quantized
until it fits an 11 GiB card instead of being placed cleverly across one, runs
**52.46 tok/s against 18.86** — 2.8× — and MTP goes from buying nothing to
**+16.33 %**, clearing the strict fully-resident bar of 5.02 %. On this class
of hardware, making the model fit beats placing it well, and it is not close.
A prior arithmetic estimate of 70–145 tok/s for this configuration was
optimistic by 1.3–2.8×; the direction held, the magnitude did not.

## Verification

The full model gate ran on `f64dfb2`, the head of this release, with every
engine change in it:

- **Model suite: 86 passed, 1 failed, 0 skipped, 2 flaky**, on a GTX 1080 Ti.
  Attached as `model-results-v2.13.0.json`, which from this release also
  records `built_version` — what the tested build actually carried, as
  distinct from what the tree claimed. All three of `version`,
  `built_version` and `git_sha` agree on this run.
  - `test-outside-root-approval` failed one assertion of 15: the lead never
    issued the second of two requested reads, so the path approver was never
    consulted about it. Everything the test exists to prove is verified by
    the other 14 — the approver is asked, one path is served and one refused,
    and the refused file's contents never reach the model. This is the same
    known-marginal class as the recall assertions below: an E2B-class lead
    complying with a tool-call instruction, not engine behaviour.
  - Two earlier failures are **fixed** by this release and pass here.
    `test-e7-delegation` previously timed out on all three attempts (~120 s
    each); with the repeated-failure guard it passes on the first attempt in
    86 s. `test-gh165-restore` passes with no retry.
- **A note on those marginal assertions.** Several model tests assert that
  the lead reproduces a specific word — a remembered codeword, a delegated
  anomaly code. Some of those only became real assertions in this release: a
  test helper used to return the whole conversation, so an assertion could
  match the test's own seed and pass while the model said nothing at all.
  Now that they test what they claim to, a small lead satisfies them roughly
  two times in three. Reported rather than tuned away.
- **CPU suite: 1925/1925.** Pre-commit clean.
- **ThreadSanitizer: 36/36 with zero warnings** over the gh#158 / gh#160 /
  gh#166 concurrency scenarios — the suite that found the `TodoTool` heap
  corruption.
- Benchmarks `[mtp-e4b]`, `[mtp-a4b]` and `[mtp-a4b-experts]` all pass.

## Distribution

- CPU tarball: `entropic-2.13.0-linux-x86_64-cpu.tar.gz` (sha256 in companion file)
- CUDA tarball: `entropic-2.13.0-linux-x86_64-cuda.tar.gz` (sha256 in companion file)
- Python wrapper: `pip install entropic-engine==2.13.0` then `entropic install-engine`
- Model-test audit record: `model-results-v2.13.0.json`

## Known limitations

- **`[mtp-a4b-iq2]` runs only as the sole arm in its process.** It now runs and
  reports (see Measured, above): fully resident, 52.46 → 61.05 tok/s, +16.33 %.
  What does not fit an 11 GiB card is *several* arms in one process — a later
  arm building its context on top of an earlier one fails to allocate the
  compute pp buffers. Invoke it anchored, `--filter
  "gh108-config-benchmark-mtp-a4b-iq2"`, and it fits — but not comfortably.
  Measured: trunk 9535.79 MiB, MTP head 225.21 MiB, KV 115.31 MiB, compute
  buffers up to 520.61 MiB, against ~11162 MiB usable. The log reads **712 MiB
  free at the moment the head loads**, leaving on the order of 500 MiB once it
  is resident. That is the budget, not room to grow into.
  Making the four-arm harness release between arms is tracked by gh#167 and
  gh#180. Nothing else in the matrix depends on it.
- **Expert-tensor offload is a prototype, default off, and unmeasured by the
  admission math.** `estimate_footprint_bytes` and `gpu_layers: auto` still
  price whole layers, so a prototype user states `gpu_layers` explicitly and
  the `auto` combination is refused rather than approximated. Whether it ships
  as a supported feature is gh#180's to decide.
- **A routed or default-tier lead still does not pick up its identity's
  `max_iterations` / `max_tool_calls_per_turn`** — the tier is chosen after the
  loop preamble. Name the tier explicitly, or track gh#185.
- **`delegation.isolation: sandbox` serializes sandboxed delegations on a
  handle.** `set_working_dir` is one field per in-process server, so two
  sandboxed children would otherwise interleave their swaps. gh#166's
  per-workspace server instances are what give the parallelism back;
  unsandboxed delegations are untouched.
- **An external stdio/SSE MCP server cannot be contained.** It is a separate
  process with its own cwd. A sandboxed child that can reach one whose
  `tools/list` does not assert `readOnlyHint: true` is **rejected** with a
  typed message naming the tool, rather than being given a containment
  guarantee that is quietly false.

Issues closed: gh#148, #149, #153, #154, #156, #157, #158, #159, #160, #161,
#162, #164, #165, #166, #168, #169, #181, #182, #183. gh#163 was investigated
and produced no defect; its evidence tests shipped.

# entropic v2.12.2

Patch release — **an MTP-vs-plain comparison can now be read off the logs**, and
the prompt log stops re-emitting itself.

Both fixes are observability. Neither changes what the engine computes or what
the model sees. Both were found by a consumer whose measurements went wrong in
ways that pointed at their own code first.

## gh#151 — the two decode paths reported throughput differently

The plain paths logged:

```
Generated: 87 tokens, finish=stop, 3040ms, 28.5 tok/s
```

The speculative path logged a differently shaped line carrying no throughput,
so an MTP-vs-plain A/B measured from logs could read tok/s on one arm and not
on the other — the single comparison speculative decoding exists to be judged
by. The consumer resorted to **estimating** the MTP arm's output volume from
drafted/accepted arithmetic, and had to publish their result as an upper bound
because of it.

**The field itself was never broken.** `spec_finalize` has populated
`throughput_tok_s` since gh#108, and `state.n_generated` carried the true
generated-token count all along — this issue was originally filed here claiming
the field read zero, and that claim was wrong. What was missing was the *line*.

Both paths now format through one shared `format_generation_summary`, because
two hand-rolled lines drifting apart is the defect being fixed; a second one
would reintroduce it the first time either changed. The plain line is
byte-identical to what it has always been, and speculative runs append
`drafted=…, accepted=…, accept_rate=…` rather than substituting a different
shape. A plain decode prints no speculative clause at all, so "did speculation
run" stays answerable from the log.

The original `Speculative:` line is kept, not replaced — it is an existing log
contract, at least one consumer parses these logs, and it carries the same
numbers.

## gh#152 — the prompt log re-emitted the whole conversation every turn

`log_prompt` wrote every message on every turn, and the message list grows
monotonically, so a body emitted at turn 5 was re-logged by turns 6, 7, 8 …
Anyone counting model output by grepping the log over-counted by roughly the
number of remaining turns.

The consumer counted finding-shaped lines across two A/B arms, got **147 against
380**, and read it as one arm looping 2.6x more — a serious defect in their own
code, which is where they went looking. It was re-emission: 36 and 38
`End prompt` markers against 36 and 38 generation turns. Their framing is the
right one: **the log is not a transcript, and it reads like one.**

The rule already existed for the system message, which has been hashed and
elided when unchanged since v2.0.6. It was applied to the one message known to
be large and invariant, and never to the ones that *accumulate* — which are
exactly the ones a reader is trying to count. Each distinct body is now logged
in full once and referenced thereafter by index, role, size and hash, so the
sequence is still reconstructable.

`ENTROPIC_LOG_FULL_PROMPT=1` restores the unabridged dump verbatim. The full
prompt is what you want when diagnosing what the model actually saw, so eliding
it is not the only option.

## Verification

- 10 new scenarios across two pure headers (`generation_summary.h`,
  `prompt_log_util.h`), CPU-testable with no model and no GPU.
- CPU suite green; pre-commit clean.
- No model-suite re-run: no inference path changed. The v2.12.0 gate
  (76 passed, 0 failed, 3 skipped) stands.

## Known limitations

- The hybrid Qwen family remains outside the model gate on this host; see the
  v2.12.0 notes and gh#148. Unchanged by this release.
- Elision is per-tier and keyed by content hash, so two genuinely identical
  bodies in one conversation render the second as a reference. The reference
  line names the index, so the sequence is unambiguous.

# entropic v2.12.1

Patch release — **one interrupt no longer permanently disables every external
MCP server.**

## The bug (gh#150)

`StdioTransport::cancel_flag_` was a latch, not a flag. `store(true)` appeared
exactly once in the tree and nothing anywhere stored false:

```
AgentEngine::interrupt()       -> external_interrupt_cb_
                               -> ServerManager::interrupt_external_tools
                               -> StdioTransport::interrupt()
                               -> cancel_flag_.store(true)     never cleared

AgentEngine::reset_interrupt() -> interrupt_flag_.store(false)
                                  clears the ENGINE flag; transports untold
```

So the first interrupt — a Ctrl+C, or a bridge client disconnecting — took
every external MCP server away for the lifetime of the process. `open()` did
not help: it early-returns when already connected, and cleared nothing on any
path.

**Reported by a consumer hosting one engine for several clients over the
external bridge**, where clients connecting and disconnecting is not an edge
case, it is the normal operating mode. One disconnect disabled external tools
for every other session on that host.

This is **not a v2.12.0 regression** — `cancel_flag_` and the interrupt
propagation both landed 2026-04-23 in the 2.0.6-rc16 P1 batch, so the defect
has been latent for roughly four months. What changed is reachability: v2.12.0
exists to make hosted multi-client operation the normal case, which is exactly
the pattern that trips it.

## Fixes

- **The interrupt is scoped to a run again.** `Transport::clear_interrupt()`
  (virtual, default no-op, symmetric with the existing `interrupt()`), a
  `StdioTransport` override, `ServerManager::clear_external_tool_interrupts()`,
  and `AgentEngine::reset_interrupt()` now driving it through a registered
  callback. The facade wires the release alongside the abort, so no future
  call site can get one without the other.
- **`open()` now does what its doc always claimed.** The comment on
  `interrupt()` said the flag was "cleared implicitly by a successful open()".
  It was not, and nothing else cleared it either. `open()` clears first, on
  both paths — the early return when already connected is precisely why a
  later placement would have left a live-but-latched transport stuck. The
  false comment is called out rather than quietly corrected: it sent the
  reporter looking in the wrong subsystem first.
- **A dropped call is now reported as a failure.** `Transport::is_interrupted()`
  makes the state answerable at all, and the two failure envelopes in
  `ExternalMCPClient::execute` now lead with `Error:`. That prefix is
  load-bearing: `classify_tool_result` routes on the leading text, so a call
  that failed was logged `status=ok` and counted as a success. The consumer
  watched every external call fail while the logs said everything was fine.
- **The 0 ms timeout is explained.** `"timed out or transport error"` covered
  four distinct conditions, and the one that returns instantly — a transport
  suppressed by an interrupt — read as a timeout that took no time. The
  interrupted case now says so in its own words.

## Verification

- RED first: the engine-level test fails on unfixed code at the exact
  assertion that matters — the transport latch is still set after
  `reset_interrupt()`.
- 4 new scenarios; CPU suite 1748/1748; pre-commit clean.
- No model-suite re-run: this release changes no inference path. The v2.12.0
  gate (76 passed, 0 failed, 3 skipped) stands.

## Known limitations

- The hybrid Qwen family remains outside the model gate on this host; see the
  v2.12.0 notes and gh#148. Unchanged by this release.
- `SSETransport` does not latch and so needs no release, but it has no
  interrupt support at all — an interrupt does not abort an in-flight SSE
  request. Pre-existing, and out of scope here.

# entropic v2.12.0

Minor release — **one hosted engine can now serve several callers without them
seeing each other**, plus a tool-call crash that killed whole runs.

All three issues came from one consumer building the thing the bridge always
documented as its purpose: a host that keeps one model resident and serves
several MCP clients. Two of the three were caused by contracts our own headers
stated and nothing implemented.

## Highlights

- **Per-caller sessions.** A `session` key on the bridge's ask tool, and an
  `entropic_*_session` C API family, give each caller its own conversation,
  context, system-prompt seed and KV sequence.
- **Concurrent runs are no longer undefined behaviour.** A second run on a
  handle is refused with `ENTROPIC_ERROR_ALREADY_RUNNING`; the bridge queues
  callers FIFO instead of racing them.
- **An argument-free tool call no longer kills the run.**
- **Consumers name their own tools.** `tool_prefix`, `server_name` and
  per-tool description overrides.
- **MTP stops discarding a prefix it can prove unchanged.** Speculative
  decoding and prefill reuse now compose instead of being mutually exclusive.

## Engine bug fixes

- **gh#143** — a tool call carrying no arguments serialised to the string
  `"null"`; every built-in server then threw `type_error.306` out of dispatch
  and aborted the turn. `git.diff` with no arguments is a legitimate shape, so
  this was reachable from any model on any turn. Fixed at the origin, plus a
  dispatch-level exception barrier that turns any throwing tool into a tool
  error the model can correct — the in-process counterpart of the guarantee
  gh#133 gave plugin servers. `ContextInspectTool` was independently
  unguarded and is fixed too.
- **gh#144 (data race)** — `@threadsafety Serialized per-handle.` on the six
  run entry points has been false since gh#109 removed `api_mutex` from them.
  The bridge serves each client on its own thread, so two concurrent asks
  raced the shared conversation and decoded concurrently on one
  `llama_context`. The doc is corrected and the documented error code is now
  actually returned.
- Two missing `handle->engine` null checks in `entropic_context_get` and
  `entropic_context_count`, which their siblings already had.
- The `try_warm_reuse` comment claiming an interleaved conversation "falls
  back" was wrong — neither stated branch fires. Corrected, and the underlying
  hazard fixed by per-sequence residency.

## New features

- **gh#144 — keyed conversations.** `entropic_run_session`,
  `_run_session_as`, `_run_session_streaming`, `entropic_session_context_get`
  / `_count` / `_clear`, `entropic_session_drop`, `entropic_session_list`.
  A NULL or empty key means the default session, so every existing caller is
  unaffected. The key is opaque: two callers passing the same string share a
  conversation deliberately.
- **gh#144 — session pool.** `max_sessions` on a tier derives the whole KV
  geometry (`n_seq_max`, `kv_unified`, `n_ctx`) rather than exposing three
  knobs that can disagree. `context_length` is PER SESSION.
- **gh#144 — occupancy.** The status tool reports `busy`, `queue_depth` and
  the session table, so a queued caller is distinguishable from a hung one.
- **gh#145 — consumer identity.** `mcp.external.tool_prefix`,
  `server_name` and `tool_descriptions`. All default to today's values.
- **gh#144 — MTP prefix retention.** `mtp_init_run` cleared the whole
  context and fully re-prefilled on EVERY generation, so under
  `speculative.mtp` warm-keep and the prompt cache never executed at all.
  Nothing about speculative decoding requires discarding the cache; that
  was an implementation choice in this path. Measured over four turns of a
  growing conversation, prefill went from 52 -> 116 -> 180 -> 244 tokens
  (linear in history) to a constant per-turn delta.

  **Corrected after publication.** This entry originally carried a
  consumer's prefill ratio and an MTP-off comparison. The consumer has
  withdrawn those figures: every external tool call in both arms of their
  measurement was silently failing (gh#150), so both arms measured a
  degraded fallback path rather than the feature. What they have confirmed
  instead, and what stands: prefix retention engaged on **19 of 19 turns**
  with MTP active, against **0 events on v2.11.1**. The engine-side
  measurement above is our own instrumented figure and is unaffected. A
  real latency number will follow once gh#150 is in their hands.
- `GenerationResult::prefill_tokens` reports what a run actually decoded.
  The MTP path counted nothing, so there was no way to tell reuse from
  re-decode.

## Breaking changes

None to the C ABI. Every addition is a new named function, so
`ENTROPIC_API_VERSION` is unchanged.

One behaviour change consumers will notice: **a concurrent run on one handle
now returns `ENTROPIC_ERROR_ALREADY_RUNNING` instead of racing.** Callers that
were relying on the stale "serialized per-handle" doc were getting undefined
behaviour, not queueing. A host that wants callers to WAIT should serialize
above the C API, as the external bridge now does.

`include/entropic/types/config.h` gains members on `ExternalMCPConfig` and
`ModelConfig`. The C ABI is opaque-handle based, so this affects only C++
consumers that construct `ParsedConfig` directly and must recompile.

## Distribution

- CPU tarball: `entropic-2.12.0-linux-x86_64-cpu.tar.gz` (sha256 in companion file)
- CUDA tarball: `entropic-2.12.0-linux-x86_64-cuda.tar.gz` (sha256 in companion file)
- Python wrapper: `pip install entropic-engine==2.12.0` then `entropic install-engine`

## Known limitations

- **The hybrid Qwen family is not covered by this release's model gate.** The
  full 79-test roster completed: **76 passed, 0 failed, 3 skipped, 1 flaky**
  (`test-gh106-mtp-route-c1`, passed on retry), on a GTX 1080 Ti. All three
  skips are the same model — `qwen3_6_a3b` — and all three were skipped by an
  explicit operator allowance (`ENTROPIC_SKIP_LARGE_MODEL_TESTS=1`), not by a
  missing file and not by a defect in the code under test:
  `test-v219-qwen36`, `test-gh87-verify-qwen36`, `test-gh103-sequential-stop`.

  The underlying constraint (gh#148): the engine's WARM state maps the ENTIRE
  GGUF into host RAM regardless of `gpu_layers` — 12952 MiB measured for a
  13.6 GB file — and only the ACTIVE reload honours the offload split, so peak
  host usage is the whole file. This is not a size rule: the dense 13.6 GB
  gemma-4-26B passes where the smaller 12.6 GB hybrid Qwen fails, because the
  recurrent state is additional. Two related defects WERE fixed this release
  (mlock pinning a model too large to pin, and an offload split frozen at a
  stale VRAM measurement); the WARM-load behaviour itself is untouched.

  **What this means for the standing hybrid-arch rule.** KV-touching changes
  are supposed to be exercised on a hybrid/recurrent architecture, not only on
  plain-KV gemma4 — and this release touches KV heavily. That coverage is
  absent here and the risk is not hypothetical. It is stated rather than
  papered over.

  The allowance was made explicit precisely so this is reproducible: the
  previous gate was a MemAvailable estimate whose own documentation calls it
  best-effort, so the same commit skipped or did not depending on page-cache
  state. The allowance has a 10 GiB floor inside the predicate, so it cannot
  mute the rest of the suite. See gh#149 for the skip messages, which still
  misattribute the reason to a missing GGUF.

  The gate ran at `ae418cc`. Commits after it on the release tag are
  documentation only — verifiable with
  `git diff ae418cc..v2.12.0 -- src include`.

- The session pool is mutually exclusive with `entropic_run_batch`: gh#98's
  fan-out needs a unified KV buffer and a pool needs private streams. The
  combination is refused at configure time with a typed error naming both keys.
- A separate draft model (as opposed to a target-owned MTP head) with
  `max_sessions > 1` is likewise refused — both contexts decode the same
  sequence id, and no target-to-draft sequence mapping exists.

# entropic v2.11.1

Patch release — **a config bug that only ever hit fresh installs and CI, and the
build gate that was hiding it.**

Both defects were invisible on a developer machine by construction. That is the
theme.

## The bundled default outranked layers it should never have touched

`load_layered` runs a bundled-default fallback when no model tiers are
configured anywhere. It ran that fallback **after** the project layer, and it
re-parsed the entire `data/default_config.yaml` straight over the
already-populated config — so every setting an explicit, higher-precedence layer
had established was silently overwritten by the layer that is supposed to be the
*least* specific one.

`default_config.yaml` sets `mcp.enable_bash: true`. A project config asking for
`false` got `true` back. That is REQ-CFG-001's most-specific-layer-wins rule
being broken from below.

**Who this hit:** anyone with no `~/.entropic/config.yaml` declaring tiers —
every fresh install, and every CI runner. On a machine that *does* have one, the
fallback never fires and the bug cannot be observed.

The fallback now parses into a scratch config and transplants only the model
block. The condition that triggers it is a missing model set, so the model set
is the only thing it may supply. `REQ-CFG-007` (usable configuration with no
user config present) still holds and is pinned by its own assertion, so this
cannot regress into the opposite bug.

## doxygen-guard is pinned, and CI is green again

The hook was on `rev: main`, a mutable ref. CI installs it fresh every run; a
developer machine keeps whatever it cached. The two ran **different builds of
the same tool** and disagreed about identical code — 3 violations on one, 578 on
the other, same commit. The two builds even print different exemption
vocabularies in their own error text, so the message could not be trusted to
describe the build that produced it.

Now pinned to `v1.4.2`. Upgrades become a deliberate, reviewed act.

With the guard passing, CI reached the unit tests for the first time in over
three weeks — which is how the config bug above was found. A gate that fails
early hides everything behind it.

## `@internal` → `@dg_internal`

`@internal` is a reserved doxygen command; using it as a guard exemption
overloaded a tag doxygen already owns. v1.4.2 defines its own
(`EXEMPTION_TAGS = {"utility", "dg_internal", "callback"}`), so all 929
occurrences across 108 files move to the tag the tool owns outright.

Nothing is newly exempt — every one of these was already exempt under the old
spelling, and the exemption ratio is unchanged.

## Three documentation defects found underneath

| | |
|---|---|
| `backend.cpp` | `evaluate_logprobs` had **two** `@return` tags; only the first was ever used, so the detailed one was dead text. Kept the detailed one. |
| `filesystem.cpp` | two functions' docs **merged into one block** — an "apply string replacement" brief and its four params sat atop `count_occurrences`, documenting nothing. |
| `external_bridge.h` | stale class `@version`. |

## Distribution

- CPU tarball: `entropic-2.11.1-linux-x86_64-cpu.tar.gz` (sha256 in companion file)
- CUDA tarball: `entropic-2.11.1-linux-x86_64-cuda.tar.gz` (sha256 in companion file)
- Python wrapper: `pip install entropic-engine==2.11.1` then `entropic install-engine`

## Known limitations

- Patch release: unit tests only per the version gate. The v2.11.0 model-suite
  result (74/74, 0 skipped, 0 failed) stands; no engine inference path changed
  here.
- `docs/roadmap.md` still reports a stale "Current State" and is not maintained
  alongside GitHub issues.

---

# entropic v2.11.0

Minor release — **the requirements catalog is back and enforced, and four
places where the engine documented behaviour it did not have are corrected.**

Mostly about the engine telling the truth about itself. One feature, #141.

## Requirements traceability, restored and made durable

`docs/requirements.yaml` has been missing since v2.1.0, where a `docs/` purge
deleted all 1008 lines as collateral. Nothing failed, for fifteen months,
because all three consumers of that file fail *open* when it is absent — the
cross-reference check self-disables on an empty catalog, the coverage check only
verifies the config names a path, and impact reporting quietly degrades to "No
requirements affected."

The catalog is rebuilt **from the shipped implementation**, not written ahead of
it: eight parallel subsystem sweeps produced 112 requirements, reconciled to
105. Cross-cutting rules that each sweep had restated at its own boundary were
merged (`REQ-ABI-001/002`, `REQ-SAFE-001`), and one subsystem that no sweep
owned — `ExternalBridge`, whose header and implementation sat in different
scopes — was written by hand as `REQ-BRIDGE-001`.

Coverage went **10/105 → 105/105**. Requirement tags went from 21 against 2318
exemptions (0.9%) to 1634 against 1530 (51.6%).

**New gate: `inv check-requirements`**, wired into pre-commit. Three checks:

- orphan requirement ids, *including on bodiless header declarations* —
  doxygen-guard skips those entirely, which is how a dead `REQ-INFER-003`
  survived unnoticed in `i_inference_backend.h`
- catalog entries nothing implements
- exemption creep, which doxygen-guard's own coverage command is structurally
  blind to because it never collects an `@internal`-only function

A missing catalog now fails closed.

## Documented behaviour that did not exist

The sweeps were told the implementation is the specification. They read headers,
which describe intent — and five times the intent had never been built:

| | |
|---|---|
| `entropic_set_error_callback` | returned `ENTROPIC_OK` while discarding the callback; the type is invoked from nowhere. Now returns `ENTROPIC_ERROR_NOT_IMPLEMENTED`, and the header says so. |
| `FileAccessTracker::was_read_unchanged` | no production caller; the gate only ever checked *was read*, not *unchanged*. Removed, along with a test named `..._detects_external_change` whose own comment conceded the write succeeds. |
| Bash/git command timeout | stored, logged, exposed by an accessor, never enforced. Catalog corrected; filed as #140. |
| `REQ-MCP-021` / `REQ-MCP-023` | both asserted the two above. Rewritten to describe what the code does. |
| VRAM budget resolution | `orchestrator.h:446` documented `env -> cudaMemGetInfo -> 0`; line 594 conceded the `cudaMemGetInfo` half was "intentionally deferred". The gate it feeds has therefore never run on a default deployment. Built — see below. |

**Behaviour change:** `entropic_set_error_callback` now returns
`ENTROPIC_ERROR_NOT_IMPLEMENTED` where it previously returned `ENTROPIC_OK`.
Nothing that relied on the callback firing can break, because it never fired.

## The VRAM admission gate now actually runs (#142)

**This is the behaviour change in this release. Read it before upgrading.**

`ModelOrchestrator::residency_admits()` has refused over-large tiers with
`ENTROPIC_ERROR_TIER_MODEL_TOO_LARGE` since v2.2.4. It never fired. The gate is
guarded by `vram_budget_bytes_ > 0`, and that value came only from
`ENTROPIC_VRAM_BUDGET_BYTES` — unset on every real deployment, so the budget read
as "unknown" and the gate disabled itself. A tier that could not fit went straight
to llama.cpp and took the **host process** down on
`ggml-backend.cpp:179 GGML_ASSERT(buffer)`.

The budget now falls back to the free VRAM the device reports. **Free, not
total**: the reported failure was an operator whose GPU was busier than the
developer's, and a total-derived budget would admit the load and abort anyway.

Switching the gate on required fixing the estimate first, because the old one was
wrong in three ways that all bias toward refusing configurations that work:

| | before | now |
|---|---|---|
| weights | entire file, regardless of `gpu_layers` | priced by offload placement |
| KV cache | flat 16 KiB/token, any `cache_type` | scaled by type — q4_0 is ~0.28x f16, not 1.0x |
| vision projector | not counted | counted — it is the allocation that failed |

**A partially offloaded tier is deliberately not priced at all.** How much of the
file lands on the device depends on a layer count only GGUF metadata carries, so
the estimate reports *unknown* and the gate stays open rather than guessing.
Qwen3.6-35B-A3B IQ3_XXS (~13 GB) runs at `gpu_layers=15` on an 11 GB card, and a
gate that guessed would refuse it. Refusing a working configuration is worse than
missing a broken one, because the operator cannot tell a false refusal from a real
one.

A refusal also logs the largest context length that *would* fit, so what comes
back is a setting rather than a wall.

### What the estimate does not count

llama.cpp reserves graph/activation scratch ("compute buffers") per context at
load time, sized by ubatch and model internals rather than by context length.
Measured here: **1222 MiB** for a gemma-4 E4B MTP head's context at a 512-token
ubatch — more than twice the default `vram_reserve_mb` of 512, and a speculative
configuration pays it twice because it holds two contexts.

It is not estimated, because it cannot be derived without reading GGUF metadata.
`vram_reserve_mb` is the knob that covers it; raise it if you run near the edge.

So, stated plainly: **the estimate can admit a configuration that then fails to
load.** It exists to prevent the catastrophic case — an abort that takes the host
process down — and to hand back an actionable recommendation. It is not a
guarantee. A load that fails after admission surfaces as a typed error, which is
the outcome #142 asked for.

### What consumers should expect

- **Full-offload tiers that never fit** now fail fast with
  `ENTROPIC_ERROR_TIER_MODEL_TOO_LARGE` instead of aborting the process. If you
  were relying on the abort... you were not.
- **Partial-offload tiers are unaffected** — never refused, by design.
- **CPU-only builds and machines with no GPU are unaffected** — no device, budget
  0, gate stays disabled, which is correct: there is no VRAM to exhaust.
- **A busy GPU can now refuse a load that used to succeed** on an idle card,
  because the budget is sampled from free VRAM at `initialize()`. That is the
  intended behaviour — it is the reported scenario — but it does mean the same
  config can be admitted or refused depending on what else holds the card.
  `ENTROPIC_VRAM_BUDGET_BYTES` overrides the device query if you need
  determinism.

## app_context accepts inline content (#141)

`app_context` could only ever name a file. A consumer holding the text in memory
had no supported way to deliver it, and for the reporter writing the file was not
an option: it is a provenance boundary on their side (what the app can rewrite at
runtime it can rewrite wrongly and silently), and on their Android target there is
no stable writable path across launches.

```yaml
app_context:
  content: |
    This family follows a Scandinavian, play-first approach...
```

Resolution order is **explicit opt-out > inline content > path**, so
`app_context: false` still wins over supplied text. Content never touches the
filesystem. Every prior spelling keeps its meaning — bare string is a path, `true`
is bundled, `false` disables, absent is opt-out — each pinned by a regression
test.

Not taken: a `entropic_set_app_context()` C setter, the reporter's other option.
That is public-ABI surface and a larger decision than a bug report should settle.

## Storage write failures are reported

`save_messages` ran an INSERT per row, discarded every result, and returned
`true` unconditionally. `create_conversation` returned a generated UUID even
when its INSERT never landed — so the caller went on to reference an id that did
not exist, and the real failure resurfaced later as something unrelated. Both
now report.

## Speculative decode failures were completely silent

`spec_error()` built the error result and returned it without logging. The
message reached only `GenerationResult::error_message`; nothing appeared in the
log. A consumer whose speculative configuration failed saw `finish=error`, empty
content, and no explanation anywhere — the same shape as the gh#138 gap.

Found the hard way, diagnosing a benchmark config that failed every turn with
`LOAD_FAILED` and produced not one line saying why. With the log line in place
the cause was immediate:

```
Speculative decode failed (ENTROPIC_ERROR_LOAD_FAILED):
  MTP head setup failed: .../mtp-gemma-4-E4B-it.gguf
```

`spec_error()` now logs at ERROR with the code name and the message.

## Diagnostics for gh#137 and gh#138

Neither issue is closed. Both were missing the observability needed to diagnose
them at all.

**gh#138** — there was no signal anywhere that a tool-call grammar was or was
not in force; the orchestration line reports `params.grammar` (the *request*
grammar) and so reads `unconstrained` even when a tool grammar is fully active.
Now logged: what the render derived, whether it reached the sampler, and — at
ERROR — a tier that set `require_tool_call` whose render produced no grammar.
Measured on Gemma-4 QAT, the mechanism the flag actually controls:

```
require_tool_call: true    grammar 4782 bytes, lazy=false   enforced from token 1
require_tool_call: false   grammar 1272 bytes, lazy=true    never triggers
```

`tool_choice: AUTO` yields a *lazy* grammar armed by a trigger the model never
emits — inert by construction.

**gh#137** — a turn that produced tokens and delivered zero content was told to
"Raise max_tokens". That is only sound when the generation was truncated. The
reported case was `finish=stop`: the model ended the turn itself, and no budget
increase can help. The diagnosis is now `finish_reason`-aware and says plainly
when budget is *not* the problem.

The underlying gh#137 defect **did not reproduce** — three GPU runs across E2B
and E4B QAT, including the reporter's exact model, all emitted no reasoning
markers and delivered 100% of their content. The issue stays open with the
untested surface named: `speculative.mtp`, real tool staging, delegation.

## Also

- #139: partial CPU/GPU offload of a hybrid architecture can exceed
  `GGML_SCHED_MAX_SPLIT_INPUTS` and abort in `do_activate`. Filed; full offload
  is unaffected.
- #140: unenforced bash/git timeout.
- gh#131 closed — its three dependencies shipped in v2.10.0.
- **The benchmark gate was reporting failures for benchmarks that never ran.**
  `add_bench_test` registered the binary with no arguments, and the benchmark
  cases are Catch2 `[.]`-hidden — so a bare run collected nothing, exited 2, and
  ctest recorded all three as ~0.1s failures. They are now invoked with
  `"[benchmark]"`.
- **The agentic benchmark was loading the wrong model's vision tower.** It
  repointed its tier at a gemma-4 model but never cleared `mmproj_path`, which
  the default tier inherits from the global config as `mmproj: primary_mmproj`
  — the Qwen3.6-35B projector. 857.6 MiB of an unrelated model's weights, in a
  text-only throughput benchmark, and the exact allocation that produced the
  #142 abort. Cleared, and the matrix now sizes its context per config from the
  same estimator the admission gate uses, reporting configs that do not fit
  instead of attempting them. A run that fits nothing fails rather than
  reporting an empty table.
- Model-suite skip audit: all 78 `SKIP()` sites in `tests/model` are either GGUF
  guards whose files are present, or sit in single-case binaries where a skip
  exits 4 and ctest records a failure. The suite's 74/74 is a real green, not an
  absence of running tests.

---

# entropic v2.10.4

Patch release — **tools-staged tiers were decoding unconstrained (gh#134), and
`type_error.316` is closed at its source rather than per-call-site (gh#136).**

## gh#134 — the tool-call grammar was built and discarded

llama.cpp derives a GBNF from your staged tool schemas during prompt rendering.
entropic harvested the prompt, format, generation prompt and parser from that
render — and **threw the grammar away**. So every tier with tools staged has
been decoding completely unconstrained, and llama.cpp's own
`tool_choice: REQUIRED` mechanism would have had no effect even once exposed.

**New: `require_tool_call` per tier** (opt-in, off by default):

```yaml
models:
  researcher:
    require_tool_call: true
```

The turn then *cannot* end with prose — narrate-then-stop becomes
unrepresentable rather than corrected after the fact.

Measured on Gemma-4 E4B QAT, 10 turns, only the flag varying:

```
off: C C C C C [stop]p C C [stop]p C    2 prose-only turns
on : C C C C C C C C C C               10 tool calls, 0 prose-only
```

The off arm still stalls with a larger budget, so **raising `max_tokens` alone
does not fix this** — the grammar does.

### Budget matters, and the engine now says so

Under `REQUIRED` the grammar allows unbounded text *before* the mandated call,
so the call still has to fit inside `max_tokens`. Too tight a budget produces a
turn that ends on `length` with no call. That case is now logged explicitly,
naming both levers — raise `max_tokens`, or disable `enable_thinking` on the
tier, since the thinking channel is what consumes the preamble.

## gh#136 — `type_error.316`, closed at the source

This crash has been fixed four times (gh#112/113, gh#114, gh#118, gh#132), each
time by sanitizing one more `.dump()` site. There are ~18 in the facade alone,
so the next occurrence always landed somewhere nobody had reached.

Model bytes now get sanitized **where they enter** — the three points where
generated output first becomes a string. Every downstream serialization is safe
by construction, and a newly added `.dump()` anywhere cannot bring it back.

If you were seeing `invalid UTF-8 byte at index N` kill an `entropic.ask`
response, that is closed.

## Standing invariants

Both bugs recurred because every previous fix was per-instance. This release
adds tests that pin the *property*: a fifth grammar source cannot be declared
without being wired to the sampler, sanitized output is always serializable
(with a control proving the test can fail), and the new per-turn backend state
is per-instance — verified, not assumed, for consumers running concurrent
handles.

## Upgrade notes

Nothing is required. `require_tool_call` is opt-in and off by default; tiers
that do not set it behave exactly as before.

# entropic v2.10.3

Patch release — **Gemma-4 reasoning (`<|channel>`) leaked into content and the
live token stream on every generate path (gh#108).**

## The bug

A toolless generate on a Gemma-4 tier returned raw
`<|channel>thought…<channel|>` in `result.content` **and** streamed it live to
consumers. Plain decode, streaming, MTP, batch — all four. Anything rendering a
stream showed the model's private reasoning; conversation history kept it.

If you run a Gemma-4 tier and have seen thinking text in output, this is why.

## Root cause — not what it looked like

Every model family has an adapter that strips its own reasoning markers.
**Gemma-4 had none.** `adapter_registry` deliberately omitted it because its
tool calls are parsed by llama.cpp's `PEG_GEMMA4` grammar — so
`adapter: gemma4` silently resolved to `GenericAdapter`, which strips `<think>`
(a marker Gemma-4 never emits) and left `<|channel>` untouched.

The one `<|channel>` handler lived inside `parse_response`, reachable only when
`common_chat_parse_reliable()` is true — which requires **both** a tooled render
**and** `PEG_GEMMA4`. A toolless call fell through to the adapter branch with no
channel handling at all. Reasoning stripping had been attached to a gate that
exists to answer an unrelated question: *is this captured format
multi-parameter safe?*

MTP, streaming, and grammar were red herrings — a plain-decode non-streaming
control leaks identically. The v2.9.1 MTP-streaming guard was gating one feature
over a defect belonging to a different layer, and never protected anyone.

## What changed

- **`Gemma4Adapter`** — the missing fallback, owning the `<|channel>` pair.
  `PEG_GEMMA4` remains primary whenever a parser arena exists.
- **`ChatAdapter::thinking_markers()`** — each family declares its delimiters
  once, consumed by both the buffered strip and the live stream filter so they
  cannot drift apart again.
- **One shared parse rule** (`response_parse.h`) — template first, adapter
  second — replacing a duplicated branch in the orchestrator and the interface
  factory. Content cleanup always runs the adapter strip (idempotent); tool-call
  extraction falls back to the adapter when the template result fails validation
  against the staged tool schema, which catches `common_chat`'s *silent*
  first-parameter-only extraction.
- **`StreamThinkFilter`** takes adapter-resolved markers. This is load-bearing:
  the agent-loop streaming path builds content from its own token accumulator
  and discards the parsed result, so the filter is the only defense there.
- **Constitutional validator** resolves markers per tier, so critique calls on a
  Gemma-4 tier no longer hand raw reasoning to the critique model as claims.

## Also

Three dead methods removed from `adapter_base.h` (`extract_thinking`,
`parse_bare_json_tool_calls`, `format_system_prompt`) — zero callers in `src/`,
kept alive only by their own tests. `do_unload` now invalidates the sticky
parse snapshot, which nothing previously cleared.

## Known gaps

Model tests for qwen36 and gemma4-a4b remain skipped on the release box for
lack of disk for those GGUFs; both families retain full CPU unit coverage.

# entropic v2.10.2

Patch release — **the bridge no longer answers `"(no response)"` when the
answer is already in the conversation (gh#130).**

## The bug

A turn that ends without `entropic.complete` leaves a trailing **empty**
assistant message — e.g. anti-spiral rejects the lead's tool call and the next
generation returns `finish=stop`, 0 tool calls, 0 chars.
`extract_final_text` scanned backwards, found that empty message first, and
returned it, never looking further back. Operators got the literal string
`"(no response)"` while the real answer sat one or two messages earlier.
Reported at ~4 of 16 runs in a live consumer acceptance matrix.

The worst case involved a completed sub-tier delegation. `fold_delegation_summary`
(gh#119, v2.9.17) already folds a child's summary into the lead's empty
assistant turn precisely so this function can find it — but a *later* terminal
empty assistant turn shadowed it, so an answer the engine had correctly
produced was thrown away at the last step.

**Fix:** skip empty assistant messages and keep scanning backwards.

## Better diagnostics on a genuinely empty turn

`"(no response)"` could not distinguish an engine failure from a model that
simply stalled. It now says which:

- `(no response: the turn produced no assistant message at all)`
- `(no response: the turn ended with every assistant message empty — the tier
  most likely stopped without calling entropic.complete)`
- `(no response: the engine returned no readable conversation)`

`"(no response"` remains the leading substring, so prefix/substring matching on
the old sentinel still fires. **Exact-equality matching on `"(no response)"`
will not** — adjust if you match that string exactly.

## Async ask had it worse

`derive_async_final_state` had no fallback at all: a stalled async
`entropic.ask` returned `status: "done"` with empty text — less diagnosable
than the sync path's sentinel. All three ask paths (plain, streaming, async)
now share one selection rule.

## A note on scope

The report suggested also falling back to "the most recent delegation/pipeline
result text." That is **not** implemented, deliberately. Tool and delegation
results are injected as `role: "user"`, and the serialized conversation carries
only `{role, content}` — so at that layer a delegation summary is
indistinguishable from the operator's own prompt, and using it would echo the
user's question back as the answer. Delegation summaries reach the extractor
through the assistant-turn fold instead. A regression test pins that a user
message is never returned as the answer.
