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

## CUDA runtime prerequisite (CUDA tarball only)

The `entropic-*-cuda.tar.gz` tarball's `librentropic.so` dynamically
links to the NVIDIA CUDA 12.8+ runtime libraries:

- `libcudart.so.12` — from the CUDA toolkit (package
  `cuda-cudart-12-8` on Ubuntu / equivalent on other distros)
- `libcublas.so.12`, `libcublasLt.so.12` — from the CUDA toolkit
- `libcuda.so.1` — from the NVIDIA driver

Install CUDA 12.8+ runtime before extracting this tarball. On
Ubuntu 24.04:

```
wget https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64/cuda-keyring_1.1-1_all.deb
sudo dpkg -i cuda-keyring_1.1-1_all.deb
sudo apt-get update
sudo apt-get install -y cuda-cudart-12-8 libcublas-12-8
```

### GPU compatibility

Native SASS kernels are baked in for every discrete NVIDIA GPU
generation from Maxwell onward:

| Arch       | Compute | Example GPUs                         |
|------------|---------|--------------------------------------|
| Maxwell    | 5.0/5.2 | GTX 9xx, Titan X (Maxwell)           |
| Pascal     | 6.0/6.1 | GTX 10-series, P40, P100             |
| Volta      | 7.0     | Titan V, V100                        |
| Turing     | 7.5     | RTX 20-series, T4                    |
| Ampere DC  | 8.0     | A100                                 |
| Ampere     | 8.6     | RTX 30-series, A40                   |
| Ada        | 8.9     | RTX 40-series, L40                   |
| Hopper     | 9.0     | H100, H200                           |
| Blackwell  | 10.0    | B100, B200                           |
| Blackwell  | 12.0    | RTX PRO 4000, RTX 50-series          |

PTX fallback is still available for Jetson/embedded variants
(sm_53/62/72/87) and any future architectures.

The CPU tarball (`entropic-*-cpu.tar.gz`) has no CUDA dependency and
only needs system libc/libstdc++/libssl/libsqlite3.

## Models

Models are **not** bundled. The engine ships with a registry
(`share/entropic/bundled_models.yaml`) of vetted GGUF files.

### Fetch a bundled model

```
entropic download --list            # show available keys + sizes
entropic download primary           # fetches to ~/.entropic/models/
```

This downloads with resume support and stores at the path the engine
discovers by default. For a custom location:

```
entropic download primary --dir /opt/models
export ENTROPIC_MODEL_DIR=/opt/models
```

### Discovery order

The engine resolves model paths in this order at load time:

1. `ENTROPIC_MODEL_DIR` (environment override, always wins)
2. Absolute path specified in config
3. `~/.entropic/models/`
4. `/opt/entropic/models/`

Any GGUF at one of those paths with a name matching a registry entry
is picked up automatically.

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
