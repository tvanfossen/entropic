# Entropic — C++ Architecture Design

Reference architecture for the C++ engine. This document defines the
library decomposition, dependency graph, interface contracts, class
hierarchy patterns, plugin architecture, and build configuration that
constrain feature work.

> **Status**: living design document. The original C++ rewrite from
> the v1.7.x Python prototype completed at v2.0.0; this file now
> guides v2.x evolution. Port-era notes referencing the Python
> behavioral spec have been retained where they document a decision
> (so the rationale stays visible) but no longer describe in-flight
> work.

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
library links against. Established in v1.8.0.

- Message, ToolCall, Directive, GenerationResult
- Config structs (ModelConfig, TierConfig, etc.)
- Enums (ModelState, AgentState, DirectiveType)
- Error types (`entropic_error_t`, `entropic_last_error()`,
  `entropic_error_callback_t`)

#### Message C Representation

Messages are JSON strings at `.so` boundaries, C++ structs internally.
The JSON wire format:

```json
{"role": "user", "content": "...", "metadata": {"source": "user"}, "tool_calls": []}
```

Within a single `.so`, messages use the C++ `Message` struct. Cross-boundary
APIs accept and return `const char* messages_json` (JSON array of message
objects). Metadata is an arbitrary JSON object — no fixed schema enforced
at the type level.

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

SQLite conversation persistence, session log files, audit log.

Links: `librentropic-types`, `sqlite3`, `spdlog`

Implements: `IStorageBackend` interface contract.

**Note on spdlog:** All libraries use spdlog for structured logging (established
in v1.8.0). spdlog is listed under storage in the dependency graph for
simplicity, but it is a universal development dependency linked by every `.so`
that needs logging.

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
│   ├── i_hook_handler.h               C function pointer callback types
│   └── i_inference_callbacks.h        Function pointer typedefs (core→inference)
├── types/                             Shared types (librentropic-types.so)
│   ├── message.h                     Messages + metadata
│   ├── tool_call.h                   Tool call + result types
│   ├── directive.h                   Directive types + wire format
│   ├── generation_result.h           Generation output + metrics
│   ├── error.h                       Error enum + callback types
│   ├── config.h                      Config structs (ModelConfig, TierConfig, etc.)
│   ├── engine_types.h                LoopConfig, LoopContext, AgentState, Metrics
│   ├── generation.h                  GenerationParams, ResourceProfile
│   ├── identity.h                    IdentityConfig, PhaseConfig
│   ├── hooks.h                       Hook point enum, callback types
│   ├── audit.h                       AuditEntry, AuditLogConfig (v1.9.5)
│   └── validation.h                  CritiqueResult, Violation (v1.9.8)
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
/// @return JSON string of ServerResponse. Caller must free with entropic_free().
///
/// Return format (ServerResponse JSON envelope):
/// @code
/// {
///   "result": "Human-readable result text",
///   "directives": [
///     {"type": "delegate", "target": "eng", "task": "..."},
///     {"type": "stop_processing"}
///   ]
/// }
/// @endcode
///
/// If the tool has no directives, the "directives" array is empty.
/// The engine parses directives from this JSON and dispatches them
/// through the DirectiveProcessor. This is the wire format for
/// tool-to-engine communication across .so boundaries.
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

/// @brief Raw text completion without chat template.
/// @param backend Backend handle.
/// @param prompt Raw prompt string (no chat formatting applied).
/// @param params_json Generation parameters as JSON.
/// @param result_json Output: JSON result string. Caller must free.
/// @return ENTROPIC_OK on success.
///
/// Used by the router for digit-based classification. Distinct from
/// generate() which applies chat templates. The prompt is passed
/// directly to the model without any template formatting.
entropic_error_t entropic_inference_complete(
    entropic_inference_backend_t backend,
    const char* prompt,
    const char* params_json,
    char** result_json);
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

    /// @brief Raw text completion without chat template.
    /// Used by the router for digit-based classification.
    GenerationResult complete(
        const std::string& prompt,
        const GenerationParams& params);

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
    virtual GenerationResult do_complete(
        const std::string& prompt,
        const GenerationParams& params) = 0;

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

