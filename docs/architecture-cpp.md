# Entropic 2.0.0 — C++ Architecture Design

Reference architecture for the C++ engine rewrite. This document defines
the library decomposition, dependency graph, interface contracts, class
hierarchy patterns, plugin architecture, and build configuration that
guide the v1.8.x port.

The Python codebase (v1.7.x) is the behavioral specification. This document
defines HOW to implement it in C++, not WHAT to implement.

---

## Library Decomposition

The engine is a set of shared libraries with explicit dependency boundaries
and explicit interface contracts. Each library has a single responsibility.
Arrows in the dependency graph point down only — no circular dependencies.

```
                    ┌─────────────────┐
                    │  librentropic   │  Facade — unified C API
                    │     (.so)       │  (entropic.h)
                    └───────┬─────────┘
                            │ links all
       ┌──────────┬─────────┼──────────┬──────────┐
       ▼          ▼         ▼          ▼          ▼
 ┌──────────┐ ┌──────┐ ┌───────┐ ┌──────┐ ┌────────┐
 │inference │ │ mcp  │ │ core  │ │config│ │storage │
 │  (.so)   │ │(.so) │ │ (.so) │ │(.so) │ │ (.so)  │
 └────┬─────┘ └──┬───┘ └───┬───┘ └──┬───┘ └───┬────┘
      │          │         │        │         │
      │          │    ┌────┴────┐   │         │
      │          │    │  types  │   │         │
      │          │    │  (.so)  │   │         │
      │          │    └─────────┘   │         │
      ▼          ▼                  ▼         ▼
 llama.cpp  nlohmann/json        ryml      sqlite3
            cpp-httplib                    spdlog
```

### librentropic-types.so

Pure types with zero logic. The universal dependency that every other
library links against.

- Message, ToolCall, Directive, GenerationResult
- Config structs (ModelConfig, TierConfig, etc.)
- Enums (ModelState, AgentState, DirectiveType)
- Error types

### librentropic-core.so

Engine loop, state machine, context management, delegation, directive
processing. The operational heart of the engine.

Links: `librentropic-types`

**Zero dependencies** on inference, MCP, storage, or config. Communicates
with other libraries exclusively through interface contracts.

### librentropic-inference-{cuda,vulkan,cpu}.so

Inference backend implementations. Each is a separate build of the same
source with different compile flags (except AXCL which is different source).

Links: `librentropic-types`, `llama.cpp`

Implements: `IInferenceBackend` interface contract.

### librentropic-mcp.so

Server base class, tool registry, tool executor, permission manager,
transport layer (stdio/SSE for external servers).

Links: `librentropic-types`, `nlohmann/json`, `cpp-httplib`

Implements: `IMCPServer` interface contract.

### librentropic-mcp-{filesystem,bash,git,diagnostics,web,entropic}.so

Individual MCP server plugins. Each is a standalone `.so` loaded at runtime.
A consumer ships only the servers they need.

Links: `librentropic-mcp` (for `MCPServer` base class)

Implements: `entropic_create_server()` factory export.

### librentropic-config.so

YAML config loader, schema validation, prompt manager, identity/constitution
loading, bundled models registry.

Links: `librentropic-types`, `ryml`

Implements: `IConfigLoader` interface contract.

### librentropic-storage.so

SQLite conversation persistence, structured logging, audit log.

Links: `librentropic-types`, `sqlite3`, `spdlog`

Implements: `IStorageBackend` interface contract.

### librentropic.so (Facade)

Thin layer linking all libraries. Exposes `entropic.h` — the unified C API
that most consumers link against.

---

## Interface Contracts

Each `.so` boundary is governed by an explicit interface header in
`include/entropic/interfaces/`. These are **pure C** at the plugin boundary.
C++ interfaces are used ONLY within a single compilation unit / library.

### The Rule

**All cross-`.so` communication uses C types only.** No `std::string`,
`std::vector`, `std::optional`, or C++ vtables cross a shared library
boundary. Factory functions return opaque handles. Method calls go through
the C API or through C function pointers.

This prevents ABI incompatibility between different compilers, standard
library versions, or C++ standard revisions.

### Contract Headers

