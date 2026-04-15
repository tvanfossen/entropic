# Entropic Inference Engine

Local-first LLM inference engine with built-in MCP tool support. This
package contains a single shared library (`librentropic.so`), the
`entropic` CLI, public headers, and runtime data.

---

## Install

### Recommended: one-line installer

Auto-detects CUDA vs CPU by probing for `nvidia-smi`:

```
curl -fsSL https://raw.githubusercontent.com/tvanfossen/entropic/main/install.sh | bash
```

Flags: `--cpu` / `--cuda` to force a backend, `--prefix DIR` to pick a
non-standard install location (default `/usr/local`), `--version vX.Y.Z`
to pin, `--yes` to skip the confirmation prompt.

### Manual

```
tar -xzf entropic-<version>-linux-x86_64-<backend>.tar.gz -C /usr/local --strip-components=1
```

`--strip-components=1` maps the tarball's top-level `entropic/` into
`/usr/local/{bin,lib,share,include}` — the standard Unix layout that
`find_package` and `ld` search by default.

### Python

```
pip install entropic-engine
```

Ships the same `librentropic.so` as the CPU tarball, bundled inside
the wheel. One binary, two channels. CUDA users: use the installer
above — there is no CUDA wheel on PyPI at this time.

### Layout

After install (relative to the prefix):

```
bin/entropic                          CLI
include/entropic/entropic.h           Public C API
lib/librentropic.so                   Shared library (SOVERSION-linked)
lib/cmake/entropic/                   find_package support
share/entropic/                       Runtime data (models.yaml, prompts, grammars, tools)
share/doc/entropic/LICENSE            LGPL-3.0
share/doc/entropic/README.md          This file
```

---

## Models

Models are **not** bundled. The registry (`share/entropic/bundled_models.yaml`)
records filenames; point the engine at a directory holding those files:

```
export ENTROPIC_MODEL_DIR=/path/to/your/gguf/dir
```

Discovery order:

1. `ENTROPIC_MODEL_DIR` (environment)
2. Absolute path specified in config
3. `~/.entropic/models/`
4. `/opt/entropic/models/`

Download the GGUF files listed in `bundled_models.yaml`, drop them in
one of those directories, and the engine finds them.

---

## Link against entropic (C++ consumer)

```cmake
cmake_minimum_required(VERSION 3.21)
project(my-app CXX)

find_package(entropic 2.0 REQUIRED)

add_executable(my-app src/main.cpp)
target_link_libraries(my-app PRIVATE entropic::entropic)
```

No manual include paths, no `LD_LIBRARY_PATH`. If CMake can't find the
package automatically, pass `-Dentropic_DIR=/usr/local/lib/cmake/entropic`.

---

## Python consumers

`pip install entropic-engine` (PyPI) installs the same library bundled
inside a wheel, plus the `entropic` CLI on `PATH`. This tar.gz is
intended for C++ consumers and for system-wide CLI installs where pip
isn't appropriate.

---

## Licensing

Entropic is **LGPL-3.0-or-later**. See `share/doc/entropic/LICENSE`.

Dynamic linking against `librentropic.so` imposes minimal obligations
on your application: you must allow end users to replace the entropic
library (this is automatic when linking against the shared `.so`) and
include the LGPL notice with your distribution. Your own source code
does not need to be open-sourced.

Bundled inference runtime (`llama.cpp`) is **MIT-licensed**; see its
upstream repository for attribution text.
