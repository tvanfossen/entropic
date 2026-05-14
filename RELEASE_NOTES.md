# entropic v2.1.8

Patch release laying the multimodal foundation for v2.2.0. Five
issues bundle as a coherent slice: facade ABI for multimodal
messages (**gh#37**), tier-routing on `content_parts` with a new
`ENTROPIC_ERROR_NO_VISION_TIER` (**gh#41**), bundled mmproj paired
to the existing Qwen3.5-35B-A3B primary (**gh#42**), OpenAI server
example accepts `data:` / `file://` / absolute-path image content
parts (**gh#38**), and a small ride-along token-pressure getter
(**gh#39**).

> **Scope note (gh#42).** Initial design called for a separate
> Qwen2.5-VL bundle. Upstream `unsloth/Qwen3.5-35B-A3B-GGUF` is
> already tagged `image-text-to-text` and ships paired mmproj
> GGUFs. Pairing the F16 mmproj turns the existing primary tier
> into a VLM without a parallel model load (~6 GB VRAM saved) and
> preserves family alignment.

> **Deferred to a follow-up patch.** The runtime mmproj load +
> mtmd image-encoding path in `LlamaCppBackend` is part of v1.9.11
> Phases 5–7 (deferred there; not addressed here). v2.1.8 ships
> the full facade + config + routing slice; end-to-end vision
> generation lands when the inference-layer work follows. The
> facade fails fast with `ENTROPIC_ERROR_NO_VISION_TIER` for image
> content arriving at a non-vision-capable tier, so consumers get
> a clear signal rather than silent image-part drops.

## Highlights

- **gh#37 (MEDIUM):** Two new C ABI entry points —
  `entropic_run_messages` and `entropic_run_messages_streaming`.
  Accept OpenAI-style messages with mixed `text` and `image`
  content parts. `entropic_run` continues to work unchanged —
  strictly additive ABI, no JSON ser/parse on the text-only fast
  path.
- **gh#41 (MEDIUM):** Tier configs gain a `capabilities` field
  (default `[text]`). Facade preflight-checks every multimodal
  turn against `ModelOrchestrator::has_vision_capable_tier()` and
  returns `ENTROPIC_ERROR_NO_VISION_TIER` (50) when no tier
  qualifies. Tier-lock semantics preserved — consumers that lock
  to a text-only tier and then send an image get the error rather
  than a silent auto-switch.
- **gh#42 (MEDIUM):** `data/bundled_models.yaml` adds
  `primary_mmproj` (F16, ~899 MB). `BundledModelEntry` gets a
  `mmproj_key` field so `entropic download primary` fetches the
  paired mmproj automatically. `data/default_config.yaml`'s `lead`
  tier declares `capabilities: [text, vision]` and references the
  mmproj.
- **gh#38 (MEDIUM):** `examples/openai-server` accepts `image_url`
  content parts. `data:image/<mime>;base64,...` URLs decode into
  per-request tempfiles (RAII-cleaned on every exit path including
  streaming completion). `file://` and absolute paths pass
  through. `http(s)://` returns HTTP 400 with a SSRF-aware
  diagnostic.
- **gh#39 (LOW):** `entropic_context_usage(handle, *used, *cap)`
  exposes the token-pressure pair the engine already logs every
  iteration — direct replacement for the
  re-tokenize-`entropic_context_get` workaround consumers were
  using.

## Engine fix surfaced by the audit

- **Core serializer (gh#37 audit):** `ResponseGenerator::serialize_messages`
  was the structural bottleneck that would have broken multimodal
  end-to-end even after every other piece landed. It hand-rolled
  JSON emitting only `role` + `content` (string) — `content_parts`
  vanished at the engine→backend hop. Extended to emit OpenAI-style
  content arrays when `content_parts` is non-empty. Still hand-rolled
  to honor core's no-nlohmann-json rule (design decision #21).

## Breaking changes

None. All ABI additions; existing entry points unchanged.

## Distribution

- Tarball: `entropic-2.1.8-linux-x86_64-{cpu,cuda}.tar.gz` with
  matching `.sha256` files.
- PyPI: `pip install entropic-engine==2.1.8` publishes from the
  release-published trigger.

## Known limitations

- **End-to-end vision generation is gated on the inference-layer
  follow-up.** v1.9.11 Phases 5–7 (`LlamaCppBackend::do_load`
  mmproj init, `do_generate` multimodal path via libmtmd,
  text-only fallback) remain deferred. v2.1.8 wires every
  facade-side surface so the inference-layer patch is a drop-in.
- **No HTTP fetch for remote images in the OpenAI server.**
  `http(s)://` returns 400; consumers download themselves and
  resend as `data:`.

---

# entropic v2.1.7

Patch release with one architectural fix: **gh#34** — `entropic
mcp-bridge` is now a pure stdio↔unix-socket relay. Previously it
spawned a full standalone engine on every external MCP connection,
loading the model a second time into VRAM (~13–15 GB) even when a
consumer already had an engine running for the same project. The
name "bridge" implies a relay; the implementation was a hidden second
server. v2.1.7 makes the bridge match its name.

## Highlights

- **gh#34 (HIGH):** `entropic mcp-bridge` no longer creates an engine
  handle or loads a model. It is a thin byte-transparent relay
  between stdio JSON-RPC (e.g. Claude Code, IDE plugins) and a
  running engine's unix-socket bridge. If no engine is reachable for
  the project_dir, the bridge exits 1 with a diagnostic naming the
  requested path, canonical path, computed socket, and errno reason.
- **`mcp-connect` removed:** the two CLI surfaces collapse into one.
  `mcp-bridge` is the only relay subcommand. `--socket PATH` is
  retained as an override for non-standard engine deployments and
  for deterministic testing.
- **Unix-socket hardening:** SO_PEERCRED uid check on accept,
  `~/.entropic/socks/` explicit mode 0700, socket file mode 0600,
  symlink-safe + non-socket-safe bind.

## Engine bug fixes

- **gh#34 — `0163760`:** `src/cli/mcp_bridge.cpp` rewritten as a
  pure relay; no `entropic_create`, no model load, no engine handle.
  Discovery uses the same `compute_socket_path(canonical project_dir)`
  hash the engine's `ExternalBridge` publishes to, so consumer
  `.mcp.json` configs need no path tweaks. A bidirectional `poll(2)`
  loop forwards bytes in both directions — server-initiated
  `notifications/progress` and streaming responses pass through
  unmodified, fixing a latent stall in the pre-2.1.7 `mcp-connect`
  synchronous request→response loop.
- **gh#34 — `0163760`:** `src/facade/external_bridge.cpp` hardened:
  `SO_PEERCRED` uid mismatch closes the client fd before any serve
  thread is spawned; symlink or pre-existing non-socket at the bind
  path is refused with a clear log line; socket file and containing
  directory get explicit `chmod` rather than relying on umask.
- **gh#34 — `0163760`:** bind success now logs canonical project_dir
  + socket so engine and bridge log lines are symmetric — the silent
  misroute mode (consumer launches engine from a different
  canonicalized cwd than the bridge sees) is diagnosable from either
  end.

## New features

None. v2.1.7 is a behavior-correction patch.

## Breaking changes

- **`entropic mcp-bridge` semantics changed.** Pre-2.1.7 `.mcp.json`
  configs that relied on the bridge starting its own engine will now
  exit 1 with a "no running engine for `<project_dir>`" diagnostic.
  Fix: start an engine host (TUI, consumer app, or invoke the engine
  via the C/Python API) for the same `project_dir` before connecting
  the MCP client. The bridge is an *optional service* on top of an
  engine — never a substitute for one.
- **`entropic mcp-connect` subcommand removed.** The functionality is
  now the default `entropic mcp-bridge` behavior. Anywhere
  `mcp-connect --socket PATH` was used, `mcp-bridge --socket PATH`
  is the drop-in replacement.

## Distribution

- Tarball: `entropic-2.1.7-linux-x86_64-{cpu,cuda}.tar.gz` with
  matching `.sha256` files.
- PyPI: `pip install entropic-engine==2.1.7` publishes from the
  release-published trigger.

## Known limitations

- **Engine hosts (TUI, consumer apps) still have no idle-exit
  policy.** The acute VRAM-orphan case from gh#34 is fixed because
  the bridge no longer holds the model; the chronic case — a
  long-running consumer engine sitting idle with the model resident
  — remains. Tracked separately as **gh#35** (engine idle-exit
  policy).
- **Single-uid trust boundary.** The unix-socket transport is
  authenticated only by file perms + SO_PEERCRED uid match. Same-uid
  attackers with shell access can still reach the socket; this is
  the same trust posture as embedding the engine in a consumer
  process. TCP transport (if ever added) is the trigger for adding
  proper token/OAuth auth — recorded in `architecture-cpp.md`
  decision log entry #37.

## Post-release

- gh#34 closes automatically on this release (referenced in the
  commit + merge messages).
- gh#35 (engine idle-exit) and gh#36 (speculative decoding, CPU draft
  → GPU verifier) tracked as separate, unscheduled work.