```
include/entropic/
├── interfaces/                        Contract headers (.so API surface)
│   ├── i_inference_backend.h          Pure C factory + handle interface
│   ├── i_mcp_server.h                 Pure C factory + handle interface
│   ├── i_storage_backend.h            Pure C factory + handle interface
│   ├── i_config_loader.h              Pure C factory + handle interface
│   └── i_hook_handler.h               C function pointer callback types
├── types/                             Shared types (librentropic-types.so)
│   ├── message.h
│   ├── tool_call.h
│   ├── directive.h
│   ├── generation_result.h
│   └── config.h
├── core/                              Engine internals (librentropic-core.so)
├── inference/                         Backend impl (inference .so internal)
├── mcp/                               MCP impl (mcp .so internal)
└── entropic.h                         Public C API facade
```

### Plugin ABI Versioning

Every plugin `.so` must export an API version function:

```c
extern "C" int entropic_plugin_api_version();
```

The loader checks this before calling the factory. If the version doesn't
match the engine's expected version, the plugin is rejected with a clear
error message. New interface capabilities are added via new version numbers,
not by modifying existing interfaces.

### Symbol Visibility

All `.so` files use explicit symbol visibility. Only factory functions
and the C API are exported. Everything else is hidden.

```cpp
#if defined(_WIN32)
  #define ENTROPIC_EXPORT __declspec(dllexport)
#else
  #define ENTROPIC_EXPORT __attribute__((visibility("default")))
#endif
```

Applied only to:
- Factory functions (`entropic_create_server`, `entropic_create_inference_backend`)
- C API functions in `entropic.h`
- `entropic_plugin_api_version()`

### No Third-Party Headers in Interfaces

Interface headers must not include third-party library headers. The `json`
type from nlohmann/json, `YAML::Node` from ryml, etc. are implementation
details that do not appear in any header under `interfaces/` or `types/`.

Cross-boundary data uses C types: `const char*`, `size_t`, opaque handles.

### Example: Plugin C Interface

```c
// include/entropic/interfaces/i_mcp_server.h

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Opaque handle to an MCP server instance.
typedef struct entropic_mcp_server* entropic_mcp_server_t;

/// @brief Get the server name.
/// @param server Server handle.
/// @return Null-terminated server name string. Owned by the server.
const char* entropic_mcp_server_name(entropic_mcp_server_t server);

/// @brief List tools as JSON array string.
/// @param server Server handle.
/// @return JSON string of tool definitions. Caller must free with entropic_free().
char* entropic_mcp_server_list_tools(entropic_mcp_server_t server);

/// @brief Execute a tool.
/// @param server Server handle.
/// @param tool_name Tool name (without server prefix).
/// @param args_json JSON string of arguments.
/// @return JSON string of result. Caller must free with entropic_free().
char* entropic_mcp_server_execute(
    entropic_mcp_server_t server,
    const char* tool_name,
    const char* args_json);

/// @brief Destroy a server instance.
/// @param server Server handle to destroy.
void entropic_mcp_server_destroy(entropic_mcp_server_t server);

/// @brief Free a string allocated by the server.
/// @param ptr Pointer returned by list_tools or execute.
void entropic_free(void* ptr);

#ifdef __cplusplus
}
#endif

// ── Plugin export requirements ──────────────────────────

// Every MCP server .so must export these two C functions:
//
//   extern "C" ENTROPIC_EXPORT int entropic_plugin_api_version();
//   extern "C" ENTROPIC_EXPORT entropic_mcp_server_t entropic_create_server();
```

Inside the `.so`, the implementation wraps the C handle around the C++ object:

```cpp
// Inside librentropic-mcp-filesystem.so

#include <entropic/mcp/server_base.h>
#include <entropic/interfaces/i_mcp_server.h>

class FilesystemServer : public entropic::MCPServer { /* C++ internals */ };

extern "C" ENTROPIC_EXPORT int entropic_plugin_api_version() { return 1; }

extern "C" ENTROPIC_EXPORT entropic_mcp_server_t entropic_create_server() {
    return reinterpret_cast<entropic_mcp_server_t>(new FilesystemServer());
}
```

---

## Error Handling Contract

Exceptions must NOT cross `.so` boundaries. All cross-boundary error
reporting uses one of:

1. **Return codes** — C API functions return `entropic_error_t` enum.
2. **Error callbacks** — consumer registers a callback for async errors.
3. **Error state on handles** — `entropic_last_error(handle)` returns
   the last error message string.