**YAML everywhere** — config files AND identity frontmatter use YAML,
parsed by ryml (single-header amalgamation via `ryml_all.hpp`). The
validation layer is built in-house, replacing Pydantic's cross-field
validators with explicit `validate()` functions per config struct.

**Config resolution order** (highest wins):
1. Compiled defaults (struct initializers)
2. Global config (`~/.entropic/config.yaml`)
3. Project config (`.entropic/config.local.yaml`)
4. Environment variables (`ENTROPIC_*`)

**Bundled data file discovery:**
- Compile-time `ENTROPIC_DATA_DIR` define (set by CMake to install path)
- Overridable at runtime via `config_dir` field in config YAML
- Used for: identity prompts, tool JSONs, grammars, `bundled_models.yaml`

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

## Test Infrastructure

### Shared Mock Backend

`tests/mocks/mock_inference.h` — a single `MockInferenceInterface` that
grows with each version. Provides scripted responses for `generate()`,
`complete()`, and `generate_streaming()`. Each version adds mock
capabilities as needed (e.g., v1.8.5 adds tool call responses, v1.8.6
adds delegation responses).

**Rules:**
- Each version owns its own test files — tests do not depend on each other
- Mock infrastructure is shared — all tests use the same mock headers
- Integration tests span subsystems but use mocks for inference
- Model tests (GPU recommended; CPU works but is impractically slow)
  are separate from unit/integration tests

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
| ryml | YAML config + frontmatter | Header-only (amalgamation) | Yes | `ryml_all.hpp`, replaces yaml-cpp |
| sqlite3 | Conversation storage | System lib | Yes | Universal |
| spdlog | Structured logging | Header-only | Yes | |
| cpp-httplib | HTTP/SSE (ext MCP) | Header-only | Yes | |
| Test framework | Unit/integration | Header-only | Yes | Selected in v1.8.0 (Catch2 or GoogleTest, BDD-style preferred) |

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
│   ├── storage/
│   └── facade/                         C API wrappers (entropic.h implementations)
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

## Handle Lifecycle

`entropic_handle_t` is an opaque pointer to the engine's root state object.
It owns all subsystems and controls their creation/destruction order.

**Creation order** (in `entropic_create` + `entropic_configure`):
1. Types/logging (spdlog init, error state)
2. Config loader (parse YAML, validate)
3. Inference backend (model load deferred until first generate)
4. MCP server manager + built-in servers
5. Engine core (AgentEngine, ResponseGenerator, ContextManager, DirectiveProcessor)
6. Storage backend (optional — NULL if not configured)
7. Hook registry (empty until hooks registered)

**Destruction order** (in `entropic_destroy`, reverse of creation):
7 → 1. Each subsystem's destroy is null-safe.

**Optional subsystems:**
- Storage (`IStorageBackend*`) — engine works without it (in-memory only)
- External MCP servers — engine works with built-in servers only
- Hooks — engine works with empty registry

