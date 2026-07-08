_Last 10 releases. Older history: [OLD_NOTES.md](OLD_NOTES.md). Kept short
because `gh release create --notes-file` hits GitHub's 125,000-char release
body limit once this file accumulates full project history — see v2.9.3._

# entropic v2.9.5

Patch — **turn/run entry points now log to `session.log` with the console
sink disabled** (gh#109). `entropic_run`, `entropic_run_as`,
`entropic_run_batch`, `entropic_run_streaming`, `entropic_run_messages`, and
`entropic_run_messages_streaming` never entered a `HandleLogScope`, so the
thread-local handle id stayed unset for the whole turn and
`HandleAwareSink` silently dropped every log line emitted during
generation. Consumers running with `console_logging: false` (e.g. a TUI
that keeps stderr clean for its own paint) got zero turn diagnostics —
`session.log` stopped at "configure complete" and never logged another
line, even on failure.

These six entry points intentionally skip the full `HandleApiLock` so a
long-running turn doesn't block `entropic_interrupt()` called from another
thread — but dropping the lock also dropped the log scope bundled inside
it. Fix enters a bare `HandleLogScope` (no `api_mutex`) at the top of each
instead; `run_turn`/`run_streaming` execute synchronously on the calling
thread with no internal logging worker threads, so a single scope per
entry point is sufficient — no change to the interrupt/cancel contract.

Adds a regression test (`facade_integration_test.cpp`) that configures a
handle via `entropic_configure_dir` with `console_logging: false`, runs a
turn, and asserts `session.log` grows with `[core.*]`-style content —
locking in that every run entry point holds a log scope.

# entropic v2.9.4

Patch — **MTP works with `temperature>0` and grammar-constrained tiers**
(gh#108). v2.9.1-v2.9.3 hardened MTP to fail loud outside a narrow envelope
(greedy-only, no grammar, non-streaming) — but combined with
`speculative.mtp` being a global-only flag, that envelope blocked MTP for
most realistic multi-tier consumer configs. Two fixes, both non-breaking:

- **Dropped the `temperature>0` guard.** Re-derivation found MTP's draft
  proposal (`common_speculative_impl_draft_mtp::draft()`) always proposes the
  argmax of its filtered distribution, never a genuine stochastic sample —
  a deterministic point mass, at any temperature. For a point-mass proposal,
  the standard rejection-sampling accept rule collapses algebraically to
  exactly what entropic's existing accept step already does, so the guard
  was stricter than the math required — not a new sampling algorithm, a
  proof that the existing one was already correct. A statistical model test
  (`test_gh108_mtp_guards.cpp`) empirically confirms MTP's output
  distribution matches plain decode's at `temperature=0.7` and serves as a
  regression tripwire if a future `extern/llama.cpp` pin bump changes the
  draft's selection logic.
- **Per-tier `speculative.mtp` override + request-level grammar safety
  net.** A new `TierConfig::speculative_mtp` (inherits the global flag
  unless set) lets a consumer keep MTP on globally while excluding a
  specific identity/model — e.g. a grammar-heavy tier that should always
  run plain decode. A tier that statically combines `speculative_mtp=true`
  with a static `grammar` is now rejected at config-load time instead of
  failing every request. Independently, a *dynamic* per-request grammar
  (e.g. the constitutional validator's critique call, which is not a
  static tier property) on an otherwise MTP-effective tier now falls back
  to plain decode instead of propagating
  `ENTROPIC_ERROR_SPECULATIVE_INCOMPATIBLE_CONFIG` — fixing a real bug
  where that error was silently swallowed by the validator, disabling
  grammar-based constitutional validation without any visible signal.
- MTP is still grammar-*blind* — these fixes make grammar and MTP coexist
  in a config, not make MTP itself grammar-aware. Grammar-constrained
  speculative decode (validating drafted tokens against the grammar) is
  deferred as a separate capability-track item.

# entropic v2.9.3

Patch — **MTP + flash attention unblocked, verified with real speedup data**
(gh#108). v2.9.2 shipped a loud, correct guard for the MTP-draft-crashes-with-
flash issue rather than a silent fallback or a `GGML_ABORT`. That guard is now
obsolete: upstream llama.cpp merged the fix, and extensive benchmarking
(single-turn, multi-turn agentic, and a 24-config permutation matrix at true
128k context) confirms a genuine speedup — on the right weight quant.

- **`extern/llama.cpp` pin bumped** `ac4cddeb` (2026-06-10) → `b9886`
  (`20a04b22`, 2026-07-06). Pulls in upstream #25148, "CUDA: fix Gemma E4B MTP
  FlashAttention" (merged 2026-06-30, fixes ggml-org/llama.cpp#24400): the
  flash-attn MMA template had disabled the GQA-1/2 specializations as part of
  an earlier compile-time optimization (#21768) — GQA-2 is exactly what the
  Gemma-4 E4B/E2B MTP assistant head uses, so it hit the `DKQ<=256` fallback
  path at head_dim=512 and `GGML_ABORT`ed.
- **Dropped the `flash_attn` guard in `mtp_unsupported_reason`.** MTP now runs
  with flash attention enabled instead of erroring — engaging flash also
  unlocks quantized KV cache (`cache_type_k/v=q4_0`/`q8_0`), which llama.cpp
  requires flash for.
- **Verified real speedup with UD-Q4_K_XL / qat-UD-Q4_K_XL trunks**: 1.05–1.65x
  over Q8 baseline across single-turn and multi-turn agentic benchmarks (see
  gh#108 for the full 24-config matrix). **Mobile-QAT (TQ2_0, `*_qat_mobile`)
  is NOT recommended for this config** — this llama.cpp pin has zero CUDA
  kernels for TQ2_0 (confirmed via source audit, any GPU architecture), which
  forces the tied token_embd/output tensor onto CPU every decode step and
  erases the win regardless of hardware; independently, TQ2_0 also showed
  real reasoning-coherence degradation in testing (non-convergent looping),
  not just a speed cost. Upstream CUDA support (llama.cpp #11183) has been
  open since 2025-01-10 and remains unmerged — tracked, not fixed here.
  `bundled_models.yaml` descriptions updated accordingly; added `mtp_e2b`/
  `mtp_e4b` registry entries so the MTP drafters are downloadable by key.
- **Fixed a tool-calling test-harness bug** (not a production bug): several
  gh#106/gh#108 model tests staged `GenerationParams.tools` in OpenAI's
  `{type:"function", function:{...}}` wire shape, but `mcp_tools_to_common_chat`
  expects entropic's native MCP shape (`{name, description, inputSchema}`).
  The mismatch silently parsed to zero tools every time (`t.value("name","")`
  finds nothing at the top level), so those tests' "tools staged" claims were
  never actually exercising real tool-call parsing. Fixed the JSON shape and
  strengthened `test_gh106_mtp_route.cpp`'s assertion from a hedge ("format
  too unreliable to hard-assert") to a real `tool_calls.size() == 1` check,
  now that the actual cause is fixed.
- Temperature and grammar guards in the MTP envelope are unchanged — MTP is
  still lossless **only at `temperature=0`** and still errors loudly on
  grammar-constrained or streaming tiers.

No `interfaces/i_*.h` touched.

# entropic v2.9.2

Patch — **MTP made usable** (gh#108). Real-hardware testing on an RTX PRO 4000
(Blackwell) confirmed MTP genuinely accelerates when reached in-envelope — **up to
~2.3× on Q8** at `n_draft=4`, byte-correct vs plain. v2.9.1's guards were correct
but left MTP unreachable from the consumer path and over-broad on tools. v2.9.2
makes it actually usable.

- **Tools are no longer refused.** MTP is lossless at temperature=0 and gemma4
  tool-calling is parsed post-hoc (not sampler-grammar-constrained), so MTP +
  staged tools produces the same correct tool call as plain decode. The v2.9.1
  "tools" guard was over-broad — dropped. This makes MTP **reachable through the
  existing agent loop** (`entropic_run`), which stages the built-in meta-tools.
- **MTP honors stop sequences.** The kernel now applies `effective_stop`
  (`params.stop` + the gh#103 sequential-tool close marker), so it stops where
  plain decode would instead of over-generating past the first tool call — the
  actual gap the "tools" guard was masking.
- **`n_draft` default 16 → 4.** 16 over-drafts the tiny MTP head — measured a NET
  slowdown on Q2 (0.53× @16 vs 1.39× @2) and 1.91× vs 2.34× on Q8. 4 is the sweet
  spot (upstream's MTP example uses 3).
- **`swa_full=false`** (general, all Gemma-4 tiers). `llama_context_default_params`
  returns `true` (a full-context SWA cache); the CLI default is `false`. For
  Gemma-4 (sliding-window 512, 5:1 SWA:global) the un-windowed cache wastes ~5 GB
  at 128k. Now windowed — correctness-neutral (SWA attention only uses the window),
  pure memory savings.
- **MTP + flash attention fails loud** (no `GGML_ABORT`). The gemma4-assistant head
  (GQA-2 + head_dim-512) aborts the flash kernel on this pin → a clear error asking
  you to set `flash_attn=false` for MTP tiers. MTP is consequently **locked to f16
  KV** (quantized KV requires flash). The flash + quantized-KV speedup waits on an
  upstream `fattn` fix — tracked in gh#108.

Still **experimental** and lossless **only at `temperature=0`**; grammar-constrained
and streaming tiers still error loudly (those are real sampler/strip constraints,
unlike tools). No `interfaces/i_*.h` touched.

---

# entropic v2.9.1

Patch — **MTP hardening: fail-fast / fail-loud** (gh#108). v2.9.0 shipped MTP
documented as "validated / lossless," but it silently bypassed grammar, tools,
stop-sequences, and streaming, and was lossless **only at `temperature=0`**.
v2.9.1 makes MTP refuse — loudly — to run outside that envelope so a consumer
corrects the config instead of getting silently-wrong output. No capability
change (MTP still only *runs* in the greedy envelope); the *honesty* changes.
No `interfaces/i_*.h` touched.

- **Loud incompatibility errors (never a silent fallback).** When
  `speculative.mtp` is enabled but the request can't run correctly —
  `temperature>0`, an active grammar, staged tools, or a streaming call —
  generation returns the new typed **`ENTROPIC_ERROR_SPECULATIVE_INCOMPATIBLE_CONFIG`**
  with an actionable message (e.g. *"MTP is lossless only at temperature=0 …
  set temperature=0 or disable speculative.mtp"*). The orchestrator
  **propagates** it — it does not quietly run plain decode, which would mask
  that MTP never engaged.
- **No-abort robustness.** `speculative.n_draft` larger than the batch, an empty
  `draft.path`, or a zero-draft round now produce a clear error or are handled
  gracefully — never a `GGML_ABORT` that crashes the embedding host.
- **Thread-safety.** A coarse mutex serialises MTP head setup/teardown against an
  in-flight `generate_mtp`, so a tier-swap `deactivate` can no longer free the
  head context mid-decode (use-after-free).
- **Observability.** The speculative path now populates
  `GenerationResult.throughput_tok_s` (it previously reported `0.0` — the one
  metric the feature exists for; also fixes the gh#36 path).

**Lossless scope clarified:** MTP is lossless **by construction at
`temperature=0`** (greedy argmax accept) — *not* at temperature>0, where the
accept step is naive token-equality rather than speculative rejection sampling.
MTP remains **experimental** and engages only in the greedy / unconstrained /
non-streaming envelope. Making MTP actually *honor* grammar / stop / streaming /
prompt-cache is a designed follow-up (gh#108).

**Perf note (correcting v2.9.0):** MTP measured ~+15% in an *isolated* greedy
benchmark on Pascal, but it is bandwidth-bound there and is **not a net
throughput win** on that hardware — it is a modern-HW lever. (v2.9.0 stated both
"+15%" and "no speedup"; the accurate framing is "engages + lossless, but no net
win on Pascal".)

---

# entropic v2.9.0

Minor — **llama.cpp pin bump + Gemma 4 QAT + MTP speculative decode** (gh#106).
No `interfaces/i_*.h` touched.

## llama.cpp bump → b9592 (+423 commits)

`extern/llama.cpp` `253ba110b → ac4cddeb` (2026-06-11). Brings Gemma 4 QAT tensor
support (TQ2_0 CUDA kernels) and the MTP runtime. Build delta against our code was
a **single** API change (`mtmd_helper_bitmap_init_from_file` gained a `bool` param +
a wrapper return). **Regression-gated**: the full model suite passes on the bumped
build with zero failures across every family, hybrid-KV, and multimodal path — the
bump is transparent to existing consumers and models.

## Gemma 4 QAT (quantization-aware training)

QAT preserves near-bf16 fidelity at a 4-bit footprint. New **opt-in** registry
entries (additive — non-breaking):
- **`gemma4_e2b_qat` / `gemma4_e4b_qat`** (UD-Q4_K_XL) — the **recommended** QAT
  models: Q8-class quality at the Q4 footprint, full CUDA on this pin (~116 tok/s
  for E2B on a GTX 1080 Ti).
- **`gemma4_e2b_qat_mobile` / `gemma4_e4b_qat_mobile`** (TQ2_0 ternary) — smallest
  footprint (~1.95 GB VRAM for E2B), but the ternary CUDA kernel is **compute-bound
  on older GPUs** (≈3× slower than Q4 on Pascal). Opt-in / modern-HW.

The QAT models are **thinking models** — they emit a `<|channel>thought … <channel|>`
reasoning block before the answer/tool-call (always when tools are staged). The
engine now strips this into `reasoning_content` (`strip_thinking_channels` in
`parse_response`), so user-facing content stays clean and tool-calls extract
normally. Give them a generous `max_tokens` (the reasoning precedes the call).

## MTP — multi-token-prediction speculative decode (target-owned)

Lossless speculative decode driven by a tiny (~57 MB) trunk-sharing drafter head
(`mtp-gemma-4-E2B-it.gguf`). **Opt-in** via `inference.speculative.{enabled,mtp}`
with `speculative.draft.path` pointing at the head GGUF. A clean, separate path
from the gh#36 separate-draft kernel (which is untouched):

- **Target owns the head.** The MTP context shares the target trunk's KV via
  `ctx_other` (`LLAMA_CONTEXT_TYPE_MTP`, `n_rs_seq=0`), set up lazily against the
  live context and torn down on deactivate. No second resident model — ~15 MiB
  marginal VRAM.
- **Lossless by construction.** At `temperature=0` the head only contributes a
  token when it equals the target's own greedy argmax (`sample_and_accept_n`); the
  loop is `draft → decode(target) → process → sample_and_accept_n → accept`,
  mirroring upstream's server MTP consumer. The caller never decodes the head
  context — the impl owns it (shared-KV gemma4-assistant topology).
- **Validated** on GPU: engaged (drafts proposed) with a healthy accept-rate on a
  Q8 E2B trunk, through both the backend kernel and the full
  `orchestrator->generate()` route. `GenerationResult.n_drafted` / `n_accepted`
  expose the speculative observability.
- **Quant coverage:** also validated *functional* (lossless, head drives the trunk,
  coherent output) on the mobile QAT **TQ2_0 ternary** trunks for **both E2B and
  E4B**. Accept-rate is low on the ternary quant (~0.06–0.12 vs ~0.26 on Q8) — the
  head's predictions degrade with the lower-fidelity hidden states, so there is no
  throughput win there; it simply works correctly.

MTP's throughput payoff is a modern-HW lever (~+15% on Pascal, more on newer GPUs).
Recurrent/hybrid targets are out of scope for this path (shared-KV gemma4 only).

## Distribution
- CPU tarball: `entropic-2.9.0-linux-x86_64-cpu.tar.gz` (sha256 in companion file)
- CUDA tarball: `entropic-2.9.0-linux-x86_64-cuda.tar.gz` (sha256 in companion file)
- Python wrapper: `pip install entropic-engine==2.9.0` then `entropic install-engine`

---

# entropic v2.8.3

Patch — fixes gh#105: a severe bug where, with **constitutional validation on**,
the validator's interleaved toolless critique render clobbered the backend's
GLOBAL captured PEG parser before the engine re-parsed the main generation — so a
syntactically-perfect tool call extracted as **ZERO calls** and the turn spiraled
(consumer fully blocked with validation enabled). Also fixes the gh#103
sequential hard-stop, which resolved its close marker one generation late and so
never fired on the generation it was configured for. No `i_*.h` touched.

## gh#105 (A) — per-call render-param capture (the killer)

The captured common_chat render params (format / generation_prompt / parser) were
single GLOBAL mutable backend state. A toolless render (`render_prompt` clears
`have_chat_params_`) — e.g. the constitutional validator's critique, which the
engine fires on POST_GENERATE **between** the main generation and its re-parse
(`engine.cpp:529-543`) — cleared the main call's parser before `parse_response`
ran → `common_chat_parse_reliable()` false → fallback → 0 tool calls → the engine
injected "no tool call, retry" → spiral. (Pre-existing global-state design;
unrelated to gh#103.)

Fix: a **"sticky last-tooled"** snapshot (`parse_*`), written ONLY by a successful
`render_with_tools` and NEVER cleared by a toolless render. `parse_response` /
`common_chat_parse_reliable` read the snapshot; the live capture still serves
`has_common_chat_params()` / `tool_call_close_marker()` (this-render semantics).
A validator interleave can no longer clobber the main call's parser.

## gh#105 (B) — gh#103 sequential stop resolved one generation late

`inject_sequential_stop` ran at `resolve_and_stage` time, BEFORE this call's
render, so the close marker came from the PREVIOUS render's format (or empty on
the first call) — it never fired on the generation it was configured for. Moved
the injection POST-render into the backend (`LlamaCppBackend::effective_stop`,
applied in every decode loop), deriving the marker from THIS call's captured
format. Removed the orchestrator's pre-render `inject_sequential_stop`.

## Tests
- **New RED-first engine-loop test** (`test_gh105_validator_clobbers`): full
  `AgentEngine::run` with constitutional validation ON + tools on gemma4_e4b —
  FAILS on v2.8.2 (0 tool-call extraction; verified), passes with the snapshot.
- The gh#103 sequential model tests (gemma4_e4b + qwen35moe) were **vacuous** —
  the marker was never injected (pre-render, first call), so they passed on the
  model's voluntary EOG. Strengthened with a log-scan **non-vacuity guard** that
  proves the marker actually injected post-render.

INTERFACE NOTE: no `interfaces/i_*.h` change; removed an internal orchestrator
method (`inject_sequential_stop`).

---

# entropic v2.8.2

Patch — two consumer-reported correctness fixes around terminal/looping tool
calls (gh#103, gh#104). No `i_*.h` interface header touched.

## gh#103 — `tool_call_mode: sequential` hard-stops generation at the first tool call

Terminal directives (`delegate`/`pipeline`/`complete`) and dependent-tracing
workers had no way to stop generating at a tool-call boundary: the model
generated its *entire* response past the action, and only afterward did the
engine process directives. A worker doing a dependent trace emitted multiple
tool calls in one turn with **no result seen between them** → blind planning →
non-convergence (the gh#103 repro: 37 calls, 0 `complete`, delegations failed).

New per-tier (and per-call) **`tool_call_mode`**:
- `"sequential"` → the orchestrator appends the family's tool-call close marker
  to `GenerationParams.stop`, so the **existing decode-loop stop check** halts
  generation at the FIRST closed tool call (one tool/turn — the model observes
  each result before the next call). Reuses `check_stop_sequences` on the shared
  decode loop (streaming + batch); **no new streaming detector, no `cancel_flag`
  plumbing**. The marker is RETAINED in the output, so common_chat still parses
  the complete call.
- `"batch"` (default, unset) → unchanged: parallel/independent tool calls in one
  turn are preserved.

Close markers are derived family-aware from the resolved common_chat format
(`tool_call_markers.h`): PEG_NATIVE / PEG_SIMPLE → `</tool_call>`; PEG_GEMMA4 →
`<tool_call|>` (gemma wraps a call as `<|tool_call>call:NAME{…}<tool_call|>` — the
close is distinct from the open, so the ends-with check fires on the close);
CONTENT_ONLY / unknown → `""` → no stop injected (batch-safe). The marker map AND
the engagement decision (`append_sequential_stop`) are CPU-unit-pinned; the
end-to-end hard-stop is validated by emergent model tests on **qwen35moe**
(hybrid: `≤ 1` tool call/turn, ends AT the marker, KV coherence) and on
**gemma4_e4b** (the severe case below).

**gemma4 was the severe case, and the hard-stop fixes it.** A consumer session
showed gemma4 emit a complete `entropic.delegate`, generate *past* it, and the
call register as **zero tool calls** — the terminal directive dropped entirely
(0 rows in the `delegations` table), not just tokens wasted. Root cause: the
runaway past the call defeated extraction. Sequential mode stops at the first
`<tool_call|>`, leaving a clean single-call stream that common_chat parses — the
GPU test confirms the call now **extracts** under sequential mode (it was
runaway-defeats-extraction, not a parse bug). Consumers hit by this set the
affected tier to `tool_call_mode: sequential`.

Terminal-action halting for *batch* tiers is unchanged (the existing post-parse
`stop_processing` directive path). The generation-time hard-stop is the
sequential lever; a consumer marks its lead/worker tiers `sequential` to claim it.

## gh#104 — tool descriptions present capability, not availability

`complete.json` hardcoded *"This tool is only available during delegation (child
contexts)"* — false for any root tier that allow-lists + requires `complete`
(`explicit_completion: true`). The claim was never enforced (verified against the
engine headers); it only misled the model, and a thinking lead burned a turn
litigating the contradiction. A tool description is broadcast to every tier that
allow-lists it, so it **cannot assert a per-context availability or caller
topology** — that is `allowed_tools`' job. Rewrote `complete.json` capability-only
(dropped the availability + parent/child framing; kept purpose + `coverage_gap`
effect semantics). A sweep confirmed it was the only such violation across the
bundled schemas.

---

# entropic v2.8.1

Patch — **activates model-based tier routing** (the v2.8.0 audit found it was a
silent no-op) and hardens the surrounding config. No `i_*.h` interface header
touched.

## Routing: `classify_task` instructs the router

`ModelOrchestrator::classify_task` fed a non-fine-tuned router the bare
`"<msg> ->"` with `max_tokens=1`; a general instruct model just continues the
text and never emits a routing digit, so `model_raw` was always empty and
`route()` silently fell back to the default tier. When
`routing.classification_prompt` is configured, it is now prepended (so the model
is told the digit scheme) and `max_tokens` is widened to 4 to capture the
leading-space digit. **Unconfigured deployments keep the exact bare
`"<msg> ->"` + `max_tokens=1` path — byte-identical, no behavior change.**

## Config hardening (review fixes)

- **Empty `tier_map` is now rejected** when `routing.enabled` +
  `classification_prompt` are set — otherwise the router emits a digit that maps
  to no tier and every route silently falls back (the same no-op by a different
  door). `validate_routing` now fails loudly at load.
- **`classification_prompt` logs an INFO line** when its (active) path is taken —
  it was parsed-but-never-read before v2.8.0, so a deployment carrying a stale
  prompt now sees the inert→active switch + the widened token budget.
- Removed the stale `@deprecated` banner on `RoutingConfig` (it claimed removal
  in v2.2.0 — false; v2.8.x actively builds on it).
- Added a cleared-prompt **RED control** test so the routing model-test is
  non-vacuous (the bare path is asserted to be a no-op).

---

# entropic v2.8.0

Minor release — two consumer-facing capabilities for the games consumer
(many cheap agents on one small GPU), the family/size/quant selector UX, and
test-infra hardening that closes the harness↔production fidelity gap. Two
additive public C ABI symbols; **no `i_*.h` interface header touched.**

Also folds in a full test-suite plumbing-not-feature **audit** (task #71) and
its RED-first fixes — which surfaced a real production finding: a **q8-KV
NPC-tier validation** (#100). (The audit also found model-based tier routing is
a silent no-op; that production fix is split to **v2.8.1**, since no consumer
uses routing yet.) See the audit section below.

## gh#98 — `entropic_run_batch`: same-prefix batched generation

N independent requests that share a large prompt prefix (e.g. NPC agents per
game tick: a shared constitution/context + a short per-agent suffix) now run
**together in one forward pass per step** instead of N serial calls. The shared
prefix is prefilled once into seq 0, `llama_memory_seq_cp`-fanned out to N
sequences, the suffixes prefilled in one batch, then a generation loop decodes
**all N sequences in a single `llama_decode` per step**, each sampled under its
own tier's grammar (fixed-N grammar chains). This is the "process many
same-prefix requests in one forward pass" win the issue asks for — it breaks the
sequential-NPC ceiling that warm-keep (gh#96, which shares the prefill but still
decodes sequentially) cannot.

- ABI: `entropic_run_batch(handle, const char** tiers, const char** prompts,
  size_t n, char** result_json)` → JSON array of `{content, finish_reason,
  tool_calls}`. Additive.
- `kv_unified` is enabled when `n_parallel > 1` (required for `seq_cp`; also
  llama.cpp's recommendation for shared-prefix sequences).
- Guarded (`batch_is_viable`): hybrid/recurrent archs, <2 requests, no shared
  prefix, over sequence-slot capacity, or suffixes exceeding the decode batch
  all fall back to the unmodified per-request serial path — no regression.

## gh#99 — `entropic_run_as`: per-call tier/grammar on a shared model

One process can interleave agent *kinds* (NPC / companion / BBEG) that need
different grammars per call while sharing ONE resident model — essential on an
11 GB GPU where a second model can't fit. `entropic_run_as(handle, tier, prompt,
result)` runs a single turn under the named tier's grammar + system prompt +
samplers, reusing the resident model (no second load, no reconfigure). Builds on
the per-tier resolution that already existed internally; the gap was purely the
ABI surface. Unknown tier → `ENTROPIC_ERROR_IDENTITY_NOT_FOUND`.

## gh#62 — model family/size/quant selectors

`BundledModels::find_by` is now wired into both consumer surfaces: a tier may
omit `path:` and give flat `family:` / `size:` / `quant:` keys (resolved to the
bundled key), and `entropic download` gains `--family/--size/--quant`. Explicit
`path:` still wins; a partial or unmatched selector errors clearly.

## gh#93 — test-infra hardening (closes the harness↔production gap)

The model-test harness built the orchestrator directly, bypassing
`configure_common` — the seam that let three reactive consumer bugs
(gh#88/90/94) through. Two structural fixes:
- A single typed `serialize_tool_calls` (`src/inference/tool_call_serialize.h`)
  is now the source of truth for BOTH production and the harness; the harness
  previously emitted untyped (raw-string) tool-call arguments, a silent
  divergence — now faithful by construction.
- A facade-driven model-test harness (`tests/model/facade_model_helpers.h`)
  exercises the real `entropic_create → entropic_configure_dir → run` path,
  hosting new behavioral tests (gh#94 `enable_thinking:false`, autoparser
  multi-parameter tool calls, cancel-bridge interrupt — closes #103).

## Tests

- CPU: `batch_shared_prefix_len` + `batch_is_viable` (the gh#98 guard);
  `entropic_run_as` precedence + unknown-tier; gh#62 selector resolve/error;
  the typed serializer.
- GPU (validated): `test_gh99_run_as` (per-call grammar switching on a shared
  model — reproduces the consumer's "loads companion, still emits npc grammar"
  symptom), `test_gh98_batch` (multi-seq decode engaged + prefix-once + hybrid
  serial-fallback), `test_gh93_behavioral` (thinking / autoparser / cancel).

## Test-suite audit (task #71)

A full plumbing-not-feature audit of the 170-file test suite (tests that pass
green while the feature is broken). Each fix is RED-first verified. It surfaced
a real production finding plus RED-first hardening:

- **#100 — q8-KV is safe on the E2B-Q4 NPC tier.** Validated on-box:
  `cache_type_k/v: q8_0` + `flash_attn` on Gemma-4 **E2B-Q4** halves per-context
  KV (48→25.5 MiB alloc; ~0.53× per-seq) with no activation breakage — unlike
  E4B-Q4, which it breaks. The `cache_type_k/v` knob is honored **per-tier**, so
  the `npc` tier can run q8-KV alone → ~2× more #98 batch slots in the same
  VRAM. Adds the `gemma4_e2b_q4` registry entry + `test_gh100_q8_kv`.
- **Hardening (RED-first):** gh#88 defang + gh#84 anti-spiral combined-path
  SCENARIOs (closed the isolated-helper / pre-commit-blind gap); gh#95 CPU
  grammar-engagement proof (closed the fail-open that only GPU tests caught);
  gh#94 effective-sampler member-path test; dynamic-identity (test8) vacuous
  assertion strengthened; gh#98 hybrid serial-fallback decode-count assertion.

**Split out to v2.8.1:** the audit also found that model-based tier routing is
a **silent no-op** (`classify_task` never feeds the router its classification
prompt). That production fix + its test are held for v2.8.1 — no consumer uses
routing yet, so it does not gate this release. Fully wiring model-routing from
identity focus/examples (the dead `build_classification_prompt`) remains a
v2.9.0 item.

Other deferred audit strengthenings (lower-value or high-harness-risk): e5
post-compaction, e7 real-delegation harness, gh#93 autoparser backend-direct
re-seam, gh#64 accumulator.

---

# entropic v2.7.6

Patch release. **gh#97 — v2.7.5 warm-keep / prompt-cache corrupted KV
bookkeeping on hybrid-architecture models.**

## The bug

On hybrid archs (`qwen35` / `qwen35moe` and anything in
`llm_arch_is_hybrid()` — attention + recurrent/SSM memory), the prompt-cache
prefix machinery desynced the KV cache. Warm-keep's partial
`llama_memory_seq_rm(0, cut, -1)` is **rejected** by recurrent memory ("a
state can't be partially erased at the end"); the rejected return was
discarded, so the decode of the delta appended past the un-removed tail and
the seq-0 position high-water mark inflated every turn (`pos_max` 140 after a
107-token prompt, growing ~18/turn). At long context this eventually produced
`decode: failed to find a memory slot for batch` with the cache ~85% empty —
severe multi-turn thrash. The pre-existing prompt-cache restore path
(`llama_state_seq_set_data` → non-contiguous cells) was independently broken
on hybrid memory, so `warm_keep: false` only reduced the failures.

The gh#96 benchmark model (gemma4-E2B) is a plain `llama_kv_cache` where both
partial `seq_rm` and contiguous restore behave — so the hybrid path was never
exercised. (Reported by a consumer running qwen3_6_a3b, source-grounded.)

## The fix

Guard the **entire** prompt-cache prefix path (warm-keep *and* cache restore)
behind `is_hybrid_ || is_recurrent_` and fall back to plain `run_prefill`
(clear + contiguous full decode) — the only correct path for these archs at
the pinned llama.cpp. Mirrors the existing speculative-decoding guard.
**Consequence:** hybrid models forgo the gh#96 warm-keep speedup (full
re-prefill per turn, as before v2.7.5) but are correct; the optimization is
unchanged for plain-KV-cache archs (gemma4 etc.). Internal change — no public
API / interface header touched.

## Tests

- `test_gh97_hybrid_cache` (GPU, the repro arch Qwen3.6-35B-A3B / qwen35moe):
  red→green on a deterministic desync gate — a correct prefill leaves
  `kv_pos_max ≈ input + generated`, so the test asserts `kv_pos_max < input +
  max_tokens` per turn. On v2.7.5 this is violated by turn 2 (the inflation);
  the guard keeps it bounded. Closes the arch-coverage gap (every gh#96 test
  ran on non-hybrid gemma4).
- Regression: `test_gh96_warmkeep` still green — warm-keep keeps engaging on
  the non-hybrid path.