```c
typedef enum {
    ENTROPIC_OK = 0,
    ENTROPIC_ERROR_INVALID_CONFIG,
    ENTROPIC_ERROR_MODEL_NOT_FOUND,
    ENTROPIC_ERROR_LOAD_FAILED,
    ENTROPIC_ERROR_GENERATE_FAILED,
    ENTROPIC_ERROR_TOOL_NOT_FOUND,
    ENTROPIC_ERROR_PERMISSION_DENIED,
    ENTROPIC_ERROR_PLUGIN_VERSION_MISMATCH,
} entropic_error_t;

/// @brief Get the last error message for a handle.
/// @return Null-terminated string. Owned by the handle, valid until next call.
const char* entropic_last_error(entropic_handle_t handle);

/// @brief Error callback type for async error reporting.
typedef void (*entropic_error_callback_t)(
    entropic_error_t code,
    const char* message,
    void* user_data);
```

Within a single `.so`, C++ exceptions are used normally. They are caught
at the `.so` boundary and converted to error codes/callbacks.

---

## Inference Backend

### Core Interface (C boundary)

```c
// include/entropic/interfaces/i_inference_backend.h

typedef struct entropic_inference_backend* entropic_inference_backend_t;

entropic_error_t entropic_inference_load(
    entropic_inference_backend_t backend,
    const char* config_json);

entropic_error_t entropic_inference_activate(
    entropic_inference_backend_t backend);

entropic_error_t entropic_inference_generate(
    entropic_inference_backend_t backend,
    const char* messages_json,
    const char* params_json,
    char** result_json);

entropic_error_t entropic_inference_generate_streaming(
    entropic_inference_backend_t backend,
    const char* messages_json,
    const char* params_json,
    void (*on_token)(const char* token, size_t len, void* user_data),
    void* user_data,
    int* cancel_flag);
```

### Internal C++ Implementation (within the .so)

```cpp
// include/entropic/inference/backend.h
// Internal to inference .so — not exposed across boundaries

namespace entropic {

class InferenceBackend {
public:
    bool load(const ModelConfig& config);
    bool activate();
    void deactivate();
    void unload();

    GenerationResult generate(
        const std::vector<Message>& messages,
        const GenerationParams& params);

    GenerationResult generate_streaming(
        const std::vector<Message>& messages,
        const GenerationParams& params,
        std::function<void(std::string_view token)> on_token,
        std::atomic<bool>& cancel);

    ModelState state() const { return state_.load(); }

protected:
    virtual bool do_load(const ModelConfig& config) = 0;
    virtual bool do_activate() = 0;
    virtual void do_deactivate() = 0;
    virtual void do_unload() = 0;
    virtual GenerationResult do_generate(
        const std::vector<Message>& messages,
        const GenerationParams& params) = 0;
    virtual GenerationResult do_generate_streaming(
        const std::vector<Message>& messages,
        const GenerationParams& params,
        std::function<void(std::string_view token)> on_token,
        std::atomic<bool>& cancel) = 0;

private:
    std::atomic<ModelState> state_{ModelState::COLD};
    std::filesystem::path model_path_;
    std::mutex transition_mutex_;   // Guards state TRANSITIONS only
    Metrics metrics_;
};

} // namespace entropic
```

Key differences from earlier design:
- `state_` is `std::atomic` — lock-free reads
- `transition_mutex_` guards transitions, not queries or generation
- `generate_streaming()` is a first-class method, not a hook bolted on later
- `cancel` is `std::atomic<bool>&` — consumer can abort mid-generation
- C boundary uses raw function pointer for streaming callback (no vtable)

### Build Matrix

```
CMake Flag                     Output
──────────────────────────────────────────────
-DENTROPIC_CUDA=ON             librentropic-inference-cuda.so
-DENTROPIC_VULKAN=ON           librentropic-inference-vulkan.so
-DENTROPIC_CPU_ONLY=ON         librentropic-inference-cpu.so
-DENTROPIC_AXCL=ON             librentropic-inference-axcl.so
```

### Runtime Detection

```c
entropic_compute_backend_t entropic_detect_backend();
entropic_inference_backend_t entropic_load_inference_backend(
    entropic_compute_backend_t backend);
```

Auto detection order: CUDA → Vulkan → CPU.
User override via config: `inference.backend: cuda | vulkan | cpu | auto`

When `CONFIG_ENTROPIC_CPU_ONLY` is set at compile time, detection is
compiled out entirely — `detect_backend()` returns CPU unconditionally.

### Distribution

