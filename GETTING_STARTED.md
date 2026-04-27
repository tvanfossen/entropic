# Getting Started with entropic

This guide covers two consumer paths: linking against the C/C++ library
directly, and using the Python wrapper. Both paths start from the same
GitHub release artifact — pick whichever fits your project.

> **Looking for the architecture overview?** Read `docs/architecture-cpp.md`.
> **Looking for the version roadmap?** Read `docs/roadmap.md`.

---

## Prerequisites

- **Linux x86_64** (Ubuntu 22.04+ tested; other distros likely fine)
- **CMake ≥ 3.21**
- **A GGUF model file** — anything `llama.cpp` accepts. The default
  registry resolves the alias `primary` to a Qwen3.5-35B IQ3_XXS quant
  (13 GB, ~15 GB VRAM). Smaller models work for tighter envelopes.
- **CUDA toolkit** (optional) — required only for the GPU tarball. CPU
  builds run on any modern x86_64.

---

## Path 1 — C / C++ direct usage

### 1. Download a release

```bash
VERSION=2.1.0
BACKEND=cpu     # or: cuda
curl -L -O https://github.com/tvanfossen/entropic/releases/download/v${VERSION}/entropic-${VERSION}-linux-x86_64-${BACKEND}.tar.gz
curl -L -O https://github.com/tvanfossen/entropic/releases/download/v${VERSION}/entropic-${VERSION}-linux-x86_64-${BACKEND}.tar.gz.sha256
sha256sum -c entropic-${VERSION}-linux-x86_64-${BACKEND}.tar.gz.sha256
```

Extract anywhere on disk:

```bash
mkdir -p ~/.local && tar -C ~/.local -xzf entropic-${VERSION}-linux-x86_64-${BACKEND}.tar.gz
# Tarball contents extract to ~/.local/entropic/{bin,lib,include,share}
```

### 2. Consume from CMake

```cmake
cmake_minimum_required(VERSION 3.21)
project(my-app LANGUAGES CXX)

# Point CMake at the install prefix from step 1.
list(APPEND CMAKE_PREFIX_PATH "$ENV{HOME}/.local/entropic")

find_package(entropic 2.1 REQUIRED)

add_executable(my-app main.cpp)
target_link_libraries(my-app PRIVATE entropic::entropic)
```

`find_package(entropic 2.1 REQUIRED)` resolves to the package config at
`<prefix>/lib/cmake/entropic/entropic-config.cmake`. The imported target
`entropic::entropic` carries the include directories, the SONAME, and
all transitive system-library link flags.

### 3. Minimal C example

```c
#include <entropic/entropic.h>
#include <stdio.h>

static void on_token(const char* tok, size_t /*n*/, void* /*ud*/) {
    fputs(tok, stdout);
    fflush(stdout);
}

int main(void) {
    entropic_handle_t h = NULL;
    if (entropic_create(&h) != ENTROPIC_OK) { return 1; }

    // Layered config: compiled defaults, then ~/.entropic/config.yaml,
    // then ./config.local.yaml, then ENTROPIC_* env vars. Pass "" to
    // skip the project-local layer entirely.
    if (entropic_configure_dir(h, "") != ENTROPIC_OK) {
        entropic_destroy(h);
        return 1;
    }

    entropic_run_streaming(h, "What is 2 + 2?", on_token, NULL, NULL);
    fputc('\n', stdout);

    entropic_destroy(h);
    return 0;
}
```

Build with the CMake snippet from step 2, then run. The first call
loads the model registered as `primary` (configurable via
`~/.entropic/config.yaml`).

For a fuller C example, see `examples/headless/main.c`.
For a multi-tier C++ example with grammar + MCP wiring, see
`examples/pychess/`.

---

## Path 2 — Python wrapper

The wrapper is a thin pure-Python ctypes shim plus an `install-engine`
subcommand that fetches the matching tarball from GitHub Releases.

> **Status:** the Python wrapper is shipping in v2.1.0 alongside the
> C library. Earlier versions of `entropic-engine` on PyPI shipped a
> separate Python engine — that namespace is being repurposed.

### 1. Install the wrapper

```bash
pip install entropic-engine==2.1.0
```

This installs ~50 KB of Python: a CLI entry point, a `ctypes` binding
module generated from `entropic.h`, and the install-engine subcommand.
No native code is included in the wheel.

### 2. Install the engine

```bash
entropic install-engine
```

The subcommand detects whether `nvidia-smi` is present, downloads the
matching tarball (`cpu` or `cuda`) for the wrapper version, verifies
`.sha256`, and extracts to `~/.entropic/lib/`. Re-running is idempotent —
matching checksums short-circuit the download.

Override the install location via `$ENTROPIC_HOME` (writes to
`$ENTROPIC_HOME/lib/`), or point at an arbitrary `librentropic.so`
via `$ENTROPIC_LIB`.

### 3. Use it

```bash
entropic ask "What is 2 + 2?"
```

`entropic ask` dispatches to `bin/entropic` from the installed tarball
via `os.execvp` — there is no Python wrapper around the conversation
loop.

For programmatic use:

```python
from entropic import (
    entropic_create, entropic_configure_dir,
    entropic_run_streaming, entropic_destroy,
)
import ctypes

handle = ctypes.c_void_p()
entropic_create(ctypes.byref(handle))
entropic_configure_dir(handle, b"")

@ctypes.CFUNCTYPE(None, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p)
def on_token(tok, n, ud):
    print(tok.decode("utf-8", errors="replace"), end="", flush=True)

entropic_run_streaming(handle, b"What is 2 + 2?", on_token, None, None)
entropic_destroy(handle)
```

The Python surface is the C ABI verbatim. There is no `EntropicEngine`
class — the v1.7.x OOP wrapper has been retired.

---

## Next steps

- `examples/headless/` — C-only minimal harness suitable for CI smoke testing.
- `examples/pychess/` — C++ example exercising tier delegation, GBNF grammar,
  and MCP tool registration. Best showcase of the v2.x feature set.
- `examples/explorer/` — interactive C++ REPL for poking at the engine.
- `docs/architecture-cpp.md` — library decomposition, design rules,
  decision log.
- `docs/roadmap.md` — version-by-version feature targeting.

If the engine misbehaves: enable trace logging via
`ENTROPIC_LOG_LEVEL=trace`, or set `[logging] level: trace` in
`~/.entropic/config.yaml`. Bug reports go to
<https://github.com/tvanfossen/entropic/issues>.
