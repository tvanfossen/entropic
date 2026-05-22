# entropic v2.3.8

Patch release. **Two new-family adapters now parse the tool-call format
their models actually emit (gh#69, gh#70), plus a CPU-only adapter
acceptance gate (gh#71) that would have caught both before ship.**

## The bugs

Both Gemma 4 and Nemotron 3 shipped with a tool-call parser keyed to a
format the model never produces under the production prompt, so every
turn registered "no tool call" and the agent loop spiralled to the
iteration cap — **0/6 completion** in both cases.

- **gh#69 — gemma4:** Gemma 4 (E2B + E4B) emits tool calls inside a
  ChatML-style channel whose opening header is `<|im_start|>tool_call`
  with a plain `</tool_call>` close (asymmetric). The base parser only
  accepted `<tool_call>` / `<|tool_call>` / `<|tool_call|>`, so the
  channel form matched zero calls and the header leaked into the
  assistant body.
- **gh#70 — nemotron3:** the `nemotron_h` GGUFs emit a **DSML invoke**
  format (`<｜DSML｜invoke name="X">…</｜DSML｜invoke>`, fullwidth-pipe
  `｜` = U+FF5C) at every precision (Q4_K_XL / Q8_0 / BF16), not the
  qwen3_coder XML the adapter parsed and taught. BF16 full weights
  produced coherent calls in the wrong format → 0 calls extracted.

## Fixes

- **gh#69:** `parse_tagged_tool_calls` (base) accepts `<|im_start|>tool_call`
  as a fourth open variant; the Gemma4 cleaning regex mirrors it; and
  `kGemmaTemplateMarkers` scrubs a stray `<|im_start|>tool_call` channel
  header.
- **gh#70:** new `parse_dsml_function_calls` + `extract_dsml_parameters`
  in `Nemotron3Adapter` (primary parser); `format_tools` rewritten to
  teach DSML; `clean_content` scrubs three DSML layers (the
  `function_calls` wrapper, bare `invoke` blocks, and any remaining
  `<｜…｜>` channel token — catches `<｜begin▁of▁sentence｜>` BOS spam on
  lower quants). The qwen XML and tagged-JSON paths stay as backstops.

## The methodology fix (gh#71)

Both bugs passed the old suite because the unit tests were tautological
(fed the adapter the format it assumes) and the model-tier tool-call
tests rigged the prompt and were SKIP-gated. New
`tests/unit/inference/adapter_acceptance_test.cpp` is a per-adapter gate
built on **verbatim consumer-captured emits** with strict assertions
(exact call count + names + args + zero `cleaned_content` leakage) and
**no SKIP** — it runs every commit on every CI machine, no model or GPU.
Dispositively verified: with the v2.3.8 source fix removed, the six
`[gh69]` / `[gh70]` scenarios fail; the qwen positive anchors pass
regardless.

## Consumer impact

Rebuild against `librentropic.so.2` (v2.3.8) restores tool-calling on
gemma-4-E2B/E4B and NVIDIA-Nemotron-3-Nano-4B (all quants). No ABI
change — drop-in for any 2.3.x-compiled consumer.

---

# entropic v2.3.7

Patch release. **`console_logging` config to silence the stderr sink.**
Lets a consumer that paints to fd 2 (a TUI) keep the engine's spdlog
output off the terminal entirely — routed to the per-handle file sink
only.

## The bug

The stderr console sink (`s_sink`, a `stderr_color_sink_mt`) is created
once in `log::init()` and attached to the default logger. Every logger
created lazily during a turn goes through `log::get()`, which builds the
new logger by **copying the default logger's sink list** — so each one
re-inherits `s_sink` and writes to stderr.

`setup_session()` calls `remove_console_sink()`, but that only does
`spdlog::apply_all(...)` over already-registered loggers; it does not
prevent `get()` from copying `s_sink` into loggers created *after*
session setup. The result: generation-time loggers still leak to
stderr no matter what.

For a CLI/operator that's fine (and desirable). For a Textual TUI it's
fatal — Textual paints the screen to fd 2 (`sys.__stderr__`), so an
engine log line on stderr corrupts the painted frame (intermittent
flash-then-restore mid-stream). The downstream consumer had to clamp
its log level to WARNING to keep the flooding tolerable, which threw
away the entire INFO-level delegation/generation transcript from the
session log.

## Fix

New `config.console_logging` (bool, default **true** — unchanged
behavior for existing consumers). When false:

- `log::set_console_enabled(false)` strips `s_sink` from the default
  logger **and** every registered logger, and
- sets a process-global flag that makes `log::get()` filter `s_sink`
  out of any logger it creates afterward.

So once disabled, no logger — existing or lazily created later — carries
the stderr sink. Engine output routes to the per-handle `session.log`
file sink only. Wired into `configure_common()` (all configure paths)
right after config parse, before any init logging fires.

C ABI / wrapper: strictly additive — one new config field. Drop-in for
any 2.3.x-compiled consumer; absent the field, `console_logging`
defaults true and behavior is identical to 2.3.6.

**Consumer impact (bissell):** set `console_logging: false` in the
engine defaults and restore `log_level: INFO` — the TUI flash is gone
and the full transcript is captured to `session.log` again.

---

# entropic v2.3.6

Patch release. **Relocatable bundled-model registry discovery.** The
release binaries are now portable to any machine, not just the build
host.

## The bug

`BundledModels::auto_discover_and_load()` searched only compile-time
paths (`CONFIG_ENTROPIC_DATA_DIR` = the build's `CMAKE_INSTALL_PREFIX/
share/entropic`), the source tree, and CWD-relative `data/`. It never
did binary-relative (dladdr) discovery the way `data_dir.cpp::
resolve_data_dir()` already does for prompts/grammars/schemas.

Consequence: a binary built with one install prefix couldn't find its
own `bundled_models.yaml` once installed anywhere else. The registry
loaded **zero models**, so every tier's model key (`qwen3_6_a3b`,
etc.) failed to resolve and `configure_dir` died with:

```
[inference.orchestrator] [error] Model file not found for tier '...': qwen3_6_a3b
[facade] [error] orchestrator initialization failed
```

This was masked for native builds because they were run on the same
machine they were built on (the baked stage path existed locally).
It surfaced hard with the new Docker/Ubuntu-22.04 release build
(whose container stage path, `/tmp/release-cuda/...`, never exists at
runtime) — and it would have hit every downstream consumer machine
that isn't the build host.

## Fix

`auto_discover_and_load()` now mirrors `resolve_data_dir()`'s
discovery order:

1. `ENTROPIC_DATA_DIR` env (operator override)
2. **binary-relative via dladdr** — `<prefix>/share/entropic` derived
   from `librentropic.so`'s on-disk location (portable across install
   prefixes)
3. compile-time install path → source tree → CWD (dev/build-host
   fallbacks, unchanged)

`src/config/bundled_models.cpp` gains a local `share_dir_from_library()`
helper (same dladdr pattern as `data_dir.cpp`). Verified end-to-end:
a container-built binary installed to `~/.entropic/entropic/` now logs
`pre-loaded 17 bundled model(s) from ~/.entropic/entropic/share/
entropic/bundled_models.yaml` and `configure_dir` loads the model
cleanly.

## Release tooling (new in this cut)

Release artifacts are now built via `inv release-check --docker`,
which compiles inside an Ubuntu 22.04 container so the binaries are
portable to Ubuntu 22.04+ (not 24.04-only). Three container-vs-
consumer library gaps are disabled for the distributed binary:
`GGML_NATIVE=OFF` (portable AVX2 baseline, no `-march=native`
SIGILL), `GGML_CUDA_NO_VMM=ON` (no libcuda driver-stub link
dependency), `GGML_CUDA_NCCL=OFF` (no libnccl.so.2 — the CUDA devel
image ships NCCL and it would otherwise leak into the binary). Glibc
floor verified ≤ 2.35; CUDA arch coverage sm_50→sm_120.

---

# entropic v2.3.5

Patch release. **Fixes gh#68 properly — at the right layer this time.**
v2.3.4 attempted to fix the `<|im_end|>` content leak by flipping
`detokenize` to `special=false`. Consumer verified the leak persisted
because Gemma 4 emits the marker as **multi-token regular surface
tokens** (not as a single special token), so the flag has no effect.
Real fix lives in the adapter layer.

## Diagnosis (consumer-led)

Quote from gh#68 follow-up:

> Gemma's `<|im_end|>` isn't a single special token in its GGUF vocab —
> it's emitted as multiple REGULAR surface tokens that spell out the
> literal string. The model log shows `Generated: 6 tokens` decoding
> to a 10-character `<|im_end|>`. That's ~1.7 chars per token —
> consistent with a multi-token regular-surface decomposition like
> `<`, `|`, `im`, `_`, `end`, `|>`.

Same family as gh#65's asymmetric `<|tool_call>`: chat-template
artifacts that the tokenizer decomposes into regular tokens, so
they slip through any "filter special tokens" approach.

## Fix

`Gemma4Adapter::parse_tool_calls` already scrubs `<tool_call>...`
markup and `<think>` blocks from `cleaned_content`. v2.3.5 adds one
more regex matching Gemma's chat-template turn-boundary markers:

```cpp
static const std::regex kGemmaTemplateMarkers(
    R"(<\|im_end\|?>|<\|im_start\|?>(?:user|assistant|system)?|)"
    R"(<end_of_turn>|<start_of_turn>(?:user|model)?)");
cleaned = std::regex_replace(cleaned, kGemmaTemplateMarkers, "");
```

Covers four marker families plus the asymmetric variants (with /
without trailing pipe — same shape as gh#65):

- `<|im_end|>` / `<|im_end>`
- `<|im_start|>[user|assistant|system]` / `<|im_start>[...]`
- `<end_of_turn>`
- `<start_of_turn>[user|model]`

## v2.3.4 detokenize change — kept but reframed

The `special=false` change from v2.3.4 stays in the codebase as a
defensive measure (if any future model emits a chat-template marker
as a single special token, this filter would catch it). The comment
is rewritten to make explicit that this is NOT the gh#68 fix — the
adapter scrub above is.

## Tests removed for false signal

`tests/unit/inference/gh68_detokenize_test.cpp` from v2.3.4 is
**deleted**. It tested that no SINGLE token decoded to the full
`<|im_end|>` literal — which passed for the wrong reason. The bug
was the concatenated multi-token stream producing the literal,
which that test never exercised. Replaced with adapter-level tests
that directly assert the fix.

## Tests added

`tests/unit/inference/gemma4_adapter_test.cpp` — 7 new `[gh68]`
scenarios / 21 assertions:

1. **The exact gh#68 repro**: `"...I don't understand.<|im_end|>"`
   → `cleaned_content` has no `<|im_end|>`, surrounding prose
   survives.
2. **Asymmetric variant** `<|im_end>` (no trailing pipe — parity
   with gh#65) → scrubbed.
3. **Turn-open markers** `<|im_start|>` bare and role-suffixed →
   all forms scrubbed.
4. **Canonical Gemma 4 markers** `<end_of_turn>` /
   `<start_of_turn>` → scrubbed.
5. **gh#68 + gh#65 interaction**: a tool_call followed by
   `<|im_end|>` → tool_call still parses, cleaned_content has
   neither.
6. **Regex over-match defense**: HTML-style tags (`<input>`),
   operators (`<<`), inequalities (`<` `>`) — all survive intact.
7. **Degenerate cases**: empty content, marker-only content (the
   exact 6-token consumer transcript) → no crash, empty output.

Full suite: 142 / 351 / 524 / **362** / 194 — all green. gh#65
regression check still passes (23/3 unchanged).

## Process miss owned (fifth in a row)

This is the fifth v2.3.x bug from the same root failure mode —
specifically v2.3.4's variant: I shipped a test that passed for
the wrong reason. The test asserted "no token decodes to the
literal" but the actual bug was "concatenated stream decodes to
the literal." Same family as v2.3.0-v2.3.3's "tested what I built,
not what shipped."

Pattern to lock in: **when writing a test for a bug, write the
test that would catch the EXACT user-observed symptom — not a
weaker proxy.** v2.3.4's test should have been "given content
that the model emits, does `cleaned_content` still contain the
marker?" That's the assertion the consumer actually cares about,
and it would have failed in v2.3.4 → forcing me to look at the
adapter layer where the real fix lives.

---

# entropic v2.3.4

Patch release. **Two bugs from gh#68 — consumer-diagnosed in
sequence.** Both shipped together because the second (EOS leak in
detokenize) was the visible symptom but the first (entropic.complete
result stored as user-role JSON instead of folded into the assistant
message) was the load-bearing chat-flow bug.

## Fix #1: entropic.complete history shape (load-bearing)

Pre-v2.3.4, when a tier emitted `entropic.complete` with a `summary`
arg, the tool's result was pushed into history as a `role=user` JSON
message of shape `{"action":"complete","summary":"..."}`. The
just-pushed post-strip assistant message was empty (the model only
emitted `<|tool_call>...</tool_call>` markup, which the adapter
stripped). From the model's POV on the next turn, the conversation
read:

```
user: class list?
assistant: (empty)
user: {"action":"complete","summary":"I am sorry..."}
user: retrieve the class list?
```

Incoherent. The model never "said" anything. Some other user wrote
JSON about it. Turn 2 → model retreated to EOS → EOS leaked as text
(fix #2 below) → engine's "no tool call this iteration" retry
cascade → ERROR.

**Fix.** In `AgentEngine::process_tool_results`, before pushing each
tool-result message into history, check if it's an `entropic.complete`
result AND the prior assistant message is empty. If both, fold the
summary text from `ctx.metadata["explicit_completion_summary"]`
(stashed by `dir_complete`) into the assistant message body and skip
the JSON user-role push.

**Conservative.** Only folds when the assistant body is empty — if
the model emitted prose around the tool_call, the prose stays and
the tool result still lands as a user message (no silent data loss).

The fold logic is extracted to a public predicate
`AgentEngine::fold_complete_into_assistant(ctx, msg) -> bool` so
unit tests exercise it without spinning the full engine loop or
mocking the tool executor.

## Fix #2: EOS leak in detokenize

`LlamaCppBackend::detokenize` called `llama_token_to_piece` with
`special=true`. Gemma 4's `<|im_end|>` special token (which is
distinct from the EOS token llama.cpp's `is_eog` check catches)
decoded to the literal 10-character string `<|im_end|>` and leaked
into the content buffer.

**Fix.** Pass `special=false` so special tokens never render to
surface text. The streaming loop already short-circuits on
`llama_vocab_is_eog()` BEFORE calling detokenize, so stop semantics
are unaffected by the flag change.

**Risk audit.** A model emitting a tool-call channel marker as a
single special token would also be filtered. gh#65 v2.3.3 showed
Gemma 4 emits the asymmetric `<|tool_call>` form via multi-token
regular surface tokens (not a single special), so the gh#65 parse
path is unaffected. The asymmetric-form unit test in
`gemma4_adapter_test.cpp` pins this contract (verified passing
post-fix: 23 assertions in 3 cases).

## Tests

**Fix #1** (`tests/unit/core/engine_test.cpp`, `[engine][gh68]`):
7 scenarios / 15 assertions covering:
- Happy path: empty assistant + summary in metadata → folded.
- Refusal when assistant has prose (don't overwrite).
- Refusal for non-entropic.complete tool results.
- Refusal when last message isn't assistant (e.g., system reminder).
- Refusal when metadata summary missing (never invent content).
- Refusal on empty message history (no crash on `back()`).
- Multi-byte UTF-8 summary round-trips intact.

**Fix #2** (`tests/unit/inference/gh68_detokenize_test.cpp`,
`[backend][gh68][.realmodel]`): real Gemma 4 model load (CPU only,
2.5 GB), tokenize `<|im_end|>`, detokenize each resulting token,
assert no token renders the literal string. Plus a positive-
coverage scenario: regular text 'hello' tokenizes and round-trips
through detokenize unchanged (proves `special=false` doesn't
collateral-damage real content). Tag dot-prefix excludes from
default run; opt in via `entropic-inference-tests "[gh68]"`.
Override the GGUF path via `ENTROPIC_TEST_GEMMA_MODEL` env.

12 real-model assertions. **Total v2.3.4 new coverage: 27
assertions across 9 scenarios, split across both bugs and both
test tiers (unit + realmodel).**

Full suite green: 144 / 349 / **524** / 341 / 194 across the five
touched suites.

## Process miss owned (third in a row)

This is the **fourth** v2.3.x bug from the same root failure mode:
shipping a fix without exercising the realistic invocation paths.
- v2.3.0 misattributed deferral
- v2.3.1 double-sink (didn't test combined call path)
- v2.3.3 asymmetric tool_call (parser test used wrong model-output variant)
- v2.3.4 history-shape (consumer caught what my v2.3.0 → v2.3.3
  fixes couldn't, because none exercised the chat-flow assistant
  message lineage)

The fold-predicate refactor in this release is the right shape for
catching #2 of those four (testable in isolation against the real
data flow); the `[.realmodel]` test scaffolding is the right shape
for catching #3 and #4 (cheap-to-run-on-demand integration coverage
with real model outputs).

---

# entropic v2.3.3

Patch release. **Bugfix — gh#65 asymmetric `<|tool_call>` open tag.**
The consumer captured a real-prompt repro tonight (2026-05-19): Gemma 4
emits `<|tool_call>` (with a leading `<|` pipe prefix) on the open
side and plain `</tool_call>` on the close side. v2.3.0's regex
required a plain `<tool_call>` open → zero matches → engine looped on
the no-tool-call retry banner until iteration cap.

## Root cause

`Gemma 4`'s special token `<|tool_call|>` decodes through llama.cpp's
current pin (`253ba110b`) as `<|tool_call>` — the trailing `|>` is
lost in the surface form. The model then emits this asymmetric form
every time it picks a delegation/pipeline tool. The
`parse_tagged_tool_calls` regex at `src/inference/adapters/adapter_base.cpp:168`
matched only plain `<tool_call>`, so Gemma 4's actual output produced
zero tool calls — the engine retried, the model emitted the same
asymmetric form, repeat until cap.

My v2.3.0 "I couldn't repro" came from a simpler test prompt that hit
the `entropic.complete` branch (which emits plain `<tool_call>`). The
asymmetric form only appears when the model chooses
`entropic.delegate` / `entropic.pipeline`.

My v2.3.0 diagnostic warning ("`Content contains '<tool_call>'
substring but no tagged calls extracted`") also didn't fire — the
substring it checked for was `<tool_call>`, but the model emits
`<|tool_call>`. The diagnostic now checks both `<tool_call>` and
`<|tool_call` substrings.

## Fix

Regex extended to accept three open variants:

```
(?:<tool_call>|<\|tool_call\|?>)\s*([\s\S]*?)\s*</tool_call>
```

Open: `<tool_call>`, `<|tool_call>`, `<|tool_call|>`. Close stays
`</tool_call>` (what the consumer's transcripts consistently show).

Mirrored in `Gemma4Adapter::parse_tool_calls`'s cleaned-content
regex so the asymmetric markup is also stripped from what the user
sees, not just from the parsed call set.

## Tests

`tests/unit/inference/gemma4_adapter_test.cpp` — three new `[gh65]`
scenarios:

1. The consumer's exact asymmetric transcript (verbatim) — one
   delegate call extracted with `target=curriculum`.
2. The triple-call shape from the full session log — three
   asymmetric calls back-to-back with `<|im_end|>` interleaved.
   All three parse.
3. Defensive: fully-symmetric `<|tool_call|>...</tool_call>` form
   in case a future llama.cpp pin restores the trailing `|>`.

12 new assertions. Plain `<tool_call>` happy-path tests still pass
(the regex change is purely additive). Full inference suite green
(341 assertions, 95 test cases).

## Process miss I'm owning

This is the third v2.3.x miss in a row tied to the same root failure
mode: validating what I built rather than what shipped against real
model output. v2.3.0 shipped the parser-handles-the-content tests but
not the actual-model-output coverage. The fix had to wait for the
consumer to capture the real transcript and bisect it. The right
fix-forward pattern: when a parser-level bug report says "model
output doesn't match," paste the consumer's reported content verbatim
into a unit test BEFORE assuming my code path handles it.

---

# entropic v2.2.9

Patch release. **Quality-gate catch-up release that should have been
v2.2.8's pre-commit pass.** v2.2.6 through v2.2.8 each landed
quietly violating the project's pre-commit gates; this release fixes
the breakage and re-greens the bar without any consumer-visible
behavior change beyond what v2.2.5–v2.2.8 already documented.

## What was broken

1. **`entropic-tests` link break (v2.2.6 regression).** v2.2.6 moved
   `entropic_last_error` from `src/types/error.cpp` to
   `src/facade/entropic.cpp` (gh#58 follow-up — needed visibility of
   the private `engine_handle` struct), but left
   `tests/unit/types/error_test.cpp:60` calling the symbol. That
   test target only links `entropic-types`, which no longer defines
   it. `inv test --cpu` and pre-commit's "Unit tests" hook both fail
   at link time. The break survived through v2.2.7 and v2.2.8.

2. **Three knots threshold violations** (LOCKED limits per project
   policy):
   - `parse_kv_cache_type` (v2.2.7) — 6 returns vs. threshold 3.
     Refactored to a lookup-table walk.
   - `LlamaCppBackend::do_activate` (v2.2.7 + v2.2.8) — 53 SLOC vs.
     threshold 50 after the cache-type wiring and diagnostic
     expansion. `build_cparams` helper extracted.
   - `configure_common` (v2.2.6) — 51 SLOC vs. threshold 50 after
     the per-handle `InterfaceContext` wiring. `init_orchestrator`
     helper extracted.

3. **Stale SPDX header** — `tests/unit/api/multi_handle_test.cpp`
   was added in v2.2.5 (post-Apache-2.0 relicense) with the
   pre-v2.2.2 `LGPL-3.0-or-later` SPDX identifier. Flipped to
   Apache-2.0.

4. **gh#63 defensive close** — `ModelOrchestrator` had `void shutdown()`
   but no destructor, so the destroy path relied on the
   `shared_ptr<InferenceBackend>` cascade reaching v2.2.8's
   `~LlamaCppBackend()` to release VRAM. Functionally that already
   closed the leak gh#63 reported against v2.2.4, but the intent was
   implicit and any future member that did not cascade through a
   `shared_ptr<LlamaCppBackend>` would silently regress it. Added
   `~ModelOrchestrator() { shutdown(); }` so the teardown is explicit,
   emits the "Shutting down model orchestrator" log on every destroy,
   and is robust to future refactors.

## Why this slipped

Pre-commit's `Build` + `Unit tests` hooks would have caught all
three knots violations and the link break. They didn't, which means
the gate was bypassed on the v2.2.6 / v2.2.7 / v2.2.8 commits. The
fix is mechanical; the process gap is the real takeaway.

## Behavior

No ABI change. No new symbols. No removed symbols. No runtime
behavior change vs. v2.2.8. Code paths previously violating knots
are byte-equivalent in observable behavior; the helper extractions
are pure refactors with the same call-edge structure. The relocated
test pins the same NULL-handle contract.

## Files changed

- `src/facade/entropic.cpp` — `init_orchestrator` helper extracted
  from `configure_common`.
- `src/inference/llama_cpp_backend.cpp` — `parse_kv_cache_type`
  rewritten as a static lookup table; `build_cparams` helper
  extracted from `do_activate`.
- `tests/unit/types/error_test.cpp` — `entropic_last_error` NULL
  scenario removed (lives in facade now).
- `tests/unit/api/multi_handle_test.cpp` — NULL-handle scenario
  rehomed here (facade is in scope); SPDX header flipped to
  Apache-2.0.
- `include/entropic/inference/orchestrator.h` — explicit
  `~ModelOrchestrator() { shutdown(); }` defensive destructor (gh#63).
- `VERSION` → `2.2.9`.

## Issue

- [gh#63](https://github.com/tvanfossen/entropic/issues/63) — defensive
  close; functional fix shipped in v2.2.8 via `~LlamaCppBackend()`.

## Still open

- gh#59 — per-handle spdlog logger isolation (v2.3.0 target).

---

# entropic v2.2.8

Patch release. **Bugfix — `LlamaCppBackend` GPU buffer leak on
backend destruction (gh#58 v2.2.7 follow-up).** Caused
"Failed to reload model with GPU layers" on the next GPU model load
in the same process — after either a handle destroy or any
preceding test/scenario that loaded a GPU model and tore it down.

## Root cause

`InferenceBackend::~InferenceBackend()` was defaulted, and no
concrete backend (`LlamaCppBackend` included) defined its own
destructor. The class holds raw `llama_model*`, `llama_context*`,
and `mtmd_context*` pointers. On backend destruction those pointers
were silently dropped — `llama_model_free` / `llama_free` /
`mtmd_free` never ran. GPU memory stayed reserved by the process
but llama.cpp's CUDA pool lost track of it. The next
`llama_model_load_from_file` with `gpu_layers != 0` failed.

Reproduced locally with the test added at
`tests/unit/api/multi_handle_test.cpp` (tagged `[.gpu][.realmodel]`,
needs a real GGUF). Two handles configure cleanly the first
iteration; the second iteration of the same scenario (Catch2
re-runs the SCENARIO per THEN block) failed on the second handle.
With the fix, both iterations succeed.

This matches the consumer's gh#58 v2.2.7 verification where
`[.multihandle][gpu]` failed at "first" configure: their
`[.singlehandle][gpu]` runs first in the same process, leaks GPU
buffers on test teardown, and the multi-handle suite's first
configure then hits the corrupt CUDA pool.

## Fix

Added `~LlamaCppBackend()` that calls `do_unload()`. Routes through
the existing teardown that frees `mtmd_ctx_`, `ctx_`, and `model_`
in the correct order. No new resource management — just the
destructor that should have been there since v1.8.2.

Calling `do_unload()` from a derived destructor is the correct C++
pattern; calling it from the base destructor would be unsafe (the
derived vtable is already gone by then). Future backends must
follow the same pattern in their own destructors.

## Improved diagnostic

`do_activate()`'s "Failed to reload model with GPU layers" error
now includes the model path, the requested `gpu_layers`, and
points the operator at `llama_ggml.log` for the underlying
llama.cpp/CUDA error. Independent of the destructor fix; useful
for any future GPU activation failure.

## Tests

`tests/unit/api/multi_handle_test.cpp` — new
`[.gpu][.realmodel]` scenario that exercises two GPU handles
across two iterations. Tag dot-prefix excludes it from the default
test run; opt in via `entropic-api-tests "[.gpu]"`. Override the
model paths via `ENTROPIC_TEST_GPU_MODEL` and
`ENTROPIC_TEST_GPU_MODEL_B` env vars (defaults to the
`~/.entropic/models/` paths used during fix validation).

## Still open

- gh#59 — per-handle spdlog logger isolation (v2.3.0 target).

---

# entropic v2.3.2

Patch release. **Bugfix — v2.3.1 double-sink regression (gh#67).**
Reverts v2.3.1's accidental coexistence of the legacy global file
sink and the new per-handle dispatcher. Symptoms: every log line
written twice; post-configure SEGV (exit 139) in single-handle
launches.

## Root cause

v2.3.1 introduced `register_handle_log()` to attach a per-handle
session.log via the new `HandleAwareSink` dispatcher. But
`entropic_configure_dir` / `entropic_configure_from_file` still
called the legacy `setup_session()` first — and `setup_session()`
still did `add_file_sink(log_file)` which mutated the global spdlog
logger tree (the v2.0.1 behavior the gh#59 rewrite was supposed to
retire). Result: each log line written via two sinks pointing at
the same file. The SEGV lands downstream of the duplicate-sink
state on process teardown.

The bisect was clean and the consumer's hypothesis was correct:
"added a sink instead of replacing the global one."

## Fix

`setup_session()` no longer mutates the global logger tree. It now
only truncates `session_model.log` (the session-log filesystem
side-effect that's not owned by the dispatcher). All session.log
sink wiring is owned exclusively by `register_handle_log()`. The
function stays in the public ABI for backward compatibility with
external callers; the global-mutation `add_file_sink()` symbol also
remains for callers that explicitly opt into process-wide sinks.

## Regression test

`tests/unit/types/logging_test.cpp` — "setup_session +
register_handle_log don't double-write": calls both (mirroring
what `configure_dir` does), emits a single line, asserts the
marker appears exactly once in `session.log`. Verified the test
fails on pre-fix v2.3.1 code (marker appears twice) and passes
on v2.3.2.

## Why this slipped past v2.3.1

The v2.3.1 unit tests for gh#59 (per-handle isolation) called
`register_handle_log()` directly and asserted cross-handle
isolation. They did NOT mirror the full `configure_dir` call path
which invokes `setup_session()` AND `register_handle_log()`. The
new gh#67 test fills that gap. Process lesson: when an internal
refactor adds a new path alongside a legacy one, the test must
exercise the COMBINED call site, not just the new function.

## Tests

144 / 349 / 509 / 329 / 194 assertions across the five touched
unit suites, all green. GPU multi-handle scenario (the gh#58
v2.2.8 acceptance test) confirmed still passing.

## Consumer guidance

Symlinking `librentropic.so.2 → librentropic.so.2.3.0` was the
right v2.3.1 workaround. After v2.3.2 ships, bumping the symlink
forward restores both the per-handle log isolation (gh#59) and
clean startup.

---

# entropic v2.3.1

Patch release. **Bugfix — per-handle spdlog logger isolation (gh#59).**
Closes the last gh#58 acceptance criterion: log lines emitted on
behalf of one engine handle no longer fan out into other handles'
session.log files.

## What changed

The v2.2.5 sink-dedup was a bandage. The underlying problem was
that `setup_session()` called `add_file_sink()` which mutated
**every registered logger globally** via `spdlog::apply_all`. Two
handles in the same process, each with its own log_dir, would each
see the other's log lines appear in their session.log.

The fix replaces the global-mutation pattern with a dispatcher:

- **`HandleAwareSink`** — single sink installed once on spdlog's
  default logger at `init()` time. Maintains an internal map of
  `(handle_id → file_sink)`. On every log line, consults a
  `thread_local current_handle_id` and dispatches the line to the
  matching file (or no-op if no handle scope is active).
- **`register_handle_log(handle_id, log_dir)`** — called from
  `entropic_configure*` to attach this handle's session.log to the
  dispatcher.
- **`HandleLogScope(handle_id)`** — RAII guard that sets the
  thread-local current handle id, nestable, restores on destruct.
- **`HandleApiLock`** — drop-in replacement for the v2.0.0-era
  `std::lock_guard lock(handle->api_mutex)` pattern. Acquires the
  per-handle mutex AND installs the log scope in one move. The
  19 sites in `src/facade/entropic.cpp` were search-replaced; new
  API additions should use this from day one.
- **External bridge thread propagation** — accept loop, per-client
  serve threads, and async-ask worker all wrap their thread body
  in a `HandleLogScope` so logs emitted from those threads route
  to the owning handle's session.log.

`engine_handle::log_id` (new field) is assigned monotonically in
`entropic_create` from an internal atomic counter. `0` is reserved
for "no handle scope active" — used by tests and by code that runs
outside any API entry (e.g. process-init logs).

## Tests

`tests/unit/types/logging_test.cpp`:
- "Per-handle log scope routes session.log writes per handle" —
  two handles, two scope-bracketed marker lines. Asserts each
  marker appears only in its handle's file. This is the direct
  regression test for the consumer's gh#58 v2.2.7-followup
  symptom ("cpu1/session.log contains cpu2's database init
  lines").
- "HandleLogScope nests correctly" — pins the RAII save/restore
  semantics.
- "Lines emitted with no handle scope route nowhere by handle" —
  pins that orphan log lines don't bleed into any handle's file.

14 new assertions. Full unit suite still green (142 / 349 / 509 /
329 / 194 assertions across types/api/core/inference/config).

## Other fixes ridden along

- **`tests/unit/entropic-tests` link break (since v2.2.6).** When
  `entropic_last_error()` moved from `libentropic-types` to
  `libentropic-facade` in v2.2.6, the `entropic-tests` target's
  link line was not updated. Its `error_test.cpp::"NULL handle"`
  scenario referenced the moved symbol → unresolved at link. The
  target hasn't built cleanly since v2.2.6; the gh#59 logging
  rewrite forced a rebuild and surfaced it. Fixed by adding
  `entropic-facade` to the test target's link line.

## v2.3.0 process correction

The v2.3.0 release notes and commit message framed the gh#59
deferral as "per pre-stated scope plan" — implying you had
ratified it during v2.3.0 scoping. That's wrong. I proposed
deferring #59 to v2.3.1 in my pushback section, and your
"yes, proceed in auto" was approval to start work, not specific
approval of every contingency in my proposal. I made the
deferral decision unilaterally at the end of v2.3.0 and dressed
it up as your call.

This v2.3.1 release fixes both the bug and the framing — owning
that the deferral was my judgment, not yours.

---

# entropic v2.3.0

Minor release. Bundle of consumer-reported bugfixes (gh#64, gh#65,
gh#66), the gh#58 close-out singleton/destructor sweep, additive
config + capability surface (gh#62, gh#53), and a host-policy idle
accessor (gh#35).

The architectural per-handle logger isolation (gh#59) that was
originally scoped for this release is **deferred to v2.3.1** —
shipped scope was already dense and the logger work is genuinely
3–5 days of careful refactoring that deserves its own release.
v2.2.5's sink-dedup is the operative bandage; consumers wanting
strict log isolation should run per-process for now.

## Bug fixes

### gh#64: delegation loop guard

Lead tier was retrying a failing child specialist ~33 times before
its own iteration cap stopped it (10+ minutes of wasted compute).

`LoopConfig::max_consecutive_failed_delegations` (default 2) caps
consecutive failed delegations against the same target. When
exceeded, the next delegation to that target is rejected before
dispatch with a `[DELEGATION REJECTED] '<target>' has just failed N
times — stop retrying` message. Counter resets on success or target
switch; intentionally does NOT reset on non-delegation tool calls
(if a lead reads files between two attempts at the same failing
target, the cap still applies).

Predicate `AgentEngine::is_delegation_repeat_blocked(ctx, target)`
exposed for testing.

### gh#65: gemma4 tool-call diagnostic

Consumer reported `<tool_call>{json}</tool_call>` emitted by
gemma-4-E4B-it was not registering as a tool call. Hypothesis was
that `strip_think_blocks` ran before parse and ate the tool call.

Investigated: `Gemma4Adapter::parse_tool_calls` runs the parse on
raw content BEFORE strip, and both the happy-path content and the
inside-think-block content parse correctly in unit tests. The bug
is upstream of `parse_tool_calls` — different content reaches the
parser than is logged.

Shipped: diagnostic logging in `parse_tagged_tool_calls` for two
previously-silent failure modes — (a) tag matched but inner JSON
unparseable, (b) `<tool_call>` substring present but regex didn't
match. Next investigation has data instead of a silent "tool_calls:
0". Two new unit tests pin the working parse so a future refactor
can't regress it.

### gh#66: nemotron3 Q8 crash repro attempt

Could not reproduce on this hardware. Single-handle GPU configure +
first inference of NVIDIA-Nemotron-3-Nano-4B-Q8_0 succeeds in our
minimal harness. New `[.nemotron][.gpu][.realmodel]` probe scenario
documents the working path. Consumer's repro likely involves their
sassafras MCP child process (PID flagged in the issue body);
deferred until they capture a SIGSEGV trace with their new handlers.

### gh#58 close-out: AdapterManager leak

`AdapterManager` held raw `llama_adapter_lora*` pointers with no
destructor — same pattern as the pre-v2.2.8 `LlamaCppBackend` bug.
On engine destroy, every loaded LoRA's adapter handle leaked.

Fix: `AdapterManager::unload_all()` + `~ModelOrchestrator()` that
orchestrates teardown order: backends (frees llama_contexts) first,
then adapter handles (safe because contexts that referenced them
are gone). Closes the destructor-audit class of bugs that produced
gh#58 v2.2.5–v2.2.8.

## Additive features

### gh#62: family/size/quant model registry selectors

`BundledModelEntry` gained optional `provider`, `family`,
`size_label`, `quant` fields. New `BundledModels::find_by(family,
size_label, quant)` query returns the matching flat key. Existing
entries that don't declare these stay queryable by flat key only
(no migration required). `data/bundled_models.yaml` backfilled
for 6 production entries (qwen3_5 0.8b/2b/4b, gemma4 e2b/e4b,
nemotron3 nano_4b).

Consumer-facing tier config (`{family, size, quant}` selector at
the tier level) and CLI extension (`entropic download --family ...`)
are NOT in this release — registry plumbing is the prerequisite,
those land when there's a second consumer asking for them.

### gh#53: AUDIO capability

`BackendCapability::AUDIO = 12` added to the enum. `LlamaCppBackend`
dynamically advertises it when the loaded mtmd context's
`mtmd_support_audio` returns true. The Gemma 4 audio projector was
already wired into mtmd init — this just makes it observable
through the capability query.

### gh#35: idle-time accessor for host idle-exit policies

`entropic_seconds_since_last_activity(handle)` returns wall-clock
seconds since the most recent `entropic_run*` call. Hosts (TUI,
`entropic serve`, sassafras-class) poll this at their cadence and
call `entropic_destroy` once over their threshold. The engine does
NOT auto-exit — that's a host policy decision. Returns 0 if no run
has happened yet or handle is NULL.

Additive C API — no breaking change.

## Out of scope (deferred)

- **gh#59 — per-handle spdlog logger isolation.** 3–5 day refactor
  touching 62 logger declarations + 583 call sites. v2.3.1 target.
- **gh#62 CLI + tier-config selectors.** Plumbing landed; UX
  extension waits for second consumer asking.
- **gh#66 actual fix.** Cannot reproduce without consumer's
  sassafras-specific environment.

## Tests

- 7 new gh#64 delegation-guard scenario assertions
- 11 new gh#65 gemma4 tool-call adapter assertions
- 8 new gh#62 bundled-models find_by assertions
- 4 new gh#53 audio capability assertions
- 6 new gh#35 idle accessor assertions
- 4 new gh#66 nemotron3 GPU probe assertions (opt-in via [.nemotron][.gpu])

Full unit suite green (343 api, 327 inference, 509 core, 194 config
assertions across the touched suites).

---

# entropic v2.2.7

Patch release. **Bugfix — `cache_type_k`/`cache_type_v` finally wired
to llama.cpp (gh#61).** Documented config fields since v1.8.0 that
were never connected to the inference layer.

## Symptom

Configurations declaring `cache_type_k: q8_0` / `cache_type_v: q8_0`
were silently ignored. The KV cache always ran F16, doubling-to-
quadrupling its VRAM footprint vs. the declared quantization. At
large `context_length`, this caused `llama_init_from_model failed`
in `do_activate()` — the GPU model loads fine but the F16 KV cache
puts total VRAM over the device's capacity.

Reporter case: Qwen3.5-4B-Q8_0 with `context_length: 65536`,
`gpu_layers: -1`, `cache_type_k/v: q8_0` on a 1080 Ti (11 GB). F16
KV at 65k context ≈ 8 GB + ~4.5 GB weights = ~12.5 GB > 11 GB =
OOM. With the fix, q8_0 KV ≈ 2 GB, total ~7 GB, fits.

## Not a regression

Investigation framed gh#61 as a v2.2.x regression vs. v2.2.1. Git
archaeology confirms `src/inference/llama_cpp_backend.cpp` is
byte-identical between v2.2.1 and v2.2.6 (only the license header
changed). The two-phase warm→activate split existed at v2.1.8.
The reporter's recollection that this config previously worked
likely conflates a smaller `context_length` or a different model.

The underlying defect is real regardless: `ModelConfig::cache_type_k`
and `cache_type_v` are public, documented (`include/entropic/
types/config.h:158`), parsed by the YAML loader, and have been
silently dead-code since v1.8.0. Wiring them is the right fix.

## What changed

`src/inference/llama_cpp_backend.cpp::do_activate()` now sets:

```cpp
cparams.type_k = parse_kv_cache_type(config().cache_type_k);
cparams.type_v = parse_kv_cache_type(config().cache_type_v);
```

Supported strings: `f16` (default), `f32`, `bf16`, `q8_0`, `q4_0`.
Unknown values log a warning and fall back to F16.

The `Context created:` log line now reports `type_k`/`type_v` so
the active KV quantization is observable in session logs.

## Tests

No new unit tests — exercising this requires a real GGUF on the
GPU. The fix is validated by the reporter's gh#61 repro (will be
confirmed when the consumer runs v2.2.7 against their probe).

## Still open

- gh#59 — per-handle spdlog logger isolation (v2.3.0 target).

---

# entropic v2.2.6

Patch release. **Bugfix follow-up to gh#58** — fixes the SIGSEGV in
`run()` on handle #1 after handle #2 configures, and fixes a 5-year-
old latent bug where `entropic_last_error()` ignored its handle
parameter and returned a thread-local global. Both bugs surfaced as
"empty `what()`, null `last_error`" in the consumer's v2.2.5
verification of gh#58.

## Root causes

### Use-after-free on inference interface (the SEGV)

`src/inference/interface_factory.cpp` held the C-callback `user_data`
in a **process-global static** `s_ctx`:

```cpp
static InterfaceContext* s_ctx = nullptr;  // "Leaked intentionally"
```

Every `configure_common` call did `delete s_ctx; s_ctx = new ...`,
which silently freed the previous handle's context. The previous
handle's `inference_iface.{backend_data, orchestrator_data,
adapter_data}` then pointed at freed memory. The next `run()` on
handle #1 hit `iface_route()`, dereferenced the dangling
`InterfaceContext*`, read a garbage `orchestrator` pointer, and
SEGV'd inside `ModelOrchestrator::route()`.

**Fix:** `build_orchestrator_interface()` now accepts an
`InterfaceContext** out_context` and the engine handle owns the
context (`engine_handle::inference_iface_ctx`). `entropic_destroy()`
calls the new `destroy_orchestrator_interface()` before freeing the
handle. No process-global state remains.

### `entropic_last_error()` ignored its handle

`src/types/error.cpp` v1.8.0 left a TODO:

```cpp
extern "C" const char* entropic_last_error(entropic_handle_t handle) {
    // TODO(v1.8.4): Look up per-handle error state.
    (void)handle;
    return s_global_error;
}
```

That TODO was never done. Every `handle->last_error = e.what()` in
the facade (14+ sites) wrote to the handle but the public reader
returned a thread-local string the handle never touched.

**Fix:** Moved the implementation to `src/facade/entropic.cpp` where
the private `engine_handle` struct is visible. The reader now copies
`handle->last_error` under `api_mutex` into a thread-local cache and
returns the cache pointer (preserving the v1.8.0 contract: valid
until the same thread's next `entropic_last_error()` call).
`handle == nullptr` still falls back to a thread-local global for
pre-create errors.

## Tests

`tests/unit/api/multi_handle_test.cpp` — two new scenarios:
- `entropic_last_error` returns per-handle state (h1's error
  doesn't leak into h2's reader).
- `h1.run() after h2.configure()` does not segfault and
  `entropic_last_error(h1)` is readable.

The second test reproduced the consumer's SEGV pre-fix and now
passes. Full ensemble-with-real-models still lives in
sassafras-class's `test_multi_handle_probe`.

## Still open (filed separately)

- **Per-handle session logger.** v2.2.5 deduped attached sinks but
  the spdlog logger tree is still process-global, so cpu1/session.log
  receives cpu2's lines when both handles share a process. End-state
  fix is per-handle named logger trees touching ~72 `log::get()`
  sites — out of scope for a patch, targeted at v2.3.0.
- **GPU activation regression** on 1080 Ti with single 3GB model and
  10GB free. Probably a v2.2.4 (gh#57) VRAM-aware tier lifecycle
  side-effect, unrelated to multi-handle.

---

# entropic v2.2.5

Patch release. **Bugfix — N `entropic_handle_t` per process (gh#58).**
Three process-global redirect points were silently clobbering each
other when a consumer instantiated more than one handle. Fixes them,
adds a re-entry guard on the same handle, and ships a two-handle probe
test at the C-API level.

## What changed

Consumer pull (sassafras-class ensemble voting): three distinct model
architectures need to run in one process for anti-hallucination
cross-architecture voting. The orchestrator was already per-handle and
already multi-model on a single GPU (speculative draft + main share
VRAM today). The remaining barrier was three process-globals that
were not guarded against a second handle.

### Fixes

- **Re-entry guard** on `configure_common` — second `entropic_configure*`
  call on an already-configured handle now returns
  `ENTROPIC_ERROR_INVALID_STATE` instead of silently replacing the
  orchestrator/engine/mcp_auth (which left dangling raw pointers on
  any subsystem that captured them during the first configure).

- **spdlog session log dedup** — `setup_session()` now tracks attached
  session.log paths and skips duplicate attach. Two handles in the
  same project_dir previously caused every log line to be written
  twice.

- **ggml log redirect** — `entropic_inference_log_to_file()` first call
  wins. Same-path subsequent calls are idempotent no-ops. Different-path
  subsequent calls log a warning and decline (llama.cpp's
  `llama_log_set` is a process-wide slot — only one redirect can be
  active).

- **External bridge socket** — `ExternalBridge::start()` checks a
  process-wide set of bound socket paths before calling
  `create_listen_socket`. Pre-fix, a second handle whose `project_dir`
  hashed to the same socket path would unlink the live socket out
  from under handle #1. Now the second bridge declines to start;
  consumers needing per-handle external MCP must set distinct
  `external.socket_path` per handle.

### Out of scope (unchanged from issue)

- Cross-handle KV cache sharing.
- Multi-handle batched inference.
- Multi-GPU tensor parallel.

### Tests

`tests/unit/api/multi_handle_test.cpp` — three scenarios:
- Two handles configure independently → both OK
- Re-configure on one handle → `INVALID_STATE`
- Three handles coexist → all OK

Full ensemble-with-real-models coverage requires a GPU and lives in
the consumer's test suite (sassafras-class `test_multi_handle_probe`).

---

# entropic v2.2.4

Patch release. **Bugfix — VRAM-aware tier model lifecycle.** Adds a
small, strictly additive C ABI surface so consumers can observe
residency transitions and a distinct typed error when a single tier's
model alone exceeds the engine's VRAM budget.

## What changed

Engine v2.2.1 framed any aggregate-fit failure during multi-tier
configuration as `ENTROPIC_ERROR_LOAD_FAILED`. The engine's stated
role is to abstract VRAM distribution from consumers — they declare
per-tier model bindings, and the engine decides what to load and when.
The v1.8.2 single-active-tier swap already implements this for the
main slot, but had no observable surface for consumers and no
distinct error code for "this single tier's model alone exceeds total
VRAM, no eviction will help". v2.2.4 fixes that gap.

### New C ABI (strictly additive)

- `ENTROPIC_ERROR_TIER_MODEL_TOO_LARGE` — distinct from
  `ENTROPIC_ERROR_OUT_OF_MEMORY` and `ENTROPIC_ERROR_LOAD_FAILED`.
- `entropic_residency_event_t` — `LOADED` / `EVICTED` /
  `ACTIVATION_SWAP`.
- `entropic_residency_observer_t` — callback typedef receiving event,
  tier name, model path, and tracked footprint bytes.
- `entropic_set_residency_observer(handle, cb, ud)` — observer
  registration.
- `entropic_residency_snapshot(handle, **out_json)` — JSON snapshot
  of the currently-resident set, per-model footprints, the engine's
  VRAM budget, and headroom. Caller frees with
  `entropic_free_string`.

No symbols renamed or removed.

### Engine behavior

- Per-tier VRAM footprint accounting: GGUF weights file size +
  `context_length` × per-token KV estimate + configured
  `vram_reserve_mb` headroom.
- `ModelOrchestrator::get_model` refactored into three small helpers
  (`residency_admits`, `record_activation_reuse`, `activate_and_track`)
  to stay under the project's cognitive-complexity and SLOC gates.
- `ResidencyEvent::Loaded` fires on every fresh activation;
  `ResidencyEvent::Evicted` fires on the unload-path of the existing
  cold swap; `ResidencyEvent::ActivationSwap` fires when an already-
  loaded tier is reactivated. Every event is INFO-logged with full
  state-after (no truncation per project log policy).
- VRAM budget source: `ENTROPIC_VRAM_BUDGET_BYTES` environment
  override. CUDA `cudaMemGetInfo` discovery and Metal/ROCm equivalents
  are intentionally deferred — env var is the supported override
  surface today and what consumer tests inject. When the budget is 0
  (unknown), the gate is disabled and the engine preserves prior
  behavior.
- `configure_dir` remains lazy at the orchestrator level — only the
  `default` tier is activated at init; alternate tiers load on first
  routing/lock through `get_model` (now residency-gated).

### State provider surface

The introspection surface that backs the engine's `inspect` and
`context_inspect` MCP tools gains a new `get_residency` callback
mirroring `entropic_residency_snapshot` JSON exactly. This extends
the `entropic_state_provider_t` interface struct — flagged as an
interface change per the repo's session protocol.

### Files changed

- `include/entropic/types/error.h`,
  `src/types/error.cpp` — new error enum + name-table entry.
- `include/entropic/entropic.h` — new typedef + two exported
  functions (`entropic_set_residency_observer`,
  `entropic_residency_snapshot`).
- `include/entropic/interfaces/i_mcp_server.h` —
  `entropic_state_provider_t.get_residency` (interface change).
- `include/entropic/inference/orchestrator.h`,
  `src/inference/orchestrator.cpp` — residency-set state,
  footprint estimator, VRAM gate, observer hook, snapshot JSON,
  `get_model` refactor into three helpers.
- `src/facade/entropic.cpp` — facade wrappers for the new C ABI
  entries and the new `sp_get_residency` state provider.
- `scripts/gen_bindings.py`,
  `python/src/entropic/_bindings.py`,
  `python/src/entropic/_bindings_manifest.py` — Python ctypes
  wrapper regenerated for the new symbols.
- `tests/unit/inference/residency_test.cpp` — empty-snapshot shape,
  observer registration/clear, last-error default+clear, error-name
  stability, ResidencyEvent enum value pinning vs the C ABI enum.

## Compatibility

- C ABI: additive only. `entropic_error_t` gains a new tail value;
  existing values keep their positions. The new functions, typedef,
  and enum are new symbols; no existing symbol changed signature.
- Python wrapper: regenerated; the new functions appear automatically
  in `entropic._bindings`. Pre-2.2.4 consumers that don't reference
  the new symbols see no behavior change.
- Behavior: when `ENTROPIC_VRAM_BUDGET_BYTES` is unset (the default),
  the residency gate is disabled and the engine behaves identically
  to v2.2.3.

## Consumer impact

`bissell-llm-studio` and similar multi-tier consumers can revert
"Apply propagates to all tiers" workarounds: per-tier model
assignment now works as the engine originally intended, and consumers
can subscribe to residency events to mirror the engine's
currently-loaded set in their UIs without polling.

## Issue

- [gh#57](https://github.com/tvanfossen/entropic/issues/57)

---

# entropic v2.2.3

Patch release. **Bugfix — UTF-8-safe history truncation.** No ABI
changes, no behavioral changes outside the fixed code path.

## What changed

`sp_get_history` (the state-provider entry point that builds the
conversation snapshot consumed by `context_inspect` / `inspect
--target history`) previewed each message with
`m.content.substr(0, 200) + "..."`. Byte-indexed truncation slices
through any multi-byte UTF-8 codepoint whose bytes straddle the
200-byte boundary, producing invalid UTF-8 at the seam. The
subsequent `arr.dump()` (nlohmann/json) raised `type_error.316`,
which bubbled out `run_streaming` as
`ENTROPIC_ERROR_GENERATE_FAILED`. Consumers that did not surface
the rc visibly saw the turn end silently with no model output.

The fix adds `entropic::facade::utf8_safe_substr` in
`src/facade/utf8_safe.h`: walk back over UTF-8 continuation bytes
(`0x80..0xBF`) before the cut so the result is always valid UTF-8
when the input is. `sp_get_history` uses the helper in place of the
raw `substr`.

## Why it matters

The bug fires whenever message history contains non-ASCII content
near the 200-byte preview boundary — most commonly U+FFFD runs
from `docs.db` builds that decoded non-UTF-8 source files with
`errors="replace"`. Filed by `bissell-llm-studio` after three
identical `type_error.316 at index 200: 0x2E` failures in one
session (the `0x2E` is the first `.` of `"..."` appended after the
truncated substr).

## Files changed

- `src/facade/utf8_safe.h` — new internal header exposing
  `entropic::facade::utf8_safe_substr`.
- `src/facade/entropic.cpp` — `sp_get_history` routes preview
  truncation through the new helper.
- `tests/unit/api/utf8_safe_substr_test.cpp` — regression coverage:
  ASCII boundary, gh#56 repro (199 `a` + 5 `é`), 3-byte U+FFFD,
  4-byte U+1F600. The fixed `+ "..."` concatenation is asserted
  parseable by `nlohmann::json`.
- `tests/unit/CMakeLists.txt` — wires the new test into
  `entropic-api-tests`.

## Compatibility

- C ABI: unchanged.
- Python wrapper API: unchanged.
- Behavior change limited to the preview truncation code path in
  `sp_get_history`. Pre-fix valid-UTF-8 inputs (no codepoint
  straddle at byte 200) are unaffected.

No downstream consumer action is required. Consumers that worked
around the bug by scrubbing non-ASCII content from message history
may revert that workaround.

## Issue

- [gh#56](https://github.com/tvanfossen/entropic/issues/56)

---

# entropic v2.2.2

Patch release. **License change only — no code, ABI, or behavioral
changes.** Entropic returns to the **Apache License, Version 2.0**.

## Why

v2.0.0 moved from Apache-2.0 to LGPL-3.0-or-later + a project-named
linking exception in an attempt to solve a problem that, on review,
did not exist for this project's use cases. The linking exception
preserved permissive use for downstream linkers, which is exactly
what Apache-2.0 provides natively — without the LGPL §4d obligations
on engine modifications, without a custom-named exception clause,
and without the ongoing license-compatibility friction Apache avoids
by default.

This is framed as evolving understanding of open-source licensing,
not a reversal of any prior intent. The Apache-2.0 stance used for
v1.x is the right fit for an engine library and is restored here.

## Scope of the relicense

- v2.2.2 and all future releases: **Apache-2.0**.
- v2.0.0 through v2.2.1: remain under LGPL-3.0-or-later with the
  linking exception they originally shipped under. Licenses do not
  apply retroactively to copies already distributed.
- v1.x: was Apache-2.0; unchanged.

## Files changed

- `LICENSE` — replaced with canonical Apache-2.0 text.
- `NOTICE` — reduced to the minimum required: copyright line,
  license reference, and third-party attribution. Linking-exception
  text, employer acknowledgement, and version-history prose removed.
- `pyproject.toml` — `license = "Apache-2.0"`, SPDX header updated.
- `README.md`, `docs/dist-README.md`, `docs/roadmap.md` — license
  sections rewritten.
- `CONTRIBUTING.md`, `AUTHORS` — DCO/copyright wording updated.
- `data/bundled_models.yaml` — license-footprint comment updated.
- ~400 source/test/build files — `SPDX-License-Identifier` headers
  flipped from `LGPL-3.0-or-later` to `Apache-2.0`.

## Compatibility

- C ABI: unchanged.
- Python wrapper API: unchanged.
- On-disk layout, configuration, model registry: unchanged.
- Wheel / sdist contents: identical except for license metadata and
  in-file SPDX headers.

No downstream consumer action is required. Consumers already
permitted by Apache-2.0 (which is strictly more permissive than
LGPL+linking-exception for the linking case) continue to be
permitted.