| Channel | Inference .so included |
|---------|----------------------|
| PyPI wheel | CUDA + CPU (detect at runtime) |
| apt/deb | Separate packages per backend |
| Source build | User picks via CMake flag |

---

## MCP Server Plugins

Built-in MCP servers compile into individual `.so` files. Third-party
servers use the same plugin interface. A consumer ships only the servers
they need.

```
librentropic-mcp.so               Base + registry + executor + permissions
librentropic-mcp-filesystem.so    Filesystem server
librentropic-mcp-bash.so          Bash server
librentropic-mcp-git.so           Git server
librentropic-mcp-diagnostics.so   Diagnostics + LSP client
librentropic-mcp-web.so           Web search + fetch
librentropic-mcp-entropic.so      Internal tools (todo, delegate, etc.)
```

### Permissions

The permission model ships WITH the tool system (not later). Default-deny
for all tools. The consumer's runtime config defines the allow list.
Per-identity granularity is added later (v1.9.4) but the enforcement
point exists from day one.

### Plugin Loading

```c
entropic_mcp_server_t entropic_load_mcp_plugin(const char* so_path);
```

The server manager discovers `.so` files in a plugin directory. Each must
export `entropic_plugin_api_version()` and `entropic_create_server()`.
Version mismatch → rejected with `ENTROPIC_ERROR_PLUGIN_VERSION_MISMATCH`.

---

## Compile-Time Configuration

Feature selection at compile time via CMake options. All options are
abstracted through a generated `entropic_config.h` so the configuration
frontend can be swapped (CMake presets today, Kconfig or menuconfig later)
without changing any source code.

### Configuration Flow

```
CMakeLists.txt                    Reads CMake options
    │
    ▼
entropic_config.h.in              Template with @CONFIG_*@ placeholders
    │
    ▼
entropic_config.h (generated)     #define CONFIG_ENTROPIC_* values
    │
    ▼
Source code                       #ifdef CONFIG_ENTROPIC_* guards
```

Source code ONLY reads `#define` from `entropic_config.h`. It never checks
CMake variables directly. This makes the configuration frontend-agnostic.

### CMake Options

```cmake
# Build type
option(ENTROPIC_SHARED       "Build shared libraries (.so)"          ON)
option(ENTROPIC_STATIC       "Build static library (.a)"             OFF)

# Inference backend
option(ENTROPIC_CUDA         "Build CUDA inference backend"          ON)
option(ENTROPIC_VULKAN       "Build Vulkan inference backend"        OFF)
option(ENTROPIC_CPU_ONLY     "Build CPU-only inference backend"      OFF)

# MCP servers (each is an individual .so or compiled-in for static)
option(ENTROPIC_MCP_FILESYSTEM  "Include filesystem MCP server"     ON)
option(ENTROPIC_MCP_BASH        "Include bash MCP server"           ON)
option(ENTROPIC_MCP_GIT         "Include git MCP server"            ON)
option(ENTROPIC_MCP_DIAGNOSTICS "Include diagnostics MCP server"    ON)
option(ENTROPIC_MCP_WEB         "Include web MCP server"            ON)
option(ENTROPIC_MCP_ENTROPIC    "Include entropic MCP server"       ON)

# Storage
option(ENTROPIC_STORAGE_SQLITE  "Include SQLite storage backend"    ON)

# Features
option(ENTROPIC_STREAMING       "Include streaming generation API"  ON)
```

### Static Build

When `ENTROPIC_STATIC=ON`, the build produces a single `librentropic.a`.
Plugin loading via `dlopen` is replaced by a compile-time registry:

```cpp
// Generated by CMake based on enabled MCP servers
// entropic_static_plugins.cpp

#include <entropic/mcp/server_loader.h>

extern entropic_mcp_server_t entropic_create_filesystem_server();
extern entropic_mcp_server_t entropic_create_bash_server();
// ... only servers enabled in config

static const entropic_plugin_entry_t static_plugins[] = {
    {"filesystem", entropic_create_filesystem_server},
    {"bash", entropic_create_bash_server},
    {nullptr, nullptr}  // sentinel
};

const entropic_plugin_entry_t* entropic_get_static_plugins() {
    return static_plugins;
}
```

The server manager checks for static plugins first, falls back to `dlopen`
discovery only when `ENTROPIC_SHARED=ON`.

### CMake Presets

