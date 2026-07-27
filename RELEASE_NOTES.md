_Last 10 releases. Older history: [OLD_NOTES.md](OLD_NOTES.md). Kept short
because `gh release create --notes-file` hits GitHub's 125,000-char release
body limit once this file accumulates full project history — see v2.9.3._

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

# entropic v2.10.1

Patch release — **the MCP server plugin loader `i_mcp_server.h` has documented
since v1.8.5 now actually exists (gh#133).**

## The gap

`include/entropic/interfaces/i_mcp_server.h` stated "ServerManager discovers
plugins via dlopen and calls these functions through the opaque handle." No
such loader existed. A consumer who implemented the documented nine-entry-point
contract produced a `.so` that nothing in the engine could load — reachable
only by disassembling the shipped binary, since the headers said the opposite.

Reported by the sassafras-class consumer, who had a conformant implementation
written and tested against the contract before establishing it was unloadable.

## Loading a plugin

```yaml
mcp:
  plugins:
    - /path/to/libmy_mcp_server.so
    - ~/plugins/libother_server.so
```

Each entry is dlopened at startup, version-checked against
`ENTROPIC_MCP_PLUGIN_API_VERSION`, and registered under the name its
`entropic_mcp_server_name()` reports. Its tools are then addressable as
`<name>.<tool>` exactly like a built-in server's, including argument
validation against the plugin's declared `inputSchema`.

Failures are loud, never silent: a `.so` that will not open, is missing an
entry point, reports a different API version, or collides with an existing
server name is rejected with `ENTROPIC_ERROR_PLUGIN_LOAD_FAILED` /
`ENTROPIC_ERROR_PLUGIN_VERSION_MISMATCH`. Every configured path is attempted
so one broken entry does not hide the diagnosis of the rest.

## Header corrections

- **`ENTROPIC_EXPORT` on all nine entry points.** Previously absent, so a
  plugin defining them the obvious way — plain `extern "C"`, inheriting
  visibility from the header — exported *nothing* under `-fvisibility=hidden`,
  the way most plugin projects build. Verified on GCC 11.4: 0 symbols exported
  before, all 9 after. Visibility only; no signature or ABI change, so no
  plugin-API version bump, and plugins that exported by other means still load
  unchanged.
- **`entropic_plugin_api_version()` and `entropic_create_server()` are now real
  declarations** rather than prose in a comment block.
- **Threading contract documented**: the engine serialises calls into a given
  server instance, so a plugin needs no internal locking for its own state.
  `PluginServer` takes its own mutex, making that guarantee hold by
  construction rather than by assumption about callers.
- **`inputSchema` camelCase** stated explicitly.

## Notes for plugin authors

Plugins are loaded `RTLD_LOCAL`, so two plugins exporting the same entry-point
names cannot collide. Strings returned by `list_tools`/`execute` are freed
through *that plugin's* `entropic_free`, not the engine's allocator. A plugin
returning a malformed tool list or response is contained to itself — it does
not throw through the agent loop or empty the tool list for other servers.

# entropic v2.10.0

Minor release — **MTP grammar + streaming support, tool-call robustness, and
filesystem/pipeline polish.**

## Highlights

- **MTP grammar (gh#108)**: tiers with `speculative.mtp: true` and a static
  GBNF grammar now work correctly. `to_common_sampling` propagates
  `params.grammar` to the MTP sampler chain; the loader rejection and the
  orchestrator routing gate are removed.
- **MTP streaming (gh#108)**: `speculative.mtp: true` is now compatible with
  streaming calls. `generate_streaming` wraps `on_token` with `StreamThinkFilter`
  for incremental thinking-channel stripping, and calls `apply_adapter_parse` on
  return — matching the non-streaming path.
- **MTP head guard (gh#107)**: using a Gemma-4 MTP head GGUF on the classical
  separate-draft path now fails loud with `INCOMPATIBLE_CONFIG` instead of
  crashing in `fattn.cu`. Message names `speculative.mtp: true` as the fix.
- **Lenient tool-call parse (gh#127)**: fenced JSON blocks containing only an
  arguments object (no `name` key) are now matched against registered tool
  schemas and synthesized into a `ToolCall` when exactly one schema matches.
- **Pipeline stage validation (gh#129)**: `PipelineTool` rejects unknown stage
  names at emission time with an `invalid_stage` error, instead of silently
  passing them to `DelegationManager` and failing per-stage.
- **Per-stage pipeline output (gh#125)**: pipeline context messages now include
  per-stage `{tier, task}` summaries in addition to the final result.
- **`read_file` guidance (gh#124)**: not-found errors now name `list_directory`
  as the corrective action.
- **`glob` path matching (gh#126)**: `**/*.cpp` and similar patterns now match
  root-level files and path-relative entries; `**` maps to `.*` (cross-directory)
  while bare `*` maps to `[^/]*` (single segment).
- **UTF-8 safety (gh#132)**: `CompleteTool::execute`, `serialize_batch_results`,
  and `entropic_validation_last_result` sanitize output before JSON serialization.

## Engine bug fixes

- gh#132: `type_error.316` on malformed model output in `CompleteTool::execute`
- gh#127: tool-call lost when model emits arguments-only fence (no `name` key)
- gh#129: silent per-stage failure on unknown tier names in `pipeline` tool
- gh#126: `glob("**/*.cpp")` returned nothing for root-level and path-relative files
- gh#124: `read_file` not-found error provided no recovery guidance
- gh#107: crash (`GGML_ABORT` in `fattn.cu`) when MTP head GGUF routed to classical draft path
- gh#108: MTP sampler did not enforce GBNF grammar constraints
- gh#108: MTP streaming emitted raw `<think>` tokens and skipped `apply_adapter_parse`

## New features

- gh#125: pipeline output includes per-stage tier + task summary
- gh#107: `looks_like_mtp_head(n_layer)` + `mtp_head_classical_path_error` in `mtp_envelope.h`

## Breaking changes

- Loader no longer rejects `speculative.mtp: true` + static grammar combination
  (was: validation error at parse time). Existing configs that relied on this
  gate as a safety net may now route to MTP with grammar applied.
- `mtp_unsupported_reason` always returns `""` — all three guards (temperature,
  grammar, streaming) are removed. Direct callers asserting non-empty for any
  condition should update their tests.

## Distribution

- CPU tarball: `entropic-2.10.0-linux-x86_64-cpu.tar.gz` (sha256 in companion file)
- CUDA tarball: `entropic-2.10.0-linux-x86_64-cuda.tar.gz` (sha256 in companion file)
- Python wrapper: `pip install entropic-engine==2.10.0` then `entropic install-engine`

# entropic v2.9.8

Patch — **completes the gh#111 UTF-8 fix that v2.9.7 left half-done.**
`entropic_run` still threw `nlohmann::json::type_error 316` mid-turn in a
lead→delegate turn under MTP, at the exact site named (but not patched) in the
v2.9.7 notes: `fire_delegate_complete_hook`'s `j.dump()` on a raw child summary.

## Why v2.9.7 missed it

v2.9.7 sanitized the hook-plugin *return* boundaries (`fire_post_generate_hook`,
`fire_complete_hook`, `fire_post_tool_hook`) — but only on the branch where a
plugin **revises** content (`out != nullptr`). On the headless path (no
content-revising `POST_GENERATE` hook) that branch never runs, so the raw
summary sailed straight into the dump.

Root cause: the summary reaches `fire_delegate_complete_hook` via the child's
**last-assistant-content fallback** in `extract_summary`, not the tool-arg path.
That content comes from `AgentEngine::parse_tool_calls`, whose backend callback
re-derives `*cleaned` / `*tool_calls_json` from the model's **raw** generation —
a channel entirely separate from the content sanitize at
`response_generator.cpp:470`. A split multi-byte UTF-8 codepoint (routine under
MTP speculative decode, when a character splits across the draft/target token
boundary) therefore survives into the message and, downstream, into the
delegate-complete hook's `j.dump()`.

## The fix (one boundary, not scattered sinks)

Sanitize **both** outputs of the tool-call parse channel at the single seam
where they cross into engine-owned state — `AgentEngine::parse_tool_calls`
(`src/core/engine.cpp`):

```cpp
std::string cleaned_str = mcp::sanitize_utf8(cleaned ? cleaned : raw_content);
std::string tc_str      = mcp::sanitize_utf8(tc_json ? tc_json : "[]");
```

This is the tool-call-channel sibling of the existing content sanitize. It
closes every downstream `json::dump()` at once: the assistant message /
delegation-summary fallback (`cleaned_str`) and the tool-call args
(`tc_str` → `CompleteTool` / directive JSON). Documented in the boundary-policy
table in `include/entropic/mcp/utf8_sanitize.h`.

Secondary benefit: `tc_str` sanitize also stops MTP from **silently dropping** a
tool call — a raw arg previously failed `nlohmann::json::parse` and the model's
directive (e.g. `entropic.complete`) was discarded.

## Tests (red-first)

Added to `tests/unit/core/engine_test.cpp`, each proven to FAIL on the
unmodified v2.9.7 code and PASS with the fix:
- **Delegation reproduction** — drives a real lead→child delegation whose child
  produces raw content; without the fix this throws
  `type_error.316 ... byte at index 9: 0x28` out of `fire_delegate_complete_hook`
  (the exact reported crash).
- **Content channel** — a backend parse returning raw cleaned content is
  sanitized before it becomes a message.
- **Tool-call survival** — a raw-arg tool call is preserved and dispatched, not
  silently dropped.

Also fixes `tasks.py`'s model-test runner to honor each test's CMake `TIMEOUT`
(carried from the develop branch; was a source of false model-test failures).

No `interfaces/i_*.h` touched.

---

# entropic v2.9.7

Patch — **UTF-8 sanitize gap at the hook-plugin return boundary** (gh#3
recurrence, gh#111). `entropic_run()` could throw `nlohmann::json::type_error
316` mid-agentic-turn in a lead→researcher delegation, immediately after
generation completed.

## The bug

The v2.1.1 fix for gh#3 established a boundary-of-ownership UTF-8 sanitize
policy covering four boundaries: MCP tool-result inbound, llama.cpp stream
inbound, audit-log inbound, and C-API outbound. It missed a class of
boundary: **a hook plugin's returned content crossing back into the
engine.** Three call sites accepted a hook's output verbatim, with no
sanitize call before the bytes could re-enter engine state and later reach
an unguarded `nlohmann::json::dump()`:

- `fire_post_generate_hook` (`src/core/engine.cpp`) — POST_GENERATE hook
  revision. In a delegation, unsanitized content here became the child
  loop's summary, which `fire_delegate_complete_hook` dumps directly.
- `fire_complete_hook` (`src/core/engine.cpp`) — ON_COMPLETE hook feedback,
  injected into a `Message`.
- `ToolExecutor::fire_post_tool_hook` (`src/mcp/tool_executor.cpp`) —
  POST_TOOL_CALL hook transform, applied to the tool-result `Message`.

v2.9.6/gh#110 made MTP reachable from the agent loop's *batch* dispatch path
for the first time — exactly the path (`generate_batch` →
`fire_post_generate_hook` → delegation summary) that exercises the first
gap, which is why the recurrence surfaced now rather than earlier.

## The fix

- All three call sites now sanitize a hook's returned bytes via
  `mcp::sanitize_utf8` before they re-enter engine state, matching the
  treatment already given to MCP tool results.
- Documented the hook-plugin boundary in
  `include/entropic/mcp/utf8_sanitize.h`'s policy table; corrected prior text
  that incorrectly listed hook contexts as "interior/trusted."
- Added regression coverage in `tests/unit/core/engine_test.cpp` and
  `tests/unit/mcp/tool_executor_test.cpp` exercising all three hook points
  with malformed UTF-8, asserting the sanitized content JSON-dumps without
  throwing.

## Deferred

`src/storage/backend.cpp`'s SQLite message-load path reads `content` off the
column with no sanitize before a later `.dump()` — same class of gap as the
(already-fixed) audit-replay path, for the SQLite backend. Not the confirmed
root cause of this crash; fixing it cleanly needs `entropic-storage` to gain
access to the sanitizer (currently only linked into `entropic-core`). Tracked
separately, not blocking this release.

---

# entropic v2.9.6

Patch — **MTP/speculative decoding is now reachable through the agent loop**
(gh#110). v2.9.0–v2.9.4 proved MTP correct and fast when the orchestrator is
called directly, but every agent-loop turn (`entropic_run` and friends) with
`speculative.mtp` enabled failed loud — the kernel never ran.

## The bug (two independent gates)

1. `build_loop_config()` hardcoded `LoopConfig::stream_output = true`, so the
   agent loop always streamed. The streaming path unconditionally binds a
   non-empty `on_token` callback, and `LlamaCppBackend::mtp_guard` derives its
   "is this a streaming call" check as `static_cast<bool>(on_token)` — a bound
   callback is indistinguishable from "this is streaming," so every agent-loop
   MTP call tripped `mtp_unsupported_reason`'s streaming rejection and
   returned `ENTROPIC_ERROR_SPECULATIVE_INCOMPATIBLE_CONFIG`, every time.
2. Even with streaming disabled, the batch path's cancel-aware bridge
   (`inference_.generate_cancellable`, always wired in production) calls an
   orchestrator overload that deliberately bypasses `run_generate_dispatch` —
   batch-with-cancel only ever ran plain decode, never speculative.

Existing MTP tests never caught this because they call
`orchestrator->generate()` directly — shaped like the agent loop's traffic,
but never actually routed through `AgentEngine`/`ResponseGenerator`/the
facade.

## The fix

- New `generation.stream_output` config key (default `true`, no behavior
  change for existing consumers) threads through `build_loop_config()`,
  making batch mode reachable from config.
- `dispatch_batch_generate` now prefers the dispatching (non-cancellable)
  `generate` entry point over the cancel-aware one whenever speculative
  decoding is enabled, so the batch path actually reaches
  `run_generate_dispatch` → MTP. v1 tradeoff, documented not hidden: a
  speculative batch turn is not cancellable mid-decode.

To use MTP from the agent loop: set `generation.stream_output: false` +
`inference.speculative.{enabled,mtp}: true`.

## Tests

- `test_gh110_mtp_agent_loop.cpp` — drives the real `entropic_create` →
  `entropic_configure_dir` → `entropic_run` path (not a direct orchestrator
  call) and asserts on the backend's own `"Speculative: generated=..."` log
  line, the only MTP-engagement signal that crosses the C-ABI boundary.
  Verified on real hardware (RTX PRO 4000 Blackwell, gemma-4-E2B-it-Q8_0 +
  MTP head): the kernel engaged across multiple turns of the same
  conversation (`accept_rate` 0.08–0.14).

No `interfaces/i_*.h` touched.

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