**Mandatory subsystems:**
- Config (must parse successfully)
- Inference backend (must have at least one loadable model)
- Core engine (state machine, response generator)

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
16. **Directive wire format.** MCP server `execute()` returns JSON with `{"result": "...", "directives": [...]}` envelope. Directives are typed JSON objects dispatched by the engine's DirectiveProcessor.
17. **Bundled data discovery.** Compile-time `ENTROPIC_DATA_DIR`, overridable at runtime via config. Used for prompts, tool schemas, grammars, model registry.
18. **Messages are JSON at boundaries.** `const char* messages_json` at C API, C++ structs internally. Metadata is arbitrary JSON.
19. **WARM→ACTIVE reloads the model.** llama.cpp ties GPU layer offloading to `llama_model_load_from_file()`, not context creation. WARM→ACTIVE frees the CPU-only model and reloads with `n_gpu_layers` from config. mlock ensures pages stay resident, so the reload is fast (~1-3s PCIe transfer, no disk I/O). This matches the Python `_swap_model()` behavior. (v1.8.2)
20. **`log` variable renamed to `logger`.** All inference `.cpp` files use `logger` for the spdlog instance to avoid ambiguity with `entropic::log` namespace and `::log` from `<cmath>`. (v1.8.2)
21. **`extract_directives()` lives in MCP, not core.** Core has zero dependency on nlohmann/json. Directive extraction from tool result JSON (parsing `_directives` key) is performed by the MCP layer (v1.8.5), which passes typed `Directive` structs to core's `DirectiveProcessor`. Core never touches JSON. (v1.8.4)
22. **Core .so uses default symbol visibility.** `librentropic-core.so` is an internal library, not a plugin boundary. All symbols are visible (`CXX_VISIBILITY_PRESET=default`) so test executables can link directly. Plugin `.so` files retain hidden visibility with explicit `ENTROPIC_EXPORT`. (v1.8.4)
23. **`TierResolutionInterface` for cross-.so tier queries.** Core needs tier identity data (auto_chain target, explicit_completion, system prompt) but cannot depend on prompts.so or inference.so. The facade injects a `TierResolutionInterface` (C++ function pointer struct in engine_types.h) — same pattern as `ToolExecutionInterface`. (v1.8.6)
24. **Pending delegation stored as typed structs, not metadata strings.** `PendingDelegation` and `PendingPipeline` are optional fields on `LoopContext`, avoiding JSON serialization in core.so. The metadata map remains for genuinely string-keyed data. (v1.8.6)
25. **DelegationManager uses callback indirection.** Takes `RunChildLoopFn` and `SwapDirFn` callbacks instead of direct `AgentEngine` references. This avoids circular header dependencies and lets the facade control the wiring. The engine exposes `run_loop(LoopContext&)` as the public entry point for child loops. (v1.8.6)
26. **PermissionPersister uses string-based YAML editing, not ryml.** ryml's static library (`libryml.a`) compiles with hidden symbol visibility. When linked PRIVATE into `librentropic-storage.so`, the ryml symbols are stripped, causing unresolved references at test link time. Since PermissionPersister only appends items to a YAML sequence (trivial operation), string-based line editing avoids the dependency entirely. ryml remains the project's YAML parser for config loading in `librentropic-config.so`. (v1.8.8)
27. **Storage .so uses default visibility (same as core).** `librentropic-storage.so` follows the same pattern as core (design decision #22) — `CXX_VISIBILITY_PRESET=default` so test executables can link directly. It is not a plugin boundary. (v1.8.8)
28. **LoRA adapter API uses `llama_set_adapters_lora()` (plural).** The pinned llama.cpp b8420 API takes an array of adapters + scales, not a single adapter. Deactivation = call with `(nullptr, 0, nullptr)`. There is no `llama_rm_adapter_lora()`. KV cache clear after adapter swap uses `llama_memory_clear(llama_get_memory(ctx), true)`. (v1.9.2)
29. **AdapterManager is a single concrete class, not a three-layer hierarchy.** There is only one implementation (llama.cpp adapters), so no base class / interface layer. Same pattern as HookRegistry. Owned by ModelOrchestrator via composition. (v1.9.2)
30. **Grammar validation uses `llama_sampler_init_grammar(nullptr, gbnf, "root")`.** GBNF parsing is vocabulary-independent — the grammar parser only needs the grammar string. Passing `nullptr` for the vocab parameter is safe for validation: the function returns `nullptr` on parse failure, or a valid sampler on success (freed immediately). This allows `GrammarRegistry::validate()` to work without a loaded model. (v1.9.3)
31. **GrammarRegistry follows the same single-class pattern as AdapterManager (decision #29).** One implementation, no interface layer. Owned by ModelOrchestrator. Grammar resolution happens in the orchestrator's generate path, not in the backend. (v1.9.3)
32. **`do_backend_name()` is the only new pure virtual in v1.9.13.** Every other new method has a default implementation. This is a deliberate breaking change — all InferenceBackend subclasses must identify themselves. Mock backends in tests get one-line overrides. The benefit (backend self-identification in logs, diagnostics, and BackendInfo) outweighs the cost. (v1.9.13)
33. **`do_supports()` uses lookup table + dynamic overrides, not switch-per-case.** A switch with 14 cases violates the 3-return threshold. Instead, a `static constexpr bool[]` table handles always-on capabilities, with a single conditional expression for runtime-dependent capabilities (KV_CACHE, HIDDEN_STATE, VISION, SPECULATIVE_DECODING). (v1.9.13)
34. **State management uses `llama_memory_*` API (not deprecated `llama_kv_cache_*`).** The b8420 pinned llama.cpp uses `llama_get_memory(ctx)` → `llama_memory_clear(mem, true)` and `llama_memory_seq_rm(mem, seq_id, -1, -1)`. The `do_clear_state()` override maps seq_id=-1 to `llama_memory_clear()` and specific seq_id to `llama_memory_seq_rm()`. Works for both transformer and recurrent models since llama.cpp handles both state types internally. (v1.9.13)
35. **PromptCache stores prefix-only KV via two-pass prefill.** `llama_state_seq_get_data` has no range parameter — whatever is in seq 0 at save time is what gets serialized. On cache miss, `run_prefill_cached` therefore prefills only the system prefix (clearing memory first via `run_prefill`), saves that state, then continues prefilling the remaining tokens. This keeps the cache entry honest: restore + auto-positioned decode is correct by construction, no runtime truncation needed. Partial-range `llama_memory_seq_rm` after `llama_state_seq_set_data` is NOT a viable alternative — `state_read_meta` allocates non-contiguous cells, and partial removal on that layout returns false (llama.cpp API contract). (v2.0.6)
36. **`entropic mcp-bridge` is a pure stdio↔unix-socket relay (no engine).** Adding `entropic_create` / model load to the bridge is forbidden by design. The bridge owns no state beyond two file descriptors; an engine host (TUI, consumer app, future headless server) owns the model and publishes the unix socket. Discovery is deterministic from the canonical project_dir hash (`compute_socket_path`). `--socket PATH` overrides discovery for non-standard layouts and testing. (v2.1.7, gh#34)
37. **Unix-socket trust boundary = same uid + file perms.** External bridge enforces this via SO_PEERCRED UID check on `accept()`, `~/.entropic/socks/` mode 0700, socket file mode 0600 (explicit `chmod`, not umask), and symlink-/non-socket-safe bind. A session-token handshake was considered and rejected: same-uid attackers can read the token, cross-uid attackers cannot reach the socket — adds protocol surface without changing the trust boundary. **TCP transport is the trigger for proper token / OAuth auth** — when/if a TCP transport ships, design auth at that point. Do not retrofit it onto the unix-socket path. (v2.1.7, gh#34)
38. **Speculative decoding is decoupled from the router.** `inference.speculative.draft_model` and `routing.model` are independent config keys. The fact that a small Qwen happens to be the bundled router AND a candidate draft is incidental, not architectural. The `"router"` and `"draft"` roles on `SecondaryModelLoader` (gh#27) are separately keyed; consumers may point both at the same GGUF if they want, but the engine does not assume or reuse the router handle as the draft. Placement is a config axis (`draft_n_gpu_layers`: 0 = CPU default, -1 = full GPU, hybrid via partial offload) so the GPU stays saturated on the verifier when VRAM is the binding constraint. (v2.1.11, gh#36)
39. **Speculative-decoding recurrent-target gate lives in entropic, not upstream.** At pin `253ba110b`, `extern/llama.cpp/common/speculative.cpp` does NOT self-disable speculative for recurrent / hybrid-Mamba targets (Nemotron-3-Nano-4B, RWKV, etc.) — the architectural rejection has to be applied before reaching `common_speculative_*`. `entropic::speculative::check_compat` calls `llama_model_is_recurrent(target)` first and returns a structured diagnostic ("target model is recurrent ... speculative decoding is incompatible") when applicable. This protects against silent incorrectness on the hybrid-Mamba models bundled in v2.1.9. (v2.1.11, gh#36)
40. **Speculative compat is metadata-only and mirrors upstream's static helper.** At pin `253ba110b`, `common_speculative_is_compat` is no longer public — the equivalent check is `static bool common_speculative_are_compatible(model_tgt, model_dft)` inside `common/speculative.cpp`, invoked implicitly from the draft-simple ctor which **throws** `std::runtime_error` on mismatch. To preserve the query-without-commit ergonomic the v2.1.11 proposal calls for, entropic mirrors the upstream rules (vocab type, BOS/EOS parity, n_tokens delta ≤ 128, prefix token-text equality from id 5) using only public `llama_vocab_*` accessors. The mirror is unit-testable on CPU with mock vocabs — no `llama_context` needed. **Drift risk:** if a future pin tightens the upstream rules, the mirror needs to be updated. The fixture for this is the bundled regression test (`entropic-speculative-compat-tests`) plus a comment pointer back to upstream `speculative.cpp` in the helper source. (v2.1.11, gh#36)
41. **Speculative kernel ships gated + inert at the v2.1.11 pin; bit-identical correctness is structurally unreachable on every bundled primary.** Session 5's Gate A diagnostic (`spec_diag_boundary`) dumped top-5 logits at the speculative split-prefill boundary for both the plain and speculative paths on Qwen3.5 and Gemma 4. With identical input tokens, identical n_past, and identical chat templates, the spec-side logits at sequence position N-1 diverged catastrophically (different top-1 token, half the logit magnitude). Root cause: upstream's `speculative-simple.cpp` scheme of `prefill[0..N-2]` then a separate batch decode of `[id_last + drafts]` assumes pure-transformer state continuity across ubatch boundaries. At pin `253ba110b`: (a) `llm_arch_is_hybrid` returns true for QWEN35, QWEN35MOE, QWEN3NEXT, KIMI_LINEAR, NEMOTRON_H/NEMOTRON_H_MOE, JAMBA, FALCON_H1, PLAMO2, GRANITE_HYBRID, LFM2/LFM2MOE — all of which actively use `ssm_d_state`/`ssm_n_group`/`ssm_dt_rank` hparams (`llama-model.cpp:1705-1715, 487-493`); the arch guard refuses these via the new `ENTROPIC_ERROR_SPECULATIVE_INCOMPATIBLE_ARCH` code. (b) Gemma 4 (`LLM_ARCH_GEMMA4`) is NOT in `is_hybrid` but still allocates `llama_memory_recurrent` for half its layers — the classification is incomplete, so the guard lets Gemma 4 through and the kernel runs but produces divergent state too; additionally, the CPU-resident E2B draft is too slow on a 16 GB GPU to amortize (measured 0.46× speedup). The kernel code, hybrid-inclusive guard, and `entropic_speculative_compat` C ABI all SHIP in v2.1.11 as the foundation; the model tests SKIP with WARN messages pointing at this entry and the proposal's Gate A. The kernel becomes useful at a future llama.cpp pin where cross-ubatch SSM state continuity is correct (or when a non-hybrid bundled primary lands). The separately discovered entropic-side off-by-one in `spec_trim_*` (seq_rm boundary used `n_past + 1` where upstream uses `n_past`) is fixed in this version regardless — it's a real bug that would still bite a future working pin. (v2.1.11, gh#36)
42. **MTP speculative decode is a target-owned, shared-KV path — separate from the gh#36 separate-draft kernel (#38–41), which stays the disabled/legacy path.** The MTP head (`mtp-gemma-4-E{2,4}B-it.gguf`, arch `gemma4-assistant`, ~57 MB) shares the target trunk's KV via `cparams.ctx_other = ctx_` (`LLAMA_CONTEXT_TYPE_MTP`, `n_rs_seq=0`). Only the holder of `ctx_` can create that context, so the **target backend owns the head lifecycle**: `setup_mtp_draft` builds it lazily against the live `ctx_`; `teardown_mtp_draft` runs before `ctx_` is freed in `do_deactivate`/`do_unload` (the head borrows `ctx_` via `ctx_other`, gh#58). The decode loop mirrors llama.cpp's server MTP consumer: prefill the **target only**, calling `common_speculative_process` on the prompt batch to seed `pending_h`, then per round `draft → llama_decode(ctx_) → common_speculative_process → common_sampler_sample_and_accept_n → common_speculative_accept`. The caller **never** decodes the head context — the impl owns every head decode (shared-KV ⇒ `process()` skips its catch-up decode); no checkpoint dance (shared-KV Gemma 4 is PART-seq_rm). Lossless by construction at `temperature=0` (only the target's own greedy argmax is ever accepted). The orchestrator routes `inference.speculative.mtp` through `try_mtp_route` **before** the gh#36 compat gate (#39/#41) — MTP tolerates the shared-KV Gemma 4 target the gh#36 gate rejects — and `activate_draft` skips the separate-draft secondary backend under MTP. `GenerationResult.n_drafted`/`n_accepted` expose engagement. MTP is the **experimental** speculative path; on Pascal it is lossless+functional but bandwidth-bound (low accept-rate, no net throughput win) — a modern-HW / ceiling-additive lever, not a floor win. Exercised greedy on Q8 E2B and on mobile-QAT TQ2_0 (E2B + E4B). **Lossless holds only at `temperature=0`** (greedy argmax accept); see #43 for the v2.9.1 envelope guards that enforce this. (v2.9.0, gh#106)
43. **MTP fails fast + loud outside its envelope — never silent fallback (#42 hardening).** v2.9.0 let `speculative.mtp` silently bypass grammar / tools / stop-sequences / streaming and run non-lossless at temperature>0. v2.9.1 makes `generate_mtp` validate up front (`mtp_unsupported_reason`, a pure CPU-testable predicate): `temperature>0`, an active grammar (`params.grammar`), staged tools (`active_tools_json_`), or a streaming call (bound `on_token`) each return the typed `ENTROPIC_ERROR_SPECULATIVE_INCOMPATIBLE_CONFIG` with an actionable message, and `try_mtp_route` **propagates** it (returns true, owns the outcome — no plain-decode fallback that would mask the misconfig). Streaming is distinguished by the callback's bound-ness, so `try_speculative_route` (non-streaming) now passes an **empty** `std::function` rather than a bound no-op lambda. Robustness: `n_draft+1 > n_batch` and empty `draft.path` fail loud (not a silent clamp, not `GGML_ABORT`); a zero-draft round skips `common_speculative_accept` (whose `impl_last` assert would abort) and progresses on the bonus token. Lifecycle: a coarse `mtp_mutex_` held across `generate_mtp` serialises against `teardown_mtp_draft` in `do_deactivate`/`do_unload` (no deactivate-during-generate UAF; teardown itself must NOT lock — it is reached from the already-locked setup-rebuild path). `spec_finalize` now sets `throughput_tok_s`. General principle adopted: incompatible config/state for an explicitly-enabled feature fails LOUDLY so it can be corrected — silent fallback masks intent, a crash kills the host. (v2.9.1, gh#108)
44. **MTP made usable — tools un-guarded, stops honored, flash guarded (#43 correction).** Real-HW (RTX Blackwell) testing showed MTP accelerates ~2.3× and that v2.9.1's guards left it unreachable + over-broad. v2.9.2: (a) the **tools guard is dropped** — MTP is lossless at temp=0 and gemma4 tool-calls are parsed post-hoc (not sampler-grammar-constrained, unlike `params.grammar` which `to_common_sampling` genuinely drops), so MTP+tools yields the same call as plain; this makes MTP reachable through the agent loop (which stages meta-tools). (b) The real gap the tools guard masked was **stop-handling**: `spec_emit_token` only checked EOS, so MTP over-generated past the gh#103 sequential-tool stop. The MTP run now threads `effective_stop(params)` onto `SpeculativeRunState.stop` and `spec_emit_token` calls `check_stop_sequences` (no-op for the gh#36 path, whose `stop` is empty). (c) **`n_draft` default 16→4** (16 over-drafts the small head → net slowdown). (d) **`build_cparams` sets `swa_full=false`** — `llama_context_default_params` defaults it true; correctness-neutral (SWA attention only uses its 512 window), saves ~5 GB at 128k for all Gemma-4 tiers. (e) **MTP+flash fails loud** (the GQA-2 + head_dim-512 head aborts the flash MMA kernel at this pin) → MTP is locked to f16 KV (quantized KV requires flash); the flash/q-KV speedup awaits an upstream `fattn` fix. Grammar + temp>0 + streaming stay guarded (real constraints). (v2.9.2, gh#108)
45. **MTP+flash guard dropped — upstream fixed the GQA-2 flash-attn abort (#44e resolved).** The `flash_attn` branch of `mtp_unsupported_reason` (decision #44e) existed because the gemma4-assistant head (GQA-2, head_dim-512) fell through the flash MMA kernel's `DKQ<=256` fallback path and `GGML_ABORT`ed — the kernel only special-cased GQA>2 for head_dim>256. Upstream llama.cpp #25148 ("CUDA: fix Gemma E4B MTP FlashAttention", merged 2026-06-30, fixes ggml-org/llama.cpp#24400) restores the GQA-1/2 MMA specializations that an earlier compile-time optimization (#21768) had inadvertently disabled. `extern/llama.cpp` bumped `ac4cddeb` → `b9886` (`20a04b22`, 2026-07-06, 294 commits, includes #25148) to pick it up. `mtp_unsupported_reason` no longer takes a `flash_attn` parameter; MTP now runs with flash enabled, which in turn unlocks quantized KV cache (`cache_type_k/v`) for MTP tiers since llama.cpp requires flash for quantized KV. Temperature and grammar guards are unaffected. (v2.9.3, gh#108)
46. **MTP's `temperature>0` guard dropped — the accept step was already lossless at any temperature, not just 0 (#42/#43 correction).** #42/#43 assumed rejection sampling was needed for temp>0 losslessness because the accept step is "naive token-equality, not rejection sampling." Re-derivation found the premise incomplete: `common_speculative_impl_draft_mtp::draft()` (`extern/llama.cpp/common/speculative.cpp`) always proposes `cur_p->data[0].id` — the **argmax** of the (top-k=10-filtered) draft distribution — and never reads `cur_p->selected`, the field a genuine stochastic draw would populate. This holds regardless of `backend_sampling` (confirmed by reading the CUDA/CPU backend-sampler dispatch: when the backend chain lacks a terminal `dist`/`greedy` op, `common_sampler_sample()` falls through to a CPU-side `dist` sampler over the backend-filtered top-10 candidates — a real stochastic draw exists, but `draft()` discards it and reads the argmax off the candidate list regardless). The draft proposal is therefore a **deterministic point mass** at every temperature. For a point-mass proposal `q(m)=1`, the general Leviathan/Chen rejection-sampling accept rule (`accept-prob min(1, p_target(t)/p_draft(t))`, resample residual on reject) collapses algebraically to exactly what `common_sampler_sample_and_accept_n` already does: draw one real sample `s ~ p_target` through the target's full filter chain, accept iff `s == draft[i]`, else emit `s` as the correction — lossless at any temperature by construction, not an approximation. `mtp_unsupported_reason` no longer takes `temperature` as a live guard (kept in the signature, unused, in case a future pin bump reinstates the need). **Correctness now depends on an assumption about vendored code** (`draft()` reads `data[0]` not `selected`) that a future `extern/llama.cpp` pin bump could silently invalidate — the empirical distribution test in `test_gh108_mtp_guards.cpp` ("MTP is lossless at temperature>0") is the regression tripwire; if a pin bump makes drafting genuinely stochastic, that test's tolerance-band check should start failing, and the guard must be reinstated. (v2.9.4, gh#108)
48. **MTP grammar handling: per-tier static control + per-request dynamic safety net, not a single dispatch-level bypass (#43 refinement, not violation).** Two real-world blockers surfaced once consumers ran realistic multi-tier configs: (a) `speculative.{enabled,mtp}` is a single global flag with no way to keep MTP on for most tiers while permanently excluding a grammar-heavy identity/model, and (b) a request-level dynamic grammar (e.g. the constitutional validator's critique call, which supplies `params.grammar` per-call on an otherwise grammar-free resident tier) hit `generate_mtp`'s loud `ENTROPIC_ERROR_SPECULATIVE_INCOMPATIBLE_CONFIG` instead of falling back, silently breaking validation (the error was swallowed by the caller, returning an empty critique). Fix has two parts: **(1) `TierConfig::speculative_mtp`** (nullopt = inherit global, `resolve_mtp_effective(tier_name)` applies the precedence) — coarse, static, per-identity/model control, following the same "inherit unless set" pattern as `TierConfig::temperature`. A tier that statically combines `speculative_mtp=true` with a static `grammar` is rejected at **config-load time** (`parse_tier_config`), not per-request, since that combination would fail every call. **(2) A request-level check** in `try_speculative_route_streaming`: even on an MTP-effective tier, `params.grammar.empty()` gates entry to `try_mtp_route` — a dynamic grammar on one call falls through to plain decode instead of erroring, without requiring the whole tier to disable MTP. Neither part violates #43's fail-loud principle: #43 was about MTP **silently running** in an unsupported config and producing subtly wrong output undetected. Here, (1) turns a would-be-every-request runtime error into a *louder, earlier* load-time error, and (2) is dispatch selecting the strictly correct decode path using a request property (`params.grammar`) that is fully known **before** any MTP-specific work starts — the same way the global on/off flag already silently selects plain decode when `speculative.enabled` is false. `generate_mtp`'s own `mtp_guard`/`mtp_unsupported_reason` grammar check is unchanged and still fires for any caller that reaches it directly (e.g. bypassing the orchestrator) — the dispatch-level check only avoids reaching it needlessly through the normal orchestrator path. (v2.9.4, gh#108)
49. **MTP was reachable from the orchestrator but structurally unreachable from the agent loop — two independent gates, not one (gh#110, v2.9.6).** Gate 1: `build_loop_config()` (`src/facade/entropic.cpp`) hardcoded `LoopConfig::stream_output = true`, so the agent loop always took `ResponseGenerator::generate_streaming()`, which unconditionally binds a non-empty `on_token` callback into `orchestrator->generate_streaming()` → `try_speculative_route_streaming` → `try_mtp_route` → `LlamaCppBackend::generate_mtp`'s `mtp_guard`. `mtp_guard` derives its `streaming` argument to `mtp_unsupported_reason` as `static_cast<bool>(on_token)` (`llama_cpp_backend.cpp`) — a bound callback is indistinguishable from "this is a streaming call," so every agent-loop turn with `speculative.mtp` on failed loud with `ENTROPIC_ERROR_SPECULATIVE_INCOMPATIBLE_CONFIG`, unconditionally. Gate 2: even with streaming disabled, `ResponseGenerator::dispatch_batch_generate` preferred the cancel-aware `inference_.generate_cancellable` bridge whenever wired (which production's `interface_factory.cpp` always does) — its backing call, `ModelOrchestrator::generate(messages, params, cancel, tier)`, deliberately bypasses `run_generate_dispatch` (doc comment: "batch only ever calls plain decode"); only the no-cancel `ModelOrchestrator::generate(messages, params, tier)` overload runs `run_generate_dispatch`. Existing MTP tests (`test_gh106_mtp_route.cpp`, `test_gh108_agentic_benchmark.cpp`) never caught this because they call `orchestrator->generate()` directly — shaped like the agent loop's traffic, but never actually routed through `AgentEngine`/`ResponseGenerator`/the facade. Fix: (a) `generation.stream_output` (new `GenerationConfig` field, default `true` — no behavior change for existing consumers) threads through `build_loop_config()` into `LoopConfig::stream_output`, making batch mode reachable; (b) `LoopConfig::speculative_enabled` (mirrors `inference.speculative.enabled` — core.so cannot depend on config.so per design rule 2, so the facade plumbs a plain bool same as `budget_mode`/`budget_limit`) makes `dispatch_batch_generate` prefer the dispatching `inference_.generate` over `generate_cancellable` whenever speculative decoding is on. v1 tradeoff, documented not hidden: a speculative batch turn is not cancellable mid-decode. `test_gh110_mtp_agent_loop.cpp` closes the coverage gap by driving the real `entropic_create`/`entropic_configure_dir`/`entropic_run` path with `generation.stream_output: false` + `inference.speculative.{enabled,mtp}: true`, asserting on the backend's own `"Speculative: generated=..."` log line (the only signal that crosses the C-ABI boundary — `n_drafted`/`n_accepted` do not appear in `entropic_run`'s or `entropic_metrics_json`'s output). (v2.9.6, gh#110)