```json
// CMakePresets.json
{
  "configurePresets": [
    {
      "name": "full",
      "description": "Full featured CUDA build (TUI developer)",
      "cacheVariables": {
        "ENTROPIC_CUDA": "ON",
        "ENTROPIC_SHARED": "ON"
      }
    },
    {
      "name": "minimal-static",
      "description": "Minimal static build (embedded consumer)",
      "cacheVariables": {
        "ENTROPIC_STATIC": "ON",
        "ENTROPIC_CPU_ONLY": "ON",
        "ENTROPIC_MCP_BASH": "OFF",
        "ENTROPIC_MCP_GIT": "OFF",
        "ENTROPIC_MCP_WEB": "OFF",
        "ENTROPIC_MCP_DIAGNOSTICS": "OFF",
        "ENTROPIC_STORAGE_SQLITE": "OFF"
      }
    },
    {
      "name": "game",
      "description": "Game engine integration (CUDA, minimal servers)",
      "cacheVariables": {
        "ENTROPIC_CUDA": "ON",
        "ENTROPIC_SHARED": "ON",
        "ENTROPIC_MCP_BASH": "OFF",
        "ENTROPIC_MCP_GIT": "OFF",
        "ENTROPIC_MCP_WEB": "OFF"
      }
    }
  ]
}
```

---

## Class Hierarchy Patterns

### Three-Layer Architecture

Every subsystem follows: interface (C contract) → concrete base (80% logic)
→ implementation (20% specifics).

```
C interface (i_inference_backend.h)     Opaque handle + C functions
  └─ InferenceBackend (backend.h)       80% logic: lifecycle, locking, metrics
       └─ LlamaCppBackend               20% override: load, generate, stream

C interface (i_mcp_server.h)            Opaque handle + C functions
  └─ MCPServer (server_base.h)          80% logic: dispatch, permissions
       └─ FilesystemServer              20% override: tool handlers

ChatAdapter (adapter_base.h)            80% logic: prompt assembly, think-blocks
  └─ Qwen35Adapter                      20% override: tool parsing, tags
```

The C interface is the `.so` boundary. The C++ classes are internal to
each `.so` — they never appear in public headers or cross library boundaries.

---

## Config Schema Pattern

Structs with defaults + separate validation functions.
No metaclass magic. Validation is explicit and testable.

```cpp
/// @brief Model configuration for a single tier.
struct ModelConfig {
    std::filesystem::path path;         ///< Resolved model path
    std::string adapter = "qwen35";     ///< Adapter name
    int context_length = 16384;         ///< Context window (512–131072)
    int gpu_layers = -1;                ///< GPU layers (-1 = all)
    bool keep_warm = false;             ///< Pre-warm at startup
    bool use_mlock = true;              ///< Lock model in RAM
    int reasoning_budget = -1;          ///< Think token budget (-1 = unlimited)
    std::string cache_type_k = "f16";   ///< KV cache key quantization
    std::string cache_type_v = "f16";   ///< KV cache value quantization
    int n_batch = 512;                  ///< Batch size for prompt processing
    int n_threads = 0;                  ///< CPU threads (0 = auto)
    std::string tensor_split = "";      ///< Multi-GPU tensor split ratios
    bool flash_attn = true;             ///< Flash attention
    std::optional<std::vector<std::string>> allowed_tools; ///< Tool filter
};

/// @brief Validate and transform config.
/// @return Empty string on success, error message on failure.
std::string validate(ModelConfig& config, const BundledModels& registry);
```

---

## Doxygen Standard

Code comments ARE the documentation. `docs/` holds roadmap, diagrams,
and architecture visuals only. Every public symbol gets a Doxygen block.

### Required Elements

| Element | Doxygen tags |
|---------|-------------|
| Class | `@brief`, lifecycle diagram (`@code`), threading notes |
| Public method | `@brief`, `@param`, `@return`, `@throws`, `@par Example` |
| Struct fields | `///<` inline doc on every field |
| Enum values | `///<` inline doc on every value |
| File | `@file`, `@brief` at top |
| Interface | `@par Implementors must provide`, `@par Plugin export` |

---

## External Dependencies

