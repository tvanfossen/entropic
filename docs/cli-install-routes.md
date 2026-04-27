# `entropic` CLI — install routes and discoverability audit

> **Scope**: review note for v2.1.0. Captures where the `entropic`
> command-line lands from each consumer route, what subcommands it
> exposes, and where doc claims diverged from reality. Companion to
> `docs/getting-started.md`.

## Native binary subcommand surface

The native `entropic` C++ binary (built from `src/cli/`) exposes
exactly four subcommands:

| Subcommand    | Purpose |
|---|---|
| `mcp-bridge`  | Run the engine as a JSON-RPC MCP server over stdio (primary integration for Claude Code, VSCode, etc.) |
| `mcp-connect` | Client side: connect to a running engine's MCP bridge socket |
| `download`    | Fetch a bundled GGUF model from the registry into `~/.entropic/models/` |
| `version`     | Print engine version |

Anything else (e.g. `entropic ask "…"`) returns
`entropic: unknown subcommand 'X'` and a usage banner.

## Install routes — where the binary comes from

```
┌─────────────────────────────┬───────────────────────────────────────┐
│ Route                       │ Binary path                           │
├─────────────────────────────┼───────────────────────────────────────┤
│ Source build (contributor)  │ build/{preset}/src/cli/entropic       │
│ Tarball (find_package user) │ <prefix>/bin/entropic                 │
│ pip wrapper                 │ ~/.entropic/lib/../bin/entropic       │
│                             │ (resolved via _loader.find_bin)       │
│ inv example                 │ build/{preset}/src/cli/entropic with  │
│                             │ LD_LIBRARY_PATH to the build tree     │
└─────────────────────────────┴───────────────────────────────────────┘
```

The same binary ships across all four — `mcp-bridge` / `mcp-connect`
/ `download` / `version` work identically regardless of how the user
got it.

## Python wrapper console script

`pip install entropic-engine` installs an `entropic` console script
backed by `python/src/entropic/cli.py`. The wrapper:

- **Handles `entropic install-engine` in-process** — fetches the
  matching tarball from GitHub Releases, verifies sha256, extracts
  to `~/.entropic/`. This subcommand exists ONLY in the wrapper; the
  native binary has nothing to do with it.
- **Forwards everything else via `os.execvp`** — replaces the Python
  process with `bin/entropic <args>` from the install root. Argv is
  passed through verbatim, so `entropic version` / `entropic
  mcp-bridge` / `entropic download primary` all work the same as
  invoking the native binary directly.

Net effective surface for a wrapper user:
`install-engine` + the four native subcommands.

## Discrepancies caught by this audit

### (FIXED in this commit) `entropic ask` documented but unimplemented

`docs/getting-started.md` and `python/src/entropic/cli.py`'s
docstring both showed `entropic ask "…"` as an example. The native
binary has no `ask` subcommand — invoking it returns
`entropic: unknown subcommand 'ask'`. Both doc sites updated to use
real subcommands; the wrapper docstring's example list trimmed to
`entropic install-engine`, `entropic version`, `entropic mcp-bridge`.

### (Open) No `--where` / `--locate` introspection

A user who has both `pip install entropic-engine` AND a tarball
extracted somewhere on PATH may end up running a different binary
than they expect. There is currently no `entropic --where` or
`entropic locate` option that prints the resolved binary + library
path. **Recommendation**: add a `where` subcommand for v2.1.x or
v2.2 that prints:

```
binary:  /home/user/.entropic/entropic/bin/entropic
library: /home/user/.entropic/entropic/lib/librentropic.so
version: 2.1.0
```

Tracked separately if pursued.

### (Open) Wrapper version vs binary version drift

`pip install entropic-engine==2.1.0` followed by `entropic
install-engine` always fetches the v2.1.0 tarball (1:1 version
coupling per `python/src/entropic/install_engine.py`). But a user
can `entropic install-engine --version 2.0.5` to override, after
which the wrapper (2.1.0) is calling a binary (2.0.5) — possibly
with ABI incompatibility on direct ctypes use. Today this is
caller-beware. **Recommendation**: warn at `install-engine` time if
`--version` differs from `entropic.__version__`. Tracked separately.

## Help-text consistency

The native binary's `print_usage()` lists subcommands. The wrapper's
`cli.py` does NOT have its own help — `entropic --help` execs to the
native binary, which then prints its own usage. So `entropic --help`
shows the native binary's surface, NOT `install-engine` (which is
wrapper-only). **Recommendation**: have the wrapper intercept
`--help` / `-h` and prepend an `install-engine` line before
delegating. Tracked separately.

## Test coverage

`tests/distribution-smoke-consumer/` exercises the
`find_package(entropic)` consumer flow + a `<prefix>/bin/entropic
version` launch (RPATH check). Wrapper-side: smoke-tested manually
in #B2 implementation; no automated end-to-end test from
`pip install` → `entropic install-engine` → `entropic version`
because that requires network access to GitHub Releases. Acceptable
for v2.1.0; revisit if a published release fails the flow.
