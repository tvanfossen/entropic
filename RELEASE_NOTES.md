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