| Library | Purpose | Type | Cross-platform | Notes |
|---------|---------|------|----------------|-------|
| llama.cpp | Inference | Submodule | Linux/Mac/Win | Direct C API |
| nlohmann/json | JSON parse/emit | Header-only | Yes | MIT, de facto standard |
| ryml | YAML config | Header-only | Yes | Replaces yaml-cpp, faster |
| sqlite3 | Conversation storage | System lib | Yes | Universal |
| spdlog | Structured logging | Header-only | Yes | |
| cpp-httplib | HTTP/SSE (ext MCP) | Header-only | Yes | |
| Test framework | Unit/integration | Header-only | Yes | TBD (Catch2 or GoogleTest) |

No third-party headers appear in interface contracts or types headers.
Dependencies are implementation details of their respective `.so` files.

---

## Project Structure

```
entropic/
├── CMakeLists.txt
├── CMakePresets.json
├── include/
│   └── entropic/
│       ├── entropic.h                  Public C API
│       ├── entropic_config.h.in        Build config template
│       ├── entropic_export.h           ENTROPIC_EXPORT macro
│       ├── interfaces/                 C contract headers
│       │   ├── i_inference_backend.h
│       │   ├── i_mcp_server.h
│       │   ├── i_storage_backend.h
│       │   ├── i_config_loader.h
│       │   └── i_hook_handler.h
│       ├── types/                      Shared types
│       │   ├── message.h
│       │   ├── tool_call.h
│       │   ├── directive.h
│       │   ├── generation_result.h
│       │   ├── error.h
│       │   └── config.h
│       ├── core/
│       │   ├── engine.h
│       │   ├── context.h
│       │   ├── delegation.h
│       │   └── directives.h
│       ├── inference/
│       │   ├── backend.h
│       │   ├── backend_loader.h
│       │   ├── orchestrator.h
│       │   └── adapters/
│       │       ├── adapter_base.h
│       │       └── qwen35.h
│       ├── mcp/
│       │   ├── server_base.h
│       │   ├── server_loader.h
│       │   ├── tool_registry.h
│       │   └── servers/
│       │       ├── filesystem.h
│       │       ├── bash.h
│       │       └── ...
│       ├── config/
│       │   ├── schema.h
│       │   └── loader.h
│       ├── prompts/
│       │   └── manager.h
│       └── storage/
│           ├── backend.h
│           └── logger.h
├── src/
│   ├── core/
│   ├── inference/
│   ├── mcp/
│   ├── config/
│   ├── prompts/
│   └── storage/
├── data/                               Bundled assets (unchanged from Python)
│   ├── prompts/identity_*.md
│   ├── tools/**/*.json
│   ├── grammars/*.gbnf
│   ├── bundled_models.yaml
│   └── default_config.yaml
├── tests/
│   ├── unit/
│   └── integration/
├── python/                             Auto-generated wrapper
│   └── entropic/
│       └── __init__.py                 Generated from entropic.h
├── docs/
│   ├── roadmap.md
│   ├── architecture-cpp.md             This document
│   ├── diagrams/
│   └── Doxyfile
└── extern/
    └── llama.cpp/                      Git submodule
```

---

## Design Rules

1. **Arrows point down.** No circular dependencies between libraries.
2. **Pure C at `.so` boundaries.** Opaque handles, C function pointers, C types only. No C++ types cross library boundaries.
3. **Plugin ABI versioning.** Every plugin exports `entropic_plugin_api_version()`. Version mismatch = rejected.
4. **Three-layer hierarchy.** C interface (contract) → concrete base (80% logic) → implementation (20% specifics).
5. **Exceptions do not cross boundaries.** Caught at `.so` edge, converted to error codes/callbacks.
6. **No third-party headers in interfaces.** nlohmann/json, ryml, etc. are implementation details.
7. **Symbol visibility.** Only `ENTROPIC_EXPORT` symbols are public. Everything else hidden.
8. **Permissions ship with tools.** Default-deny from day one. Per-identity granularity added later.
9. **Streaming is first-class.** `generate_streaming()` in core interface, raw function pointer callback.
10. **Atomic state, mutex transitions.** State queries are lock-free. Only state changes take locks.
11. **Cancel token on generate.** Consumer can abort mid-generation via atomic flag.
12. **Config via generated header.** `entropic_config.h` generated from CMake, source never reads CMake vars directly. Configuration frontend is swappable.
13. **Static build supported.** `ENTROPIC_STATIC=ON` produces `.a` with compile-time plugin registry.
14. **Data files are shared.** Same identity prompts, tool JSONs, grammars as Python.
15. **Auto-generated Python wrapper.** C API header → Python bindings, no manual sync.
