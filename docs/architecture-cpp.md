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
    bool keep_warm = false;             ///< Stay WARM on swap-out (NOT startup)
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
3. Inference backend (default tier loaded here unless
   `models.defer_load: true`, which defers it to first use — gh#157)
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
42. **MTP speculative decode is a target-owned, shared-KV path — separate from the gh#36 separate-draft kernel (#38–41), which stays the disabled/legacy path.** The MTP head (`mtp-gemma-4-E{2,4}B-it.gguf`, arch `gemma4-assistant`, ~57 MB) shares the target trunk's KV via `cparams.ctx_other = ctx_` (`LLAMA_CONTEXT_TYPE_MTP`, `n_rs_seq=0`). Only the holder of `ctx_` can create that context, so the **target backend owns the head lifecycle**: `setup_mtp_draft` builds it lazily against the live `ctx_`; `teardown_mtp_draft` runs before `ctx_` is freed in `do_deactivate`/`do_unload` (the head borrows `ctx_` via `ctx_other`, gh#58). The decode loop mirrors llama.cpp's server MTP consumer: prefill the **target only**, calling `common_speculative_process` on the prompt batch to seed `pending_h`, then per round `draft → llama_decode(ctx_) → common_speculative_process → common_sampler_sample_and_accept_n → common_speculative_accept`. The caller **never** decodes the head context — the impl owns every head decode (shared-KV ⇒ `process()` skips its catch-up decode); no checkpoint dance (shared-KV Gemma 4 is PART-seq_rm). Lossless by construction at `temperature=0` (only the target's own greedy argmax is ever accepted). The orchestrator routes `inference.speculative.mtp` through `try_mtp_route` **before** the gh#36 compat gate (#39/#41) — MTP tolerates the shared-KV Gemma 4 target the gh#36 gate rejects — and `activate_draft` skips the separate-draft secondary backend under MTP. `GenerationResult.n_drafted`/`n_accepted` expose engagement. MTP is the **experimental** speculative path; on Pascal it is lossless+functional but bandwidth-bound (low accept-rate, no net throughput win) — a modern-HW / ceiling-additive lever, not a floor win. **[SUPERSEDED — see #59.** That figure was measured at `n_draft=16`, the default #44 later changed to 4 as a net slowdown, and was never re-measured on Pascal afterwards. Do not cite it as a hardware property.**]** Exercised greedy on Q8 E2B and on mobile-QAT TQ2_0 (E2B + E4B). **Lossless holds only at `temperature=0`** (greedy argmax accept); see #43 for the v2.9.1 envelope guards that enforce this. (v2.9.0, gh#106)
43. **MTP fails fast + loud outside its envelope — never silent fallback (#42 hardening).** v2.9.0 let `speculative.mtp` silently bypass grammar / tools / stop-sequences / streaming and run non-lossless at temperature>0. v2.9.1 makes `generate_mtp` validate up front (`mtp_unsupported_reason`, a pure CPU-testable predicate): `temperature>0`, an active grammar (`params.grammar`), staged tools (`active_tools_json_`), or a streaming call (bound `on_token`) each return the typed `ENTROPIC_ERROR_SPECULATIVE_INCOMPATIBLE_CONFIG` with an actionable message, and `try_mtp_route` **propagates** it (returns true, owns the outcome — no plain-decode fallback that would mask the misconfig). Streaming is distinguished by the callback's bound-ness, so `try_speculative_route` (non-streaming) now passes an **empty** `std::function` rather than a bound no-op lambda. Robustness: `n_draft+1 > n_batch` and empty `draft.path` fail loud (not a silent clamp, not `GGML_ABORT`); a zero-draft round skips `common_speculative_accept` (whose `impl_last` assert would abort) and progresses on the bonus token. Lifecycle: a coarse `mtp_mutex_` held across `generate_mtp` serialises against `teardown_mtp_draft` in `do_deactivate`/`do_unload` (no deactivate-during-generate UAF; teardown itself must NOT lock — it is reached from the already-locked setup-rebuild path). `spec_finalize` now sets `throughput_tok_s`. General principle adopted: incompatible config/state for an explicitly-enabled feature fails LOUDLY so it can be corrected — silent fallback masks intent, a crash kills the host. (v2.9.1, gh#108)
44. **MTP made usable — tools un-guarded, stops honored, flash guarded (#43 correction).** Real-HW (RTX Blackwell) testing showed MTP accelerates ~2.3× and that v2.9.1's guards left it unreachable + over-broad. v2.9.2: (a) the **tools guard is dropped** — MTP is lossless at temp=0 and gemma4 tool-calls are parsed post-hoc (not sampler-grammar-constrained, unlike `params.grammar` which `to_common_sampling` genuinely drops), so MTP+tools yields the same call as plain; this makes MTP reachable through the agent loop (which stages meta-tools). (b) The real gap the tools guard masked was **stop-handling**: `spec_emit_token` only checked EOS, so MTP over-generated past the gh#103 sequential-tool stop. The MTP run now threads `effective_stop(params)` onto `SpeculativeRunState.stop` and `spec_emit_token` calls `check_stop_sequences` (no-op for the gh#36 path, whose `stop` is empty). (c) **`n_draft` default 16→4** (16 over-drafts the small head → net slowdown). (d) **`build_cparams` sets `swa_full=false`** — `llama_context_default_params` defaults it true; correctness-neutral (SWA attention only uses its 512 window), saves ~5 GB at 128k for all Gemma-4 tiers. (e) **MTP+flash fails loud** (the GQA-2 + head_dim-512 head aborts the flash MMA kernel at this pin) → MTP is locked to f16 KV (quantized KV requires flash); the flash/q-KV speedup awaits an upstream `fattn` fix. Grammar + temp>0 + streaming stay guarded (real constraints). (v2.9.2, gh#108)
45. **MTP+flash guard dropped — upstream fixed the GQA-2 flash-attn abort (#44e resolved).** The `flash_attn` branch of `mtp_unsupported_reason` (decision #44e) existed because the gemma4-assistant head (GQA-2, head_dim-512) fell through the flash MMA kernel's `DKQ<=256` fallback path and `GGML_ABORT`ed — the kernel only special-cased GQA>2 for head_dim>256. Upstream llama.cpp #25148 ("CUDA: fix Gemma E4B MTP FlashAttention", merged 2026-06-30, fixes ggml-org/llama.cpp#24400) restores the GQA-1/2 MMA specializations that an earlier compile-time optimization (#21768) had inadvertently disabled. `extern/llama.cpp` bumped `ac4cddeb` → `b9886` (`20a04b22`, 2026-07-06, 294 commits, includes #25148) to pick it up. `mtp_unsupported_reason` no longer takes a `flash_attn` parameter; MTP now runs with flash enabled, which in turn unlocks quantized KV cache (`cache_type_k/v`) for MTP tiers since llama.cpp requires flash for quantized KV. Temperature and grammar guards are unaffected. (v2.9.3, gh#108)
46. **MTP's `temperature>0` guard dropped — the accept step was already lossless at any temperature, not just 0 (#42/#43 correction).** #42/#43 assumed rejection sampling was needed for temp>0 losslessness because the accept step is "naive token-equality, not rejection sampling." Re-derivation found the premise incomplete: `common_speculative_impl_draft_mtp::draft()` (`extern/llama.cpp/common/speculative.cpp`) always proposes `cur_p->data[0].id` — the **argmax** of the (top-k=10-filtered) draft distribution — and never reads `cur_p->selected`, the field a genuine stochastic draw would populate. This holds regardless of `backend_sampling` (confirmed by reading the CUDA/CPU backend-sampler dispatch: when the backend chain lacks a terminal `dist`/`greedy` op, `common_sampler_sample()` falls through to a CPU-side `dist` sampler over the backend-filtered top-10 candidates — a real stochastic draw exists, but `draft()` discards it and reads the argmax off the candidate list regardless). The draft proposal is therefore a **deterministic point mass** at every temperature. For a point-mass proposal `q(m)=1`, the general Leviathan/Chen rejection-sampling accept rule (`accept-prob min(1, p_target(t)/p_draft(t))`, resample residual on reject) collapses algebraically to exactly what `common_sampler_sample_and_accept_n` already does: draw one real sample `s ~ p_target` through the target's full filter chain, accept iff `s == draft[i]`, else emit `s` as the correction — lossless at any temperature by construction, not an approximation. `mtp_unsupported_reason` no longer takes `temperature` as a live guard (kept in the signature, unused, in case a future pin bump reinstates the need). **Correctness now depends on an assumption about vendored code** (`draft()` reads `data[0]` not `selected`) that a future `extern/llama.cpp` pin bump could silently invalidate — the empirical distribution test in `test_gh108_mtp_guards.cpp` ("MTP is lossless at temperature>0") is the regression tripwire; if a pin bump makes drafting genuinely stochastic, that test's tolerance-band check should start failing, and the guard must be reinstated. (v2.9.4, gh#108)
48. **MTP grammar handling: per-tier static control + per-request dynamic safety net, not a single dispatch-level bypass (#43 refinement, not violation).** Two real-world blockers surfaced once consumers ran realistic multi-tier configs: (a) `speculative.{enabled,mtp}` is a single global flag with no way to keep MTP on for most tiers while permanently excluding a grammar-heavy identity/model, and (b) a request-level dynamic grammar (e.g. the constitutional validator's critique call, which supplies `params.grammar` per-call on an otherwise grammar-free resident tier) hit `generate_mtp`'s loud `ENTROPIC_ERROR_SPECULATIVE_INCOMPATIBLE_CONFIG` instead of falling back, silently breaking validation (the error was swallowed by the caller, returning an empty critique). Fix has two parts: **(1) `TierConfig::speculative_mtp`** (nullopt = inherit global, `resolve_mtp_effective(tier_name)` applies the precedence) — coarse, static, per-identity/model control, following the same "inherit unless set" pattern as `TierConfig::temperature`. A tier that statically combines `speculative_mtp=true` with a static `grammar` is rejected at **config-load time** (`parse_tier_config`), not per-request, since that combination would fail every call. **(2) A request-level check** in `try_speculative_route_streaming`: even on an MTP-effective tier, `params.grammar.empty()` gates entry to `try_mtp_route` — a dynamic grammar on one call falls through to plain decode instead of erroring, without requiring the whole tier to disable MTP. Neither part violates #43's fail-loud principle: #43 was about MTP **silently running** in an unsupported config and producing subtly wrong output undetected. Here, (1) turns a would-be-every-request runtime error into a *louder, earlier* load-time error, and (2) is dispatch selecting the strictly correct decode path using a request property (`params.grammar`) that is fully known **before** any MTP-specific work starts — the same way the global on/off flag already silently selects plain decode when `speculative.enabled` is false. `generate_mtp`'s own `mtp_guard`/`mtp_unsupported_reason` grammar check is unchanged and still fires for any caller that reaches it directly (e.g. bypassing the orchestrator) — the dispatch-level check only avoids reaching it needlessly through the normal orchestrator path. (v2.9.4, gh#108)
49. **MTP was reachable from the orchestrator but structurally unreachable from the agent loop — two independent gates, not one (gh#110, v2.9.6).** Gate 1: `build_loop_config()` (`src/facade/entropic.cpp`) hardcoded `LoopConfig::stream_output = true`, so the agent loop always took `ResponseGenerator::generate_streaming()`, which unconditionally binds a non-empty `on_token` callback into `orchestrator->generate_streaming()` → `try_speculative_route_streaming` → `try_mtp_route` → `LlamaCppBackend::generate_mtp`'s `mtp_guard`. `mtp_guard` derives its `streaming` argument to `mtp_unsupported_reason` as `static_cast<bool>(on_token)` (`llama_cpp_backend.cpp`) — a bound callback is indistinguishable from "this is a streaming call," so every agent-loop turn with `speculative.mtp` on failed loud with `ENTROPIC_ERROR_SPECULATIVE_INCOMPATIBLE_CONFIG`, unconditionally. Gate 2: even with streaming disabled, `ResponseGenerator::dispatch_batch_generate` preferred the cancel-aware `inference_.generate_cancellable` bridge whenever wired (which production's `interface_factory.cpp` always does) — its backing call, `ModelOrchestrator::generate(messages, params, cancel, tier)`, deliberately bypasses `run_generate_dispatch` (doc comment: "batch only ever calls plain decode"); only the no-cancel `ModelOrchestrator::generate(messages, params, tier)` overload runs `run_generate_dispatch`. Existing MTP tests (`test_gh106_mtp_route.cpp`, `test_gh108_agentic_benchmark.cpp`) never caught this because they call `orchestrator->generate()` directly — shaped like the agent loop's traffic, but never actually routed through `AgentEngine`/`ResponseGenerator`/the facade. Fix: (a) `generation.stream_output` (new `GenerationConfig` field, default `true` — no behavior change for existing consumers) threads through `build_loop_config()` into `LoopConfig::stream_output`, making batch mode reachable; (b) `LoopConfig::speculative_enabled` (mirrors `inference.speculative.enabled` — core.so cannot depend on config.so per design rule 2, so the facade plumbs a plain bool same as `budget_mode`/`budget_limit`) makes `dispatch_batch_generate` prefer the dispatching `inference_.generate` over `generate_cancellable` whenever speculative decoding is on. v1 tradeoff, documented not hidden: a speculative batch turn is not cancellable mid-decode. `test_gh110_mtp_agent_loop.cpp` closes the coverage gap by driving the real `entropic_create`/`entropic_configure_dir`/`entropic_run` path with `generation.stream_output: false` + `inference.speculative.{enabled,mtp}: true`, asserting on the backend's own `"Speculative: generated=..."` log line (the only signal that crosses the C-ABI boundary — `n_drafted`/`n_accepted` do not appear in `entropic_run`'s or `entropic_metrics_json`'s output). (v2.9.6, gh#110)
50. **ExternalBridge `entropic.ask` is configurable between streaming and non-streaming paths — `mcp.external.ask_streaming` (gh#115, v2.9.12).** `handle_ask` previously hard-coded `entropic_run_streaming`, which (a) binds `on_token`, tripping the MTP incompatibility guard (`mtp_unsupported_reason` treats any bound callback as streaming — decision #43/49), producing 0-char output with `accept_rate=0.000`; and (b) calls `j.dump()` on the streaming-path serialized result without the UTF-8 sanitization applied to the `entropic_run` path in v2.9.7–v2.9.11, throwing `type_error.316` on MTP-split codepoints. The same root as #49: `on_token` binding = streaming flag = MTP guard trip. Fix: `ExternalMCPConfig.ask_streaming` (default `true`) threads through `ExternalBridge::ask_streaming()` getter → `dispatch_ask` → either `handle_ask_plain` (calls `entropic_run`, extracts final text from result JSON — the path already sanitized and MTP-compatible) or `handle_ask` (existing streaming path, unchanged). Consumers running MTP set `mcp.external.ask_streaming: false`; streaming consumers are unaffected. Async `entropic.ask` (`async: true`) already used `entropic_run` inside `run_async_ask` (#59) and is unaffected by this flag. (v2.9.12, gh#115)
51. **`looks_like_mtp_head` predicate guards the classical draft path — fail-loud instead of crashing in fattn.cu (#44/#41 routing gap, gh#107, v2.10.0).** A Gemma 4 MTP head GGUF (1–2 transformer layers, shared-KV architecture) pointed at `draft.path` with `speculative.mtp: false` reached `generate_speculative_with_draft` → upstream `common_speculative_impl_draft_mtp`, which `GGML_ABORT`ed in `fattn.cu:110` (the head's GQA-2/head_dim-512 geometry was incompatible with the classical DRAFT_SIMPLE path). The crash surfaced when consumers upgraded from a classical draft GGUF to the bundled MTP head without toggling the flag. Fix: `looks_like_mtp_head(n_layers)` (`include/entropic/mcp/mtp_envelope.h`) returns true when `n_layers <= 2`; `mtp_head_classical_path_error(n_layers)` returns an actionable message naming `speculative.mtp: true`. In `try_speculative_route_streaming`, before delegating to the classical path, the draft backend's layer count is queried — if `looks_like_mtp_head`, `ENTROPIC_ERROR_SPECULATIVE_INCOMPATIBLE_CONFIG` is returned immediately. The predicate and error message are unit-tested in `mtp_envelope_test.cpp` without a real GGUF. (v2.10.0, gh#107)
52. **MTP grammar guard removed — `to_common_sampling` now propagates `params.grammar` into `common_params_sampling` (#48 superseded, v2.10.0).** The v2.9.4 workarounds in decision #48 (dispatch-level `params.grammar.empty()` gate + config-load rejection of `mtp+grammar`) were never fixes — the root cause, `to_common_sampling` (`llama_cpp_backend.cpp`) not setting `cps.grammar = params.grammar`, was untouched. Once that line was added, MTP's own sampler chain enforces the GBNF constraint for every call, making the guards redundant. Both workarounds are removed: the dispatch gate (so grammar calls now route to MTP rather than silently falling through to plain decode) and the config-load static rejection (so consumers can statically configure `speculative_mtp=true` on a grammar-bearing tier). `mtp_unsupported_reason`'s `has_grammar` parameter becomes `(void)` — the function always returns empty for this axis. The per-tier `TierConfig::speculative_mtp` override mechanism from #48 is retained; it remains useful for permanently disabling MTP on a per-tier basis regardless of the grammar state. (v2.10.0, gh#108)
53. **`generate_streaming` wraps `on_token` with `StreamThinkFilter` and calls `apply_adapter_parse` — MTP streaming guard removed (#43/#49 streaming limitation resolved, v2.10.0).** The v2.9.1 streaming guard (`mtp_unsupported_reason` returning non-empty when `streaming=true`) existed because `strip_thinking_channels` is post-buffer only, and the streaming path never called `apply_adapter_parse` at all — thinking channel tokens (`<|channel>`) flowed live to the consumer and persisted unrestripped in `result.content`. Fix: `generate_streaming` wraps the consumer `on_token` callback with a `StreamThinkFilter` instance before passing it to both `try_speculative_route_streaming` and `model->generate_streaming`. `StreamThinkFilter` takes a C-style `TokenCallback` (`void (*)(const char*, size_t, void*)`) + `void*` user_data; a static trampoline function (`stream_token_trampoline`) bridges the orchestrator's `std::function<void(std::string_view)>` consumer. After generation completes on both paths, `filter.flush()` is called to emit any pending buffered content, then `apply_adapter_parse(model, get_adapter(selected), result)` post-processes the accumulated `result.content` (tool-call parsing, adapter scrub) — the same call the non-streaming `generate` already made at `orchestrator.cpp:600`. The MTP path receives the `StreamThinkFilter`-wrapped lambda automatically; `mtp_unsupported_reason`'s `streaming` parameter becomes `(void)`. `entropic-core` must be linked explicitly by test binaries that include `stream_think_filter.h` — `entropic-inference-cpu` is an OBJECT library whose PRIVATE deps do not auto-propagate to consumers. (v2.10.0, gh#108)

54. **MCP server plugins load as a third routing peer, not as an `MCPServerBase` subclass (gh#133, v2.10.1).** `i_mcp_server.h` documented a dlopen plugin contract from v1.8.5 onward while no loader existed — `grep -rn "dlopen\|dlsym" src/` returned zero hits, for the MCP boundary *and* for the inference-backend boundary that mandates the same `entropic_plugin_api_version()` export. The obvious implementation, a `PluginServerAdapter : MCPServerBase` forwarding virtuals to the C ABI, **does not work**: `MCPServerBase::list_tools()` (`server_base.h:96`) and `::execute()` (`:110`) are non-virtual concrete methods iterating the server's own `registry_`, so an adapter cannot intercept them — `ServerManager` would see an empty registry instead of the plugin's tools. Making them virtual would change the `MCPServerBase` vtable, which this header explicitly defines as requiring an `ENTROPIC_MCP_PLUGIN_API_VERSION` bump, and would break any consumer already subclassing the default-visibility, installed base. Instead plugins mirror the **existing `ExternalMCPClient` pattern**: external servers are likewise not `MCPServerBase` subclasses but a parallel `std::map` with their own branch in `route_tool_call`. `PluginServer` (RAII over the dlopen handle + nine resolved entry points, destroying the instance before `dlclose` since the destructor lives inside the library) becomes the third such peer, with `plugin_servers_` and matching branches in `list_tools`/`server_names`/`list_server_info`/`shutdown`/`get_tool_schema`. Consequences worth recording: (a) **`get_tool_schema` had to grow a plugin branch** — `ToolExecutor::check_schema` skips validation on an empty schema, so without it plugin tools would silently forgo the argument checking every built-in tool gets; (b) **plugin output is untrusted input** — a malformed tool list or response envelope is caught per-plugin rather than throwing through the agent loop; (c) **plugins load after builtins** in the facade so a name collision is rejected instead of shadowing a built-in server; (d) `RTLD_LOCAL` keeps two plugins' identically-named entry points from colliding; (e) returned strings are freed via *that plugin's* `entropic_free`, not the engine allocator. Separately, adding `ENTROPIC_EXPORT` to the nine declarations is **load-bearing, not cosmetic**: measured on GCC 11.4 under `-fvisibility=hidden`, a plugin defining its entry points bare (inheriting visibility from the header, the normal way to implement a C API) exports **0** symbols without it and all 9 with it. The consumer report's stated mechanism for this — that GCC warns and silently ignores a `visibility("default")` attribute on a definition whose declaration lacked one — does **not** reproduce; a definition-site attribute is honored, with no warning. The test fixture therefore defines its entry points bare deliberately, so it stays a genuine RED control for the header fix rather than passing either way. (v2.10.1, gh#133)

55. **Thinking-block removal is an adapter-owned concern, resolved per family and applied on every path (gh#108, v2.10.3).** Gemma-4 emitted `<|channel>…<channel|>` into `result.content` and the live stream on plain decode, streaming, MTP and batch alike. Root cause was structural, not behavioural: every family had an adapter stripping its own markers except gemma4, which `adapter_registry` deliberately omitted (tool calls go through llama.cpp's `PEG_GEMMA4`), so `adapter: gemma4` resolved to `GenericAdapter` — which strips `<think>`, a marker gemma4 never emits. The sole `<|channel>` handler (`strip_thinking_channels`) sat inside `parse_response`, reachable only when `common_chat_parse_reliable()` is true, i.e. `parse_params_valid_ && format == PEG_GEMMA4` — a gate whose actual purpose is "is this captured format multi-parameter safe?". Reasoning stripping had been bolted onto it, so it ran only for gemma4 AND only after a tooled render. **MTP/streaming/grammar were red herrings** — a plain-decode non-streaming control leaks identically, which also means the v2.9.1 MTP-streaming guard (#43) gated a feature over a defect in a different layer and never protected anyone; #53's `StreamThinkFilter` "fix" targeted `<think>`, the wrong family's marker, and no test caught it because no test drove `orchestrator->generate_streaming` with MTP on. Resolution: (a) `ChatAdapter::thinking_markers()` — each family declares its delimiters once, consumed by both `strip_think_blocks` (now marker-driven, plain substring since `<|channel>` contains a regex metacharacter) and `StreamThinkFilter` (markers injected by the orchestrator; core takes plain strings, no inference dependency), so buffered and live paths cannot diverge again; (b) `Gemma4Adapter` supplies the fallback that never existed, with `PEG_GEMMA4` still primary when an arena exists; (c) `response_parse.h` collapses the byte-identical branch duplicated in `orchestrator.cpp` and `interface_factory.cpp` into one template-first/adapter-second rule — **content cleanup composes** (adapter strip always runs, idempotent) while **tool-call extraction does not**, so the template result is validated against the staged tool schema and falls back to the adapter when required params are missing, which is the only way to detect `common_chat`'s silent first-parameter-only extraction (gh#87 Phase D — it returns a well-formed ToolCall, so no try/catch fires); (d) `ConstitutionalValidator` resolves markers per tier via a facade-supplied callback, since one validator serves all tiers but thinking format is per-family. Process note: the release that introduced #52/#53 shipped on a vacuously-passing test — it asserted the removed guard no longer fired rather than that streaming worked — so the RED for this entry deliberately drives the real production path and carries plain-decode/non-streaming controls that attribute a leak to the correct layer. (v2.10.3, gh#108)

56. **Grammar has three sources and one seam; UTF-8 has many exits and few entrances — fix at the choke point, not the instance (gh#134/gh#136, v2.10.4).** Two consumer defects, one shared cause: repeated per-call-site fixes for a class of bug that needed a single structural one. (a) **gh#134** — `common_chat_templates_apply` derives a tool-call GBNF from the staged schemas; `render_with_tools` harvested `prompt`/`format`/`generation_prompt`/`parser` and DISCARDED `grammar`, so every tools-staged tier decoded unconstrained and `tool_choice: REQUIRED` was inert. That is the third dropped-grammar instance after #48-era gh#95 (identity grammar dropped in the facade) and #52 gh#108 (`params.grammar` not reaching MTP). Fixed by capturing the render's grammar (invalidated in `do_unload` beside the parse snapshot per #54's stale-arena lesson) and applying it in `to_common_sampling` — the ONE seam plain decode and MTP share, so MTP is covered by construction rather than needing the follow-up gh#108 required. Critically it must be applied as `COMMON_GRAMMAR_TYPE_TOOL_CALLS`, never `_USER`: `common_grammar_needs_prefill()` is true only for the former, and `generation_prompt` must be prefilled into the grammar sampler because the model's output begins mid-template — applied as `_USER` the grammar rejects the first token and surfaces as `GENERATE_FAILED` with empty content, which is precisely what was measured before the prefill was threaded. `TierConfig::require_tool_call` exposes it, opt-in per tier. **Under REQUIRED the gemma4 rule is `zero_or_more(any) + tool_call`** — unbounded preamble, mandatory call — so the grammar guarantees a call *eventually*, not *within* `max_tokens`; a budget-starved REQUIRED turn ends `finish_reason=length` with zero calls and is now named explicitly, since it is a config error indistinguishable from a model stall otherwise. (b) **gh#136** — `type_error.316` recurred a fourth time after gh#112/113 were closed as "permanent closure of the family". Every fix patched one `.dump()`; there are ~18 in the facade alone. Sanitized at INGRESS instead — the three points where model bytes first become a `std::string` — which `response_generator.cpp:360` had already done for the streaming accumulator since v2.1.1 without the pattern ever being generalised. Safe because `raw_content` is a copy of `content` and sanitize only replaces bytes already invalid as UTF-8, which cannot form part of a valid JSON token. **General rule adopted: when a defect class recurs, count the exits and the entrances and guard whichever is bounded.** Process note: three of the four tests written for gh#134 passed vacuously (the model emitted the tool call regardless), and the discrimination metric compared prose counts rather than `finish_reason`, hiding a real effect — only a same-model, same-prompt, same-budget A/B with the flag as the sole variable discriminated. Every invariant added here therefore carries a control that must fail. (v2.10.4, gh#134, gh#136)

57. **A traceability catalog that fails open is not a gate — and headers describe intent, not behaviour (v2.11.0).** `docs/requirements.yaml` was deleted as collateral in a `docs/` purge at v2.1.0 and stayed gone for fifteen months of green commits, because all three of its consumers degrade silently when it is absent: `check_req_exists` self-disables on an empty catalog (`req_ids = set(...) or None` → `None` → returns `[]`), `check_req_coverage` verifies only that the config *names* a path and is satisfied by exemption tags, and impact reporting degrades to "No requirements affected." Even at its healthiest the catalog was 10 of 140 entries referenced from code — it was never load-bearing. Rebuilt bottom-up from the shipped implementation (eight parallel subsystem sweeps → 112 requirements → 105 after reconciling the cross-cutting rules each sweep restated at its own boundary), and made durable by `inv check-requirements`, which fails **closed**. Three properties that tool must check and doxygen-guard structurally cannot: (a) **`@req` on a bodiless header declaration is invisible to doxygen-guard for both coverage and cross-reference** — the parser skips declarations with no body, which is how a dead `REQ-INFER-003` survived in `i_inference_backend.h` after Phase 2 merged it into `REQ-ABI-001`, seen by no gate; (b) **exemption creep is invisible** — `_extract_tagged_function` never collects a function whose only tag is `@internal`/`@utility`/`@callback`, so the repo's 0.9% `@req` ratio was undetectable by the coverage command; (c) a ratio floor, not an absolute count, since deleting code lowers both numbers legitimately and only the proportion expresses the property worth defending. **The second lesson is sharper.** The sweeps were instructed that the implementation IS the specification, but they read headers and doxygen — which describe *intent* — and four times the intent had never been built: `entropic_set_error_callback` returned `ENTROPIC_OK` while discarding its arguments (the callback type is invoked from nowhere in the tree); `FileAccessTracker::was_read_unchanged` had no production caller, and the test named `..._detects_external_change` conceded in its own comment that the write succeeds; the bash/git command timeout is stored, logged and exposed but never enforced; and two catalog entries written from those headers asserted both. This is the same class as #54 (gh#133: a dlopen plugin contract documented from v1.8.5 with no loader). **Rule: a requirement derived from a header is a hypothesis until a test exercises the production path.** Any `verified_by` naming a test must be checked for whether that test drives the real call site or asserts on a helper directly — the filesystem one passed for years while the behaviour was absent. **Third, on method:** gh#137 was chased through five hypotheses reasoned from source (strip greediness, toolless render bypass, `enable_thinking` threading, template support, named channels) and every one was wrong; the strip provably matches the model's own jinja `strip_thinking` macro case by case. What settled it was measurement — three GPU runs across E2B and E4B QAT, all delivering 100% of their content — and what nearly derailed it was a self-inflicted `gpu_layers=15` copied from a note about a model three times the size, which aborted in `do_activate` and was briefly mistaken for the model being unusable (#139). Reading code proposes; only running it disposes. (v2.11.0, gh#137, gh#138, gh#139, gh#140)

58. **Two headers promised what nobody built, and the third told a consumer to build on it (v2.12.0, gh#143/gh#144/gh#145).** gh#144 arrived with an INVERTED premise — a careful reader concluded concurrent asks would queue — because `@threadsafety Serialized per-handle.` sits on all six run entry points and has been false since #52-era gh#109 removed `api_mutex` from them so a long turn could not block `entropic_interrupt()`. Meanwhile the bridge has served each client on its own thread since v2.1.2. Two concurrent asks therefore raced the shared conversation vector AND decoded concurrently on one `llama_context`. Alongside it, `ENTROPIC_ERROR_ALREADY_RUNNING` was declared in the enum, carried in the error-string table and documented as a returnable outcome on two entry points since v1.8.9 — and returned from nowhere in the tree; and `entropic_inference_generate_seq` has been on the plugin ABI since v1.9.13 with `do_generate_seq` silently delegating to the keyless variant, overridden by nobody and called by nobody. That is three more instances of #57's class in one issue, so **the rule generalises: a contract stated only in a header is a hypothesis, and the cost of a false one is not a stale comment — it is a consumer who designs against it.** Serialization was therefore implemented as *making the documented outcome occur* rather than as new API surface. The claim is a compare-exchange on the existing `running_flag_`, never a mutex, and `HandleTurnGuard` is a deliberate SIBLING of `HandleApiLock` rather than a subclass, because reusing that type would silently reintroduce `api_mutex` on the run path and re-break gh#109 — the single likeliest mistake in the refactor, so both headers say so at the declaration. Rejecting at the ABI while QUEUEING at the bridge is the split that satisfies both callers: a direct C consumer wants to be told, a shared host wants to wait, and a FIFO ticket queue (not a bare mutex) is what makes the wait fair, bounded and observable — a mutex offers no fairness, no depth and no deadline, and MCP clients carry their own timeouts, so blocking one indefinitely is worse than telling it the truth. **Non-unified KV was chosen for fail-fast, not for memory.** Verified in the vendored llama.cpp that total KV is `n_ctx` worth of cells either way (`llama-kv-cache.cpp:98,247`; `llama-context.cpp:229-243`) — unified is not cheaper, it trades `n_seq_max` private guaranteed streams for one shared pool in which a long session starves the others behind "could not find a KV slot", a runtime failure with no attribution to a session. `max_sessions` therefore DERIVES `n_seq_max`, `kv_unified` and `n_ctx` together rather than exposing three knobs an operator can set inconsistently, and `context_length` stays PER SESSION. **The KV correctness trap is worth naming precisely**, because the in-repo comment about it was wrong: `try_warm_reuse` claimed an interleaved conversation "either fails the occupancy gate or diverges in the prefix scan, and we fall back", and NEITHER branch fires — two sessions on one handle share a system prompt, so `common_prefix_len > 0`, so `cut > 0`, so the `seq_rm` branch destroys the other session's tail and, by returning true, skips the prompt cache as well. Latent only while one shared conversation meant one monotonic history; keying ACTIVATES it, which is why per-sequence residency had to land before the store. Two process notes. First, **the exception barrier for gh#143 was initially placed one level too low**: in `ToolRegistry::dispatch` it let `inject_anchor_if_needed` still run on a failed call, so a REJECTED path traversal was anchored into context as though it had been read — caught by the existing security test, and the reason the barrier now sits in `MCPServerBase::execute` above anchor injection. Second, **normalisation must not swallow caller error**: the first cut coerced ANY non-object argument payload to `{}`, and `EntropicServer`'s followup tool — which already answers a non-object with a typed "invalid args" naming the real problem — failed and was right to. Only empty and literal `null` mean "no arguments"; an array, a scalar or unparseable input reaches the tool so the sender is told. **Fourth, MTP was discarding a cache for no reason anyone had stated.** `mtp_init_run` called `llama_memory_clear(ctx_tgt, true)` and fully re-prefilled on EVERY generation, so under `speculative.mtp` warm-keep and the prompt cache never executed at all — a consumer observed ZERO warm-keep events across a three-turn review with MTP on, against substantial prefix reuse with the same workload MTP off. **The figures first recorded here — a prefill:generate ratio and an MTP-off percentage — were WITHDRAWN, and the reason is worth keeping.** That measurement ran over the external bridge while every external tool call was silently failing via gh#150, so both arms measured a degraded fallback path; and the error has no predictable sign, because dead tool results were tiny where live ones would have INFLATED prefill, while the extra flailing turns and one 557 KB directory listing pushed the other way. So the ratio is not merely imprecise, its direction is unknown. What survives is the mechanism question — whether retention engaged AT ALL — which `mtp_init_run`'s unconditional `llama_memory_clear` settles independently of what the context held, and which the consumer later confirmed on v2.12.1 (retention engaged on every turn with MTP active, against zero events before). **Rule: a consumer's measurement is evidence about their whole stack, not about the one variable you care about — check that the instrumented path was actually alive before reading a number off it.** Nothing about speculative decoding requires that: it was an implementation choice in one path, and it made two effects that should COMPOSE mutually exclusive. Retention uses the same rule as plain decode — a prefix match validated against the KV's own occupancy via `llama_memory_seq_pos_max`, never a software counter. **Fifth, and the methodological point of the whole release: this class of defect is invisible to CPU tests, and the instrumentation is what finds it, not the output.** Three separate times the assertion that mattered was a token COUNT, not an answer: (a) the session-pool bug — `restore_cached_prefix` restored the cached prefix into sequence 0 while decoding the remainder into the caller's slot, so every session but the one holding slot 0 attended to a fragment with no system prompt, and both it and `run_prefill` wiped every other session on any cold prefill — 1739 CPU tests passed throughout, because the single-session path cannot reach either bug when slot 0 IS the hardcoded sequence, and the giveaway was one session prefilling 17 tokens where another prefilled 38, not the wrong answers; (b) the first MTP RED was red for the WRONG reason, reporting 0 tokens per turn because `last_prefill_tokens()` was never instrumented on that path at all — `GenerationResult::prefill_tokens` had to exist before anything could be measured; (c) the isolation test's cross-contamination assertions passed VACUOUSLY while the feature was broken, because a session that recalls nothing trivially contains no other session's secret. **Rule: assert the instrumentation, never only the output — a correctness test passes via fallback even when the optimisation is dead.** Worth recording that four of the defects found in this release were in the TESTS (a wrong flatness assertion, a vacuous `>= 0`, a Catch2 leaf-section re-run that started four bridges on one socket, and an impatient client that reported a working FIFO queue as a failure) against two in the engine — and that the harness itself hid evidence twice: `inv test --model --filter` filtered only the CPU phase and then ran the entire model suite, and the model runner sent stdout and stderr to `DEVNULL` so a failing GPU test discarded every assertion message that said why. The GPU time is already spent; throwing away the diagnosis is the expensive part. (v2.12.0, gh#143, gh#144, gh#145)

59. **A performance claim measured in a configuration we later called a bug outlived the fix (v2.12.2, gh#106/gh#108).** Decision #42 records that MTP "on Pascal is lossless+functional but bandwidth-bound (low accept-rate, no net throughput win) — a modern-HW / ceiling-additive lever, not a floor win." That was measured at **v2.9.0, when `n_draft` defaulted to 16.** Decision #44 then changed the default **16→4** at v2.9.2 with the note "16 over-drafts the small head → net slowdown". So #42's Pascal figure was taken in precisely the configuration #44 identifies as a net slowdown, and **it has never been re-measured on Pascal since the fix** — the claim simply stayed in the log, where it reads as a property of the hardware rather than an artifact of a default that no longer exists. A consumer has since measured **~2x decode throughput** (76.7 vs 38.3 tok/s, temperature 0, warm trials) on this project's own GTX 1080 Ti at `n_draft=4`, against a **19% floor** established by a control of two IDENTICAL plain configs — and the figure agrees with what the measured accept rate predicts independently (0.62–0.68 at `n_draft=4` ⇒ ~2.6 target tokens per forward pass ⇒ ~2x after overhead), which is the part that makes it credible rather than the raw number. **Measured with NO active request grammar** — the consumer's tier named its grammar by bare stem, their harness wrote each arm's config where no matching `.gbnf` sat, and the registry failed open as documented (gh#154). The comparison survives, because both arms lacked it equally and were judged against a same-config control; the CONFIGURATION LABEL does not, and the standing claim is "~2–3x on Pascal at `n_draft=4`, unconstrained decode" rather than "~2–3x on Pascal". Grammar-constrained decode is that consumer's production configuration and a grammar plausibly moves the accept rate the speedup rests on, so the interaction is open, not settled — both accept-rate figures on gh#147 are now withdrawn and none was ever measured under an active grammar on this hardware. **This correction is rule one catching a claim recorded under rule one, one day later**, which is the most direct argument for the rule that could be asked for. **This is not yet OUR measurement**: the suite asserts MTP *engagement* (`n_drafted`/`n_accepted`) and has never asserted throughput, so #42's claim was never under test and its staleness could not surface as a failure. The consumer's route to it is instructive on its own: four earlier figures from the same harness (−5%/+16%, 36–39%, ~8%) were each withdrawn, and every one of them was the harness measuring itself — a cold trial averaged into warm ones, 1 Hz polling quantising a 2 s measurement, a control run overwriting the real run's logs. **The control is what changed the answer, not better analysis.** Two rules follow. First, *a performance claim must record the configuration it was measured in, and must be re-measured or retired when that configuration changes* — otherwise a fixed bug leaves its symptom behind as documented fact. Second, *an A/B without a same-config control measures the harness as much as the subject*: 19% between two identical configs is the scale against which any candidate effect must be judged, and 8% was inside it. **Third rule, and the one that explains all five withdrawn figures at once:** *a per-unit-of-work wall clock is not a throughput metric when the size of the unit is itself a variable.* A later run made this visible in a single line — three arms took 8186, 9721 and 7576 ms per review, near-identical, while the fastest emitted 2378 tokens against 958. Per-review time was measuring how much each arm decided to SAY, not how fast it said it. Every withdrawn figure divided by something that moved for reasons unrelated to the variable under test: ms/iteration when the arms took 19 and 14 iterations, ms/review when output volume differed 2.5x, a prefill:generate ratio when the tool layer was dead. Tokens per second is the metric that survives, because both of its terms are measured rather than assumed. #42's Pascal sentence is superseded pending our own instrumented measurement; gh#151 shipped the `tok/s` line on the speculative path that makes it readable at all. (v2.12.2, gh#106, gh#108, gh#151)

60. **llama.cpp b9886 → b11009: the bump was five mechanical API breaks, and the interesting finding is what the enum swap hides (v2.13.0, gh#148/gh#166).** `extern/llama.cpp` moved `20a04b220` (b9886, 2026-07-06) → `fb27a525d` (b11009, 2026-09-16). Motivation: the FlashAttention fix that #45 already relies on stays in, upstream #28549 puts CUDA graphs on the MTP draft path (a GPU-gate question, not reachable from a CPU build), and the pin had drifted far enough that the API renames were going to have to be paid eventually. **Five compile breaks, all resolved without a behaviour change**: (a) `llama_model_params::use_mmap`/`::use_mlock` are gone, replaced by a single `llama_load_mode` enum; (b) `llama_sampler_init_penalties` gained a leading `n_vocab` (fed `llama_vocab_n_tokens(vocab_)`, matching upstream's own call in `common/sampling.cpp`); (c) `common_speculative_draft_params::n_past` → `pos0` — verified a pure rename by diffing every use in `common/speculative.cpp`, which are line-for-line identical; (d) `mtmd_helper_bitmap_init_from_file` gained a trailing `mtmd_helper_init_opt` carrying video-decode params (`mtmd_helper_init_opt_default()` is the still-image behaviour); (e) `tools/mtmd` dropped `LLAMA_INSTALL_VERSION` for `LLAMA_VERSION_BASE`/`LLAMA_VERSION_MAJOR`, and since `extern/CMakeLists.txt` adds that subdirectory by hand it has to mirror the new names — an undefined `VERSION` property is not a CMake error, so this one fails silently rather than loudly and is worth re-checking on every future bump. **The `load_mode` mapping is the only one with a judgement call in it.** Three values could plausibly stand in for `use_mmap = true`: `AUTO`, `MMAP`, `MMAP_MLOCK`. `AUTO` is upstream's default and is NOT the faithful choice — it additionally probes every backend device for `caps.mmap_support` and silently downgrades to a full read when one says no (`src/llama-model.cpp:1444`), a probe that did not exist at b9886. So the mapping is explicit: `MMAP_MLOCK` when `ModelConfig::use_mlock`, `MMAP` otherwise, through one helper (`mmap_load_mode`) shared by all four load sites. **The field the enum swap quietly added is `lazy_mode`, and it defaults to ON.** `LLAMA_LAZY_MODE_AUTO` reads rows of arch-marked tensors over 4 GiB on demand from the mmap instead of materialising them — and the only two tensors marked `TENSOR_READ_LAZY` in the tree are `gemma4.cpp`'s `per_layer_tok_embd` and `qwen4exp.cpp`'s PLE block. Gemma 4 is this project's MTP family, so the new default lands squarely on the primary path: a lazily-read tensor takes `lazy_read::buft()` and stays host-side, which changes VRAM footprint and first-token latency rather than correctness. Nothing was set here — changing it would be a behaviour change beyond what compiling required — but it is a **GPU-gate item, not a settled one**: if E4B load time or VRAM moves, `lazy_mode = LLAMA_LAZY_MODE_OFF` is the one-line restoration of b9886 behaviour. The sibling field `load_mtp` (default false) gates MTP tensors that live INSIDE a target GGUF (deepseek, qwen3next, qwen35moe, glm4-moe, …); `gemma4-assistant.cpp` never reads it, so entropic's separate-head MTP is unaffected — but any future in-GGUF MTP target would need it set. **Tripwires re-validated by reading, not running.** #19 (`n_gpu_layers` is still a MODEL param, so WARM→ACTIVE must still reload), #28 (`llama_set_adapters_lora` plural, still no `llama_rm_adapter_lora`), #30 (`llama_sampler_init_grammar` still returns nullptr on parse failure), #34/#35 (`llama_memory_*` unchanged; `llama_state_seq_get_data` still has no range parameter, so the two-pass prefill stays the only honest way to cache a prefix), #39/#40 (`common_speculative_are_compatible` is still `static`, still has NO recurrent/hybrid check, and the four rules entropic mirrors — vocab type, BOS/EOS parity, `SPEC_VOCAB_MAX_SIZE_DIFFERENCE` 128, prefix text equality from `SPEC_VOCAB_CHECK_START_TOKEN_ID` 5 — are byte-identical), #44b (`swa_full` still present on `llama_context_params`), #45 (the GQA-2 MMA specialisation is still live in `fattn.cu`), #51 (`common_speculative_impl_draft_simple` still exists, so the classical path the predicate guards is still reachable), #56 (`common_grammar_needs_prefill` still true only for `OUTPUT_FORMAT` and `TOOL_CALLS`), #58 (`kv_unified` still makes `n_ctx_seq = n_ctx` while non-unified divides and re-derives, so total cells are `n_ctx` either way — the fail-fast argument, not the memory one, is still the reason to pick it). **#46 is the one that mattered and it holds**: `common_speculative_impl_draft_mtp::draft()` still proposes `cur_p->data[0].id` and still never reads `selected`, so the draft proposal remains a deterministic point mass and MTP stays lossless at any temperature. **Two drifts worth recording.** First, `common_speculative_accept` relaxed a hard `GGML_ASSERT(impl)` into an early return guarded by `GGML_ASSERT(n_accepted == 0)`. #43's zero-draft-round skip is therefore no longer load-bearing against an abort — it is kept anyway, because it is still correct and removing it buys nothing. Second, the separate-draft path grew recurrent/hybrid awareness (`need_boundary_stash()` stashes the boundary `g_embd` row because "their single-position checkpoints drop it on restore"), which is the first upstream movement toward the cross-ubatch state continuity that #41 declared structurally unreachable. That does NOT reopen #41 on reading alone — #41 was settled by measurement (the Gate A logit dump) and can only be reopened the same way. **On-disk state: the format constants upstream bumped are ones entropic never reads.** `LLAMA_SESSION_VERSION` (9→10) and `LLAMA_STATE_SEQ_VERSION` (2→3) are checked only by `llama_state_load_file` / `llama_state_seq_load_file`; entropic uses the in-memory `llama_state_seq_{get,set}_data` exclusively, and those carry a magic (`io_magic`, unchanged at `0xaf143cd8`) but **no version field at all**. `entropic_state_save` then writes that raw blob to disk with no header of its own — no magic, no version, no model identity. A stale blob is therefore caught only by llama.cpp's structural checks (layer count, K/V type, row size, and a bounds-checked read that throws and is converted to a `0` return), which surface as `ENTROPIC_ERROR_INTERNAL` — a typed error, so nothing is accepted silently in the corrupt case. The prompt cache is host-memory only and never crosses a process, so no stale blob can reach it. The residual hole is narrow but real and PRE-DATES this bump: a blob whose layout still parses is accepted with no version check, and the seq-scoped KV layout happens to be unchanged for plain-KV models between v2 and v3 (only the cell-ext gate moved from `n_pos_per_embd() > 1` to `has_cell_ext()`), so this bump does not exercise it. **If `entropic_state_save` is ever promoted past "opaque blob, same pin, same model", it needs its own header with a pin id — the llama.cpp version constants will not do that job for it.** (v2.13.0, gh#148, gh#166)

61. **The engine knew and the consumer could not ask — and a documented fail-open is only documented to us (v2.13.0, gh#154).** Two findings, one root. First, **no `GenerationResult` field had ever reached a consumer.** `prefill_tokens` (gh#144) was serialized nowhere, `throughput_tok_s` and the speculative `n_drafted`/`n_accepted` went to a log line and died there, `entropic_metrics_json` carried only `LoopMetrics`, and `iface_generate` hands the engine a bare string by design (#49). So every observability field this project has added since v2.9.0 was added for a reader that did not exist, and the one consumer who needed them estimated output volume from drafted/accepted arithmetic and published an upper bound. `entropic_metrics_json` now carries `generations[]` — one record per generation, appended at the tail every orchestrated path already shares (`record_generation`, called from both `generate` overloads, `generate_streaming` and each arm of `generate_batch`), so a new decode path cannot ship unrecorded the way a new grammar source used to ship unwired. **The record is deliberately not a transcript**: metric fields only, bounded ring of 64. Second, **grammar provenance.** `GrammarRegistry::get()` returns `""` on a miss, the engine warns, and the decode proceeds unconstrained — documented, and undetectable from outside, because constrained and unconstrained output have the same SHAPE whenever the prompt also describes the shape. The only distinguishing signal was the ABSENCE of a `Registered grammar` line, which is not something a consumer can assert on; three days of speculative-decode measurement were published against that state and withdrawn (#59). `GrammarSource` gains a fourth member, `tier`, and the sentinel test caught the addition exactly as designed. **`tier` and `request` differ ONLY in reporting** — both arrive as `GenerationParams::grammar` and are applied identically — which is a trap, because the application site compared `source == GrammarSource::request` and would have silently stopped applying tier grammars, reproducing gh#95 in the fix for its successor. The site now asks `is_request_grammar()`. `describe_grammar()` reads the SAME two inputs `apply_grammar_source` reads, through the same `resolve_grammar_source`, so the record cannot disagree with what the sampler did; a provenance field computed from a parallel rule would be a second thing to keep in sync, and #56 is the catalogue of that going wrong. **`source` and `resolved` are separate fields on purpose**: a key that was NAMED and produced no text is precisely the undiagnosable state, so the naming source is still reported with `resolved: false`. **The fail-open is replaced only where the answer is static.** A tier's `grammar:` stem is config, knowable the moment the config is read, so `config::validate_tier_grammars` rejects an unresolvable one with `ENTROPIC_ERROR_INVALID_CONFIG` naming the tier, the stem and every directory searched — raised after `thread_frontmatter_samplers` (the common spelling is an identity frontmatter key, absent from `TierConfig` until that step) and before `init_orchestrator`, so no model loads first, the same placement rule gh#156 established one commit earlier. *(Superseded within v2.13.0 — see the amendment at the end of this entry: "knowable" was true and "wrong" was not, and the refusal moved to first use.)* A RUNTIME `params.grammar_key` keeps failing open: it may legitimately name a grammar registered after configure, and that miss is now readable on the record instead. **The search-path rule is mirrored, not invented**: `<config_dir>/grammars` alone when it holds any `.gbnf`, plus `<data_dir>/grammars` otherwise, because the facade's fallback runs only when the config_dir load registered nothing — listing both unconditionally would accept a stem the registry will not hold. **The gate's first act was to convict this repository.** Three shipped identities — `scribe`, `compactor`, `benchmark_judge` — declared `grammar: grammars/<name>.gbnf`, and `data/grammars/` has only ever held `constitutional_critique.gbnf`; git history shows the other three files never existed. Those keys were inert from the day they were written, and gh#95 (v2.7.4) "fixed" the threading of exactly this field so it would reach `resolve_grammar_key` and silently miss. So the default config had been running three tiers unconstrained while declaring otherwise, and every gate was green. The keys are removed rather than the grammars invented, and a test now walks the shipped `data/prompts/` so the next dead key fails in CI instead of in a consumer's measurements. **Both bundled examples were also casualties, and in the informative way**: `explorer` and `pychess` each call `entropic_grammar_register_file` AFTER `entropic_configure_dir`, for keys their own tiers NAME — a post-configure registration can only ever run too late for a tier that is validated at configure. They now declare `config_dir: data`, so `data/grammars/*.gbnf` is loaded during configure and the runtime registration (which would now collide as a duplicate key) is deleted. That is the residual risk made concrete rather than hypothetical: a tier stem naming a grammar the consumer intends to register after configure is now rejected. The alternative — checking after the registry is built — puts the diagnosis behind a multi-gigabyte load, which is the thing gh#156 established one commit earlier that we do not do. (v2.13.0, gh#154)

**Amendment, same version, one commit later: the configure-time refusal is withdrawn, because it made a documented API impossible to use.** The residual risk named two paragraphs up was not residual — it was the workflow. `entropic_grammar_register` and `entropic_grammar_register_file` both begin with `check_orchestrator`, so neither can be called until `entropic_configure*` has returned; **registering a tier's grammar from the host application is therefore necessarily a post-configure act**, and refusing at configure left a consumer whose grammar lives in memory (or at a path the engine cannot discover, or is generated at startup) with no legal order of calls at all. The entry above noticed the two bundled examples doing exactly this and read it as two examples needing a fix; they were the sample. **The GPU gate then named the real one**: `tests/model/test_gh95_identity_grammar.cpp` — the regression test for the predecessor bug — is that same sequence, and it stopped passing. A gate that breaks the test for the bug it builds on is not reporting a mis-written test. **The error was a category error, not a placement error.** "Knowable at configure" was true: the stem does not resolve, and the engine can say so. What is NOT knowable at configure is whether that is WRONG — at that instant the only call that could fix it has not become legal yet. So configure now WARNS, naming every unresolved tier, its stem, every directory searched, and the call that resolves it; the refusal lands at **first use**, in `ModelOrchestrator::refuse_unresolved_tier_grammar`, where a run SELECTING the tier is the proof that nobody is going to register it. That run returns `ENTROPIC_ERROR_GRAMMAR_NOT_FOUND` and decodes nothing. The fail-open is still closed — an unconstrained decode still cannot happen silently — and the sequence the C API documents is legal again. **Placement inside the dispatch is load-bearing twice over**: the gate runs BEFORE `get_model`, so a doomed run costs no model swap and cannot prefill, and — because it needs no backend — it is provable in a CPU unit test with no GGUF, which is what `tests/unit/inference/tier_grammar_gate_test.cpp` is. It also forced `resolve_grammar_key` out of `resolve_and_stage` (which runs after a model is resolved) and into the gate, so there is still exactly ONE place the precedence rule lives: the gate calls it and then asks `tier_grammar_unresolved()` about what it recorded, rather than re-deriving the answer. **The asymmetry with `params.grammar_key` survives and is now the clearer for having a reason**: a per-call key may name a grammar the caller registers later and the CALLER chose it, so it fails open and reports `resolved: false`; a tier stem is selected by the ENGINE, so the caller cannot see the miss at all — that is what makes it an error rather than their problem. The bundled examples keep `config_dir: data` from the entry above: both orders are valid now, and churning them back would only lose a worked example of the discovery path. **The general rule**: *before making a config-time check fatal, ask whether the call that fixes it is legal yet.* A validation that is correct about the world and wrong about the clock reads exactly like a correct validation — every gate stayed green, the test suite the check shipped with passed, and only a test that predated the check caught it. (v2.13.0, gh#154)

62. **An ignore rule's anchor base is an index, not just a regex prefix — and the walk that builds the rule set never obeyed the rule set (v2.13.0, gh#161).** A consumer vendoring boost/opencv/rtabmap/pcl/eigen/ceres reported `filesystem.glob` taking **87,249 ms for one call**. `IgnoreMatcher::is_ignored` ran two `std::regex_match` against **every** rule for **every** path — 5,061 rules x 187,855 entries — because last-match-wins appeared to forbid an early exit. Three corrections, in order of how much they explain. (a) **Bucketing.** Every `Rule` already carried `base`, the directory of the `.gitignore` it came from, and `compile_pattern` already baked `^<base>/` into both regexes. So a rule from `deps/boost/libs/geometry/doc` provably cannot match `src/main.cpp`, and the bookkeeping to know that was already on disk — `by_base_` merely indexes positions in the ordered rule set, so the global source order survives. A path consults the root bucket plus one bucket per ancestor directory: 13 rules instead of 650 in the regression test, 26 regex evaluations instead of 1,300. (b) **Descending order, not a precomputed negation map.** The plan was to precompute "does any negation follow me" per index and stop when none can. Walking the applicable rules BACKWARDS is strictly better and needs no precompute: last-match-wins means the last matching rule decides, so the first match found walking backwards IS the decision. The k-way merge over the ancestry buckets is a handful of integer comparisons per rule — free against a regex. (c) **The discovery walk was the dishonest part.** `load_nested_gitignores` carried a comment claiming it skipped excluded directories; it walked the entire tree including `.git`, loading ~200 vendored `.gitignore` files at startup and again on every `set_working_dir`. It now prunes the skip list and any directory the rules loaded so far exclude — which matches git, since git cannot re-include anything below an excluded directory. The skip list moved onto `IgnoreMatcher` because two private copies of it are exactly how discovery came to walk `.git` while glob and grep pruned it. **The ordering consequence, stated rather than fixed:** `.explorerignore` is layered last so it can override gitignore, so its exclusions are not yet known during discovery and do not prune it. **And a bound, because none of the above is a guarantee:** the pre-2.13.0 caps counted MATCHES (500 glob, 100 grep), so a pattern matching nothing still visited every entry. `filesystem.max_walk_entries` (default 250,000) caps the walk itself and, when it fires, appends `{"truncated": true, "note": "walk truncated at N entries — narrow the pattern"}` as the last element of the result array and logs a warning. A silent truncation would be worse than the hang it replaces: the model would read a partial answer as a complete one. The regression tests assert **regex-evaluation counts**, never elapsed time — a timing assertion measures the machine, not the code.

63. **A documented flag that controlled nothing, and an eager path that skipped its own gate (v2.13.0, gh#157).** `ModelConfig::keep_warm` was documented "Pre-warm model at startup" from v1.8.0 to v2.12.2 and read by exactly one site — swap-out, where it picks WARM over COLD. The default tier loaded during `entropic_configure` regardless of it, so a consumer had no key that said "load on first use": two sumac hosts opened by one editor session each loaded the same 4.8 GB GGUF and held ~9.6 GB of an 11 GB card without ever being sent a run. **The fix is a new key, `models.defer_load` (default `false`), not a new meaning for `keep_warm`** — whose default is also `false`, so repurposing it would have made every existing consumer lazy without their asking, a behaviour change disguised as a doc fix. The more interesting half is what fixing it exposed: `activate_default_tier` called `backend->load_and_activate()` **directly**, so the one load every consumer performs bypassed the residency gate (#57), fired no `Loaded` event and recorded no footprint — the residency bookkeeping could see every load except the one that always happened. Both paths now go through `get_model`, so eager and deferred differ only in WHEN. Four things assumed a loaded default backend and were corrected rather than documented as limitations: (a) **adapter preload** moved from init to the activation of the owning model — the init-time version required the base model to be loaded, which is true of exactly one tier, so every tier on a different GGUF logged "model not loaded", skipped, was never retried, and the later swap failed with "not found or COLD" (a live bug, independent of this issue); (b) `entropic_model_has_vision` answers from **config** (declared capability or mmproj path), because the backend's `has_vision_` is set while building the mmproj context during activation and an unloaded tier therefore answered a confident, wrong `0`; (c) `entropic_state_save` / `_load`, `entropic_get_logprobs`, `entropic_compute_perplexity` and `entropic_adapter_load` **lazy-load** through the gate instead of throwing "model not active" — each now claims the handle's turn first (`HandleTurnGuard`), because a load must not land mid-generation; (d) `entropic_speculative_compat` still reports "no main tier loaded" when nothing is resident — left as is, since it is a query whose honest answer depends on residency. Deferred is opt-in precisely so a consumer whose first token latency matters keeps the old behaviour by doing nothing.

64. **Weights and conversations have different lifetimes, and the engine only knew how to end both at once (v2.13.0, gh#164).** A loaded model stayed in VRAM for the life of the handle: `ModelOrchestrator` evicts only when a DIFFERENT tier needs the space, so a laptop host that wanted its 13 GB back had to `entropic_destroy` — which also drops every session's conversation, re-runs configure and re-registers MCP servers. `entropic_release_model(handle, tier_name /* NULL = all */)` unloads and keeps everything else. Three decisions inside it are worth recording. (a) **Adapter registrations are PRESERVED.** The existing swap-out path calls `AdapterManager::unload_all_for_model`, which *erases* the entries — correct for a tier being replaced, wrong for one the consumer intends to bring back, since the next activation would then fail `swap` with "not found or COLD" (the same failure mode gh#157 found for never-preloaded tiers). Release frees only the `llama_adapter_lora*` handles (they belong to the model being freed) and leaves the name → path/scale registration COLD; `load()` re-initialises such an entry in place, while a name that still holds a handle is still refused as a duplicate. (b) **It takes the handle's turn claim, and refuses with `ALREADY_RUNNING` rather than serialising.** `swap_mutex_` guards tier swaps and residency mutations, NOT generation (orchestrator.h has said so since v1.8.2), so "wait for the mutex" would not have waited for the decode: unloading mid-generation frees the `llama_context` the decode is running on. The claim is the same compare-exchange the six run entry points take (#58/gh#144). (c) **Release-all re-ensures the secondary roles at the next cold activation.** `classify_task` warns "router not loaded" and falls back to the default tier; nothing reloaded a role on its own, so releasing everything would have silently and permanently degraded routing for the rest of the handle's life. `activate_and_track` now calls `activate_router`/`activate_draft`, both no-ops when the role is loaded. No idle timer ships: only the host knows what idle means for its users, and a wrong guess unloads a model a user is about to type at.

65. **The doubled memory was page cache; the doubled WORK was real, and only one of them was in the report (v2.13.0, gh#148).** gh#148 reported "closer to 1.5x the file size" for a partially offloaded model and attributed it to the WARM and ACTIVE residencies overlapping across the reload boundary. **That overlap does not exist** — `load_gpu_model` has freed the WARM model BEFORE the GPU reload since v2.7.0 (decision #19's follow-up), so the two are never simultaneously resident and what the reporter measured was the page cache holding the file it had just read. The waste sitting next to it is not a memory figure at all: `load_and_activate` ran `do_load` — the WHOLE file, `n_gpu_layers = 0` — and then immediately discarded that model and read the whole file again with the configured split. **One cold activation, two whole-file reads**, nothing consuming the first one. Measured with a deterministic counter of `llama_model_load_from_file` calls (the instrumentation shape of #62/gh#161's regex counter, chosen because RSS and wall-clock both depend on page-cache state — which is exactly why the original report could not be turned into a test): **2 before, 1 after**, and the CPU smoke's activation halved from ~8.7 s to ~4.5 s. `do_load_active` is a new virtual whose BASE implementation is the old `do_load` + `do_activate`, so every other backend and every mock is unchanged; `LlamaCppBackend` overrides it to read once. WARM → ACTIVE still reloads and always will (decision #19: llama.cpp binds offloading to the model load), and that reload is what `keep_warm` pays for. Two policy decisions travelled with it, both from the v2.12.0 oversized-model work that lived ONLY in the test harness while the engine took config verbatim: (a) **typed refusals** for `use_mlock` beyond `RLIMIT_MEMLOCK` (`ENTROPIC_ERROR_MLOCK_LIMIT_EXCEEDED` — pinned pages cannot be reclaimed, which is what turned partial offload from slow into OOM-killed) and for an offload on a card whose free VRAM cannot hold the compute buffers alone; and (b) **`gpu_layers: auto`**, an explicit opt-in that derives the split the harness used to compute privately. Both refusals are **floor-gated to models above 10 GB** — `use_mlock` defaults to TRUE, so an un-floored mlock rule would have refused the everyday case of a CPU-resident 4 GB model on a box with the usual 8 MB limit, a configuration that works. Judging a SPECIFIC partial split is deliberately not attempted: it needs a layer count the admission gate does not read, and guessing it refuses configurations that work — the same asymmetry that makes `estimate_footprint_bytes` report a partially offloaded tier as unpriceable rather than inventing a number. And `auto` derives the layer split ONLY: the harness also flipped `use_mlock` off behind the operator, which is the silent correction this engine refuses to make.

66. **Serialization was an accident of the run guard, so removing it had to make the serialization deliberate — and the audit is the feature, not the lock (v2.13.0, gh#158).** `ENTROPIC_ERROR_ALREADY_RUNNING` was a per-HANDLE compare-exchange (#58/gh#144), so a host serving several conversations from one resident model serialized every turn — and the consumer's measurement is the argument: 123 tok/s at one sequence against 353 at eight on the same weights via llama-server, because decode is bandwidth-bound and one weight read serves a whole batch. **But nothing else serialized `llama_decode`.** One `llama_context` is shared by every session and tier on a GGUF; `LlamaCppBackend::active_slot_`, `residency_`, the staged tool arena and the sampler chain are all ONE field per backend; `entropic_run_batch` opened by clearing the WHOLE KV cache and held a local cancel flag nothing in the tree could set. So the per-handle guard was not merely a policy about turns — it was the only thing making every one of those fields safe, and keying it per session removes that protection from all of them at once. **Three consequences, recorded in the order they matter.** **(a) The lock.** `ModelOrchestrator::generation_mutex_` is the ONE lock on the generation path, held across a whole `generate*` call rather than around `llama_decode` — which is what makes concurrent runs interleave at GENERATION boundaries (a side request waits for the current generation, not the whole multi-iteration turn) and what covers `active_slot_` and the arena, which a decode-only lock would leave racing. Order: `generation_mutex_` → `swap_mutex_` / `records_mutex_` → the backend's `transition_mutex_` / `mtp_mutex_`. It is **impossible to invert by construction rather than by discipline**: the lock is taken at exactly five sites, all entry points of one class, and no holder of an inner lock calls any of them — `get_model` takes `swap_mutex_` and RELEASES it before returning, so nobody holds it across a generation. It is RECURSIVE because `generate()` routes and `route()` → `classify_task()` decodes on the router, which is itself a generation entry point; a plain mutex would self-deadlock the first time a run routed. **(b) The default is off.** `concurrent_sessions` ships `false`. Per-key runs are safe only once every piece of per-handle mutable state a turn touches is per-run or locked, and shipping the concurrency on would make every existing consumer concurrent without their asking — the mistake #63/gh#157 explicitly refused to repeat with `keep_warm`. A racy default is strictly worse than today's honest serialization, which is also why the flag is the ONLY thing that changes behaviour: with it off the guard is handle-exclusive and v2.12.0 semantics hold exactly. **(c) The bug the audit found was the binding, not the lock.** The session key was a per-handle field read at `run()`; making it thread-local so two runs can each answer "which session am I" produced a subtler defect — a binding that outlived its run stayed on that thread and silently captured the NEXT unkeyed run, on any handle. The existing gh#144 tier-switch test caught it, because Catch2 re-runs a scenario per leaf section and the second run inherited the first's binding. The fix is that **the claim IS the binding**: `try_begin_turn` installs it and `end_turn` removes it, so there is no state whose lifetime is not a run's. **External-tool cancellation becomes per REQUEST for the same reason.** gh#150's `interrupt_external_tools()` trips every transport at once, which is right for "stop everything" and wrong for "stop session A" — it would hand session B an empty tool result indistinguishable from a real one, which is the defect gh#150 was filed for, arriving through a different door. Threading a token through would change `ToolExecutionInterface`, `ServerManager`, `ExternalClient` and `Transport` — four signatures across three `.so` boundaries — for a value already unambiguous from context: a run is a blocking call on the caller's thread and every tool call it makes happens on that thread. So the token is published per THREAD (`RunCancelScope`, modelled on `HandleLogScope`) and the transport polls it beside the latch. `entropic_interrupt` keeps the latch; `entropic_interrupt_session` never touches it. **And the batch.** `run_batched_decode`'s whole-cache clear and its hard-coded sequence 0 for arm 0 are both invisible to a correctness test — a session whose KV was destroyed still answers correctly, having silently paid a cold prefill — so the fix ships with a counter (`kv_full_clear_count()`, the instrumentation shape of #62/gh#161 and #65/gh#148) and a pure planner the production path consults, because a helper the code does not call proves nothing. **COMPLETING THE AUDIT, AND WHY THE DEFAULT IS NOW ON (v2.13.0, second pass).** (b) above said the flag ships off *until every piece of per-handle mutable state a turn touches is per-run or locked*. That is a finite list, so it was finished rather than deferred, and the default moved with it — `concurrent_sessions` now defaults `true` and remains as a KILL SWITCH, the one-line escape hatch for a consumer who hits a concurrency defect in the field. Resolved in this pass: **`token_counter_`** — its memo was an `unordered_map` keyed by the ADDRESS of a `Message`, written from a `const` method with no lock. Two defects in one field: an address is not an identity (the message vector reallocates, the allocator reuses slots, so a hit could answer for a message that no longer exists) and the unsynchronized insert is heap corruption rather than a wrong number. *Deleted*, not locked — `count_text` is one `size()` and one division, strictly cheaper than hashing a pointer, so the memo was never worth defending. **`per_tier_metrics_` / `last_metrics_`** — `metrics_mutex_`, and the fold was copy-pasted at two sites (`run()` and `run_loop()`'s tail) so it was extracted first; locking one would have left the other racing. `entropic_metrics_json` copied the map while a finishing run inserted into it, and an insert that rehashes relocates every bucket under the copy's iterators. Its `generations[]` ring (gh#154) was ALREADY correct — `records_mutex_`, returning a copy — which is the useful contrast: the field added THIS release was safe and the accumulator that has been there since v2.0.6 was not. **`context_anchors_`** — `anchors_mutex_`; handle-wide, but written from INSIDE the loop by the `context_anchor` directive. **`system_prompt_`** — `prompt_mutex_`; written on the API thread under `api_mutex` and read on run threads, which have held nothing since gh#109 (v2.9.5) took `api_mutex` off every run entry point, so `api_mutex` never covered that pair. **`sandbox_mgr_` + the repo-dir cache** — `sandbox_mutex_`, as ONE unit because the manager is constructed from the cached path. The check-then-emplace was a use-after-free, not a wasted snapshot: two delegating runs both see an empty `optional`, both `emplace`, and the second destroys the manager the first already returned. `SandboxManager::ensure_base_snapshot` has the same shape one level down and gets its own `base_mutex_` — two runs both copying the project tree into one `base_dir_` is a file-level race, not duplicated effort, and the lock is held ACROSS the copy because the second caller wants the finished snapshot rather than its own. **`ConstitutionalValidator`'s per-turn fields** (`current_tier_`, `current_tool_context_`, `current_tool_evidence_`, `current_system_prompt_`) — the sharpest find, and one the brief did not name: per-HANDLE members describing a per-TURN thing, set at `handle_hook()` entry. A mutex fixes the `std::string` race and leaves the real defect, which is session A's critique prompt built from session B's tool evidence and identity prompt. Made thread-local; the one path that legitimately crosses threads, `resume_retry()` from the consumer's thread after `paused_pending_consumer`, carries the system prompt forward in `PendingValidationState`. **The decode itself** — and this is the gap the first pass left in its own work. ba48877 gave each run a cancel token and taught the agent loop and the MCP transports to poll it, but `ResponseGenerator` holds `GenerationEvents{&interrupt_flag_, &pause_flag_}`, raw pointers to the HANDLE-wide flags fixed at construction, so the per-token poll and the batch cancel poller answered only to `entropic_interrupt()`. `entropic_interrupt_session(A)` therefore could not stop the generation it named — the longest-running thing a session does. Both sites now OR in this run's token (`generation_cancelled`), and the batch poller, which runs on its OWN thread where the thread-local is not installed, captures the token pointer on the run thread before spawning. The pause prompt's "no injection" path likewise raises THIS run's token (`cancel_current_run()`) instead of the handle-wide flag, which used to abort every concurrent session on one session's declined prompt. **`pause_flag_` stays handle-wide, deliberately.** The asymmetry with the interrupt is the argument: `interrupt_session(key)` can route because the engine owns the map from key to run, whereas `pause()` is raised from a thread that owns no run and names no key — routing it would need an `entropic_pause_session` that no consumer has asked for, and every consumer of pause today is a single-session TUI holding one conversation. What WAS wrong is that a fresh turn cleared it unconditionally, so starting run B silently un-paused run A: the same shape as the interrupt defect this issue was filed for. The clear is now conditional on being the only run in flight, which is byte-identical behaviour under `concurrent_sessions: false`. **TOOL EXECUTION, which was not in the brief and is the finding worth reading twice.** `StdioTransport::send_request` has held `io_mutex_` across write-then-read since v1.8.7, so two concurrent runs never interleave BYTES on the pipe — the obvious hazard was already handled. The real one is that **nothing ever checked that the response line answered the request line.** A request can be ABANDONED — a timeout, or `request_cancelled()` tripping mid-read, which gh#158 turns from an exceptional event into a routine one — and the server still answers; that answer sits in the pipe and is read as the result of the NEXT request. Serialized, that was a stale answer to yourself. Keyed, it is a cross-session leak of exactly the gh#150 shape: session A's interrupt hands session B the content of A's tool call, and B cannot tell. Two fixes, because one of them is recovery and the other is detection: the transport marks the pipe desynced on an abandoned read and drains orphaned lines before the next write, and `ExternalMCPClient` now stamps and VERIFIES the JSON-RPC id, returning a typed error rather than another request's payload. `next_id_` became atomic in the same pass — a plain `int++` from two run threads (and from the HealthMonitor's reconnect thread, which has issued `initialize`/`tools/list` through it since v1.8.7) is a data race, and two requests sharing an id is precisely what defeats the pairing check. **What was checked and deliberately NOT changed:** `ServerManager`'s four registry maps have no lock, but two concurrent runs only READ them, and concurrent reads of a `std::map` are safe. The writer is a runtime `entropic_connect_mcp_server` / `disconnect` landing while a turn is in flight — an API-versus-run race that has existed since gh#109 (v2.9.5) removed `api_mutex` from the run path, is not made worse by keying the guard, and whose honest fix is `shared_ptr` ownership so `execute()` can hold a reference across a call it must not hold a lock across. Recorded here rather than half-fixed. `ToolExecutor` needed nothing: its only shared mutable state is `history_` (its own `shared_mutex`) and `history_seq_` (atomic); everything else is `LoopContext`, which is per-run already. **ThreadSanitizer is the proof, and getting it to run was itself a finding.** The `tsan` preset existed and could not build: on Linux >= 6.5 the kernel hands out 32 bits of mmap entropy and GCC's TSan aborts with `unexpected memory mapping` before `main()`, so `catch_discover_tests` — which RUNS each test binary at build time — failed the build outright. `sysctl -w vm.mmap_rnd_bits=28` fixes it and needs root; `setarch -R` disables ASLR for the process and needs nothing, so the preset now sets `CMAKE_CROSSCOMPILING_EMULATOR=/usr/bin/setarch;-R`, which both Catch2 and ctest honour, scoped to that preset alone. The run found two races the CPU suite cannot see, and neither was in the state this audit set out to lock. `StdioTransport::close()` closed the three fds and THEN joined the stderr pump, so `close_fd` wrote `stderr_fd_ = -1` while the pump was still reading it — a data race on an int and a use-after-close waiting to happen, since a freed fd number is immediately reusable by another thread's `open()`; the join now precedes the closes. And `MockInference`, the test HARNESS, raced on `generate_call_count++` and `response_queue.erase()` once one engine served two runs — worth recording because, left alone, a harness race reads as a finding about the engine. After both, 15/15 gh#158 tests pass with zero warnings, and the newly locked engine state is clean. **THE BUILT-IN SERVERS' OWN STATE (v2.13.0, third pass).** The first two passes locked the engine and the external transports and never opened the in-process servers — and `concurrent_sessions: true` means every UNBOUND session on a handle shares its ONE default server set, so two sessions can be inside the same `FilesystemServer` at the same moment. Three findings, and the one worth reading twice is a wrong answer, not a crash. **(1) `FileAccessTracker` had no lock.** `read_file` inserts into an `unordered_map` that `write_file` / `edit_file` look up; TSan: 7 races in `record_read`. A leaf mutex inside the tracker. **(2) The sandbox swap was exclusive against every other swap and against nobody's tool calls.** gh#160's facade `recursive_mutex` was taken by `ScopedSandbox` entry and restore only, so while session A's isolated delegation had the default set re-rooted, session B's `read_file("notes.md")` and `bash cat notes.md` resolved INSIDE A's sandbox — deterministically, the test reads `MARKER-sandbox` back — and B's writes would have landed there and shipped in A's patch. The swap itself was also an unsynchronized `std::filesystem::path` write plus an `IgnoreMatcher::load` racing B's `resolve_path` / `is_ignored` / bash's default cwd (4 TSan races in the deterministic scenario, 13 under swap churn, where all 30 of B's read batches saw the sandbox in at least one of their two calls). So the answer to "does B wait, read A's sandbox, or race?" was *both of the last two*. **The fix is `ToolRootLock` on the `ServerManager`**: SHARED around every in-process and plugin dispatch, EXCLUSIVE from sandbox entry to restore (`enter_working_dir` / `leave_working_dir`, and `set_working_dir_all` for its own duration), re-entrant for its owner, and the owner passes through its own shared side — the sandboxed child's calls are the ones that SHOULD see the sandbox. B now WAITS, and then resolves against its own root; the wait is logged by tool name before and after, so a stall behind another session's delegation is not mistaken for a slow tool. External servers take nothing — the swap never moves them. The facade's `sandbox_swap_mutex` and `EntropicWorkspace::swap_mutex` are deleted: one lock, living with the state it guards, and one per `ServerManager` is still one per workspace. **The honest cost**: under `delegation.isolation: sandbox` (default `none`) other sessions' in-process tool calls on the SAME server set stall for the length of a sandboxed delegation. The parallel path is a workspace per session (gh#166). A per-call thread-local root would give the built-ins back their parallelism but not the dlopen plugins, which hold their own cwd behind a C ABI that cannot see a thread-local — two mechanisms for one invariant, so not done. **Lock order**: `ToolRootLock` is OUTERMOST — exclusive, it is held across a child's whole run, so `ToolRootLock → generation_mutex_ → swap_mutex_ / records_mutex_ → transition_mutex_ / mtp_mutex_`; shared, it is taken only at tool dispatch on a run thread that holds none of those nor `api_mutex`, and nothing inside a dispatch reaches a generate entry point, so no inner-lock holder ever waits on it. No shared→exclusive upgrade can happen on one thread: a delegation starts after its tool batch returns (`dispatch_pending_or_halt`), never inside `ServerManager::execute`. **(3) `TodoTool::items_` — outside the three servers the brief named, found by the same sweep.** The entropic server keeps its todo list on the tool object, one per server set; two sessions appending raced `std::vector::push_back` and the TSan run ABORTED with an impossible allocation size inside it — heap corruption. The action and the render now run under one hold of a leaf mutex. **Two semantic questions are left open, deliberately, because they are design and not locking:** the read-before-write tracker and the todo list are per SERVER, so session B may overwrite a file only session A read, and B's todo result lists A's items — the per-handle-describing-a-per-session-thing shape the validator fields had, now locked but not re-scoped. **RE-SCOPED PER SESSION (v2.13.0, fourth pass — the user's decision on those two questions).** Both are now keyed by the running session. **The key is the one the call ROUTED on**: `ToolExecutor::execute_tool` publishes `LoopContext::session_key` to its own thread (`RunSessionScope`) for the duration of the dispatch, and the tracker and the todo tool read `current_run_session()` — so the state is filed under exactly the value that chose the server set, rather than a second notion of "which session" that could drift from it. `HandleTurnGuard` already publishes the same key for the whole run; republishing it at dispatch is what makes a direct `ToolExecutor` driver (the white-box tests) and a delegated child agree with production by construction. **A delegated child shares its parent's key** (`build_child_context` / `build_resumed_child_context` copy it), so a child's read legitimately unlocks the parent's write — kept deliberately, and pinned by a test that drives a real `DelegationManager`. **Bounded growth**: a session's entries are released when its conversation ends — `entropic_session_drop`, `entropic_session_context_clear`, `entropic_session_context_set` (a restore replaces the history the reads described) and `entropic_context_clear` (the active session) — through `release_session_tool_state`, which visits the default set AND every workspace's, because which set a session touched depends on when it was bound and releasing an absent key is free. The model-facing argument for releasing on clear is the same as the correctness one: read-before-write means *this conversation has seen the file*, and a cleared conversation has not. **Mechanism**: a `SessionScoped<T>` map (one leaf mutex, a `peek` that never allocates — asking "did this session read X?" must not grow the map for sessions that never read) and a `SessionStateOwner` mixin that `ServerManager::release_session` cross-casts to. **Not a virtual on `MCPServerBase`**: its vtable is plugin ABI (`i_mcp_server.h`), so a server opts in beside the base instead. `release_session` takes no `ToolRootLock` — the state is behind its own leaf lock, and waiting on the root lock would stall an API call for a whole sandboxed delegation. **Checked while here, not changed:** `AgentEngine::context_anchors_` is handle-wide and re-injected at every run start, which would leak one session's `file:` / `todo_state` anchors into another's turn — but tool-emitted anchors never reach it (`build_directive` returns nullptr for `context_anchor`, so the directive is dropped), so there is nothing to leak today. If that path is ever wired, the map must become per session first. `DelegationManager::set_todo_callbacks` (save/fresh/restore around a child) has no caller in `src/`, so a child's todos land on the parent session's list — the pre-existing behaviour, now per session.

    | Server | State | Written by | Verdict |
    |---|---|---|---|
    | filesystem | `root_dir_` | ctor; `set_working_dir` (sandbox swap) | was race + wrong root → `ToolRootLock` |
    | filesystem | `ignore_` (`rules_`, `by_base_`) | ctor; reload in `set_working_dir` | was race → `ToolRootLock` |
    | filesystem | `IgnoreMatcher::regex_evals_` (#161) | every match | atomic; cumulative across sessions (instrumentation only) |
    | filesystem | glob / grep `WalkBudget` (#161) | per call | per-call (stack) |
    | filesystem | `tracker_` | `read_file` | was race → leaf lock (46d3474); was per SERVER → per SESSION, released with the conversation (fourth pass) |
    | filesystem | `config_` — mode, `outside_root_allow` / `_deny` (#72); `max_read_bytes_` | ctor | immutable after construction |
    | filesystem | `approver_` / `approver_data_` (#72) | configure | `approver_mutex_`, copied and called outside it; the facade slot behind it is `path_approval_mutex` |
    | bash | `working_dir_` | ctor; `set_working_dir` | was race + wrong root → `ToolRootLock` |
    | bash | `timeout_`, the tool | ctor | immutable (`timeout_` is never enforced — not a concurrency finding) |
    | git | `repo_dir_` | ctor; `set_working_dir` | was race + wrong root → `ToolRootLock` |
    | all | tool objects, `registry_`, `name_` | ctor | immutable after construction |
    | plugin | the plugin's own cwd | `set_working_dir` | `PluginServer::call_mutex_` (bytes) + `ToolRootLock` (which root) |
    | diagnostics | `root_dir_` | ctor | immutable — never re-rooted (no `set_working_dir`) |
    | entropic | `TodoTool::items_` → `lists_` | `todo` | was race (heap corruption) → leaf lock (095a2f2); was per SERVER → per SESSION, released with the conversation (fourth pass) |
    | entropic | `state_provider_`, tier lists | configure / ctor | immutable after construction |
    | manager | `PermissionManager` lists | ctor | immutable in production — `add_permission` has no caller |

67. **A read nobody could write back is a read that was allowed to be lossy (v2.13.0, gh#165).** gh#144 gave consumers `entropic_session_context_get` and `entropic_session_list` and no counterpart, so a session could be read out but not restored: a host that stopped the engine to free VRAM lost every conversation, and the only way to put messages back — `entropic_run_session` — appends and then RUNS, re-executing the whole agentic loop per message, tool calls included. `entropic_session_context_set` is the missing half, and pairs with #64/gh#164 to give weights and conversations independent lifetimes, which is what a long-running local assistant actually needs. **The interesting finding is what the missing write was hiding.** `facade_json::serialize_messages` emitted ONLY `role` and `content` — it dropped `metadata` and `content_parts` — and that had been invisible for eighteen months because nothing ever fed the output back. `metadata["tool_name"]` is read by `context_manager` (twice), `compaction`, and the engine's tool-result folding; `metadata["is_context_anchor"]` marks the messages `reinject_context_anchors` preserves. A restore through the lossy serializer would therefore have produced a conversation the engine could no longer reason about — tool results indistinguishable from prose, anchors that no longer re-inject — and every assertion about message COUNT and CONTENT would still have passed. This is #57's rule in a new form: **a serializer is a contract with a reader, and a format with no reader has never been tested.** The byte-identical get→set→get assertion is the shape that catches it, because it is the only assertion that fails on a field nobody thought to check. The emission is ADDITIVE, deliberately: the new keys appear only when there is something to carry, and `content` stays a STRING on every message, so a consumer reading role/content sees byte-identical output for every message it has ever seen. Parts ride in their own `content_parts` key rather than turning `content` into the inbound multimodal ARRAY shape — the alternative would have been lossless and would have broken every consumer that reads `content` as a string. **Two scoping decisions.** (a) The refusal is per SESSION, not per handle: replacing the conversation a turn is appending to is the race, and a busy HANDLE is not. `conversations_` gains its own mutex, which also closes a latent race that predates this issue — the session APIs take `api_mutex` but NOT the run guard, so `entropic_session_context_clear` and `_drop` could already mutate the map mid-turn. (b) **What a restore invalidates, and why that is sufficient.** The session's resident KV tokens and its sequence slot are dropped. Warm-keep would have been correct without this — `warm_keep_cut` matches TOKENS and `seq_rm`s the divergent tail, so reuse is prefix-correct whoever produced the prefix — and the invalidation is kept anyway, because that gate is token equality rather than conversation identity: a future change that coarsened it (reuse by message count, by a conversation hash) would silently decode a restored session against the history it replaced. The prompt cache needs nothing, being keyed by prefix hash — a changed prefix misses, an identical prefix hits on identical tokens. The cost is one cold prefill on the restored session's next turn, which is exactly what an LRU-evicted session already costs. **The issue's own table was wrong** and is corrected here rather than quietly: it lists `entropic_session_list` as returning the message array for all sessions; it returns `[{key, messages: <count>}]`.

68. **A callback with no caller is not a feature with a bug — it is a documented guarantee that was never in force (v2.13.0, gh#160).** `DelegationManager::set_dir_swap` shipped in v2.1.5 and, in `src/`, was never called by anything. All three `ScopedSandbox` sites therefore took the `swap_dir_fn_ == nullptr` branch, so every delegated child ran against the REAL working directory while the engine dutifully snapshotted a project tree, diffed it against itself and delivered a 0-byte patch — and `entropic.h` told consumers "the engine never writes to the user's project directory; the consumer applies patches with user consent" the whole time, while `data/tools/entropic/delegate.json` told the MODEL the opposite ("Filesystem state IS shared via the parent's worktree (no isolation)"). Two documents, one codebase, opposite claims, and the tool JSON was the accurate one. **The fix is the switch, not the default.** `delegation.isolation: none|sandbox` defaults to `none`, which is what every release since v2.1.5 actually did; turning it on by default would park every delegated edit in `pending/<id>.patch` for the many consumers that register no `on_complete` callback, which is a silent loss of work, not a safety improvement. With `none` the sandbox is not constructed at all — the wasted snapshot gh#160 reported disappears along with the 0-byte patch that was its only product, the one behaviour that is NOT byte-identical to v2.12.2 and the one nobody can have depended on. **Three findings sat underneath the missing call.** (a) The snapshot source was the ENGINE's repo dir while the tools used `mcp.working_dir`; those are different paths for any consumer whose launcher cwd is not the target repo, which is the reporter's exact configuration, so even a wired swap would have diffed the wrong tree. The root now comes from ONE seam, `AgentEngine::resolve_session_root(session_key)`, which the facade answers because only the facade can see the `ServerManager` — the same shape as decision #23's `TierResolutionInterface`, and the single function gh#166 re-points at a named workspace. (b) Sibling delegations at one depth both minted `d<depth>`, so the second `create_sandbox` reused the first's directory and `pending/d1.patch` overwrote the first's patch: a lead fanning out to two specialists silently kept one result. Ids are minted from a counter on the engine-scoped `SandboxManager`, because the `DelegationManager` is per-delegation and cannot count its own siblings. (c) `ScopedSandbox` restored to the repo root unconditionally, so a NESTED delegation handed the outer child the user's working tree the moment the inner one finished — the guarantee inverted exactly where it mattered most. The restore target is now the parent's active root, carried on `LoopContext::active_root`. **The concurrency consequence is the honest cost.** `set_working_dir` is one field on each in-process server, and `concurrent_sessions` now defaults true (#66), so two sandboxed delegations on one handle would interleave their swaps and write into each other's sandbox. The facade takes a per-handle recursive swap lock from `ScopedSandbox` construction to destruction: sandboxed delegations serialize, unsandboxed ones are untouched, and gh#166's per-workspace server instances are what give the parallelism back. *(Amended by #66's third pass: that lock now lives on the `ServerManager` as `ToolRootLock` and tool dispatch takes it too, shared — a swap lock only swaps took left another session's tool calls resolving inside the sandbox.)* **And what cannot be contained is refused rather than downgraded.** An external stdio/SSE MCP server is a separate process with its own cwd; nothing the engine does moves it. If a sandboxed child can reach one whose `tools/list` does not assert `readOnlyHint: true` — absence is not a promise — the delegation is rejected with a typed message naming the tool, because a containment claim that is quietly false is worse than one the consumer is told it cannot have.

69. **The API made the expensive mistake easy and the cheap one impossible (v2.13.0, gh#162).** A lead that had already cited `app/include/b12/control/slam/SlamConfig.hpp` delegated a follow-up about the same subsystem with no path in it, because there was nowhere to put one: `task` was the only channel. The read-only child could not search, guessed at `src/slam`, and answered from inference about a file that does not exist — 553 s against 144 s for the same model and tools with no delegation, and the second question unanswered. **A prompt rule is not a fix for a schema gap**, as the reporter found: the rule helped and the schema still accepted the contextless call. So `delegate` and `pipeline` take `context: [{path, lines?, note?}]`, rendered as a `[CONTEXT]` block AHEAD of the task in the child's opening message — a small model that reads the task first has already started searching by the time it reaches the references. **References, never excerpts.** Inlining file content would copy the parent's context into the child's, which is the cost delegation exists to avoid; what the child cannot do cheaply is FIND the path again, and that is the only thing being handed over. The same block is written into the delegation's storage row, so "what did this child know" is answerable after the fact rather than inferred from a transcript. **Structural inability gets a structural refusal.** A tier configured `requires_context: true` — a reader with `read_file` and no search tool — refuses a contextless delegation at the TOOL boundary, as an error result with no directives. That placement is the point: the lead sees it inside the same turn and can re-issue with a path, where an engine-side rejection would arrive as a user message after the directive round trip, and a burned child loop would arrive as a confident wrong answer. A pipeline is refused the same way if ANY stage requires context, because one context list seeds them all. **And `resume_delegation` now takes a tier.** Reaching it used to require `followup` first purely to learn a storage id — two round trips on a slow local model before any work starts — so `target` resumes that tier's most recent COMPLETED delegation, with the id resolved by the engine at admission because the tool cannot see storage. Writing the test found the wiring bug that made the feature a no-op: `execute_pending_delegation` entered the resume path only when the id was non-empty, and resume-by-target arrives with it empty BY CONSTRUCTION, so the request silently ran as a cold delegation. **Explicitly rejected: auto-scraping the parent's file:line citations into every child.** It looks like the same feature and is not — the lead's citations are a mix of what matters and what it happened to touch, and a child that is handed the second kind reads it as instruction. The lead choosing what to pass is the signal; harvesting would destroy it.

70. **One resident model, many repositories — and the routing key was already on the thread (v2.13.0, gh#166).** `mcp.working_dir` was fixed at configure time, so serving a second repository meant a second handle and a second copy of the weights: impossible for a 13.27 GiB model on a 16 GB card, and unnecessary, because the model has no reason to care which repository a session is about. A workspace is `{root, its OWN built-in + plugin server instances, its own external servers, its own sandbox and swap lock}`, and the reason it is an OBJECT rather than a path is that **a server holds one working directory**: `FilesystemServer`, `BashServer` and `GitServer` take their root at construction, so two repositories cannot share one set, and `set_working_dir` is a field write that gh#160 already had to serialize. Weights, tiers and identity stay handle-wide, which is the entire point. **Binding is once.** A session that already holds messages is refused a rebind: its conversation cites paths relative to the root it was built in, and silently re-rooting it would make every one of those citations wrong without a single error. An UNBOUND session resolves to the handle's default servers, so a consumer that binds nothing sees no change at all. **The interesting constraint was the tool PROMPT.** Tool dispatch could route on `LoopContext::session_key`, which it already carries, but the callback that answers "which tools does this turn see?" is `get_tool_prompt(tier)` on `i_inference_callbacks.h` — an interface header that does not change without a proposal. gh#158 had already solved this exact shape for its cancel token and written down why (decision #66): a run is a BLOCKING call on the caller's thread, and everything it does happens on that thread. So the session key is published per thread by `HandleTurnGuard` and read by the facade's tool-prompt callback — `RunSessionScope`, the same mechanism as `RunCancelScope` and `HandleLogScope`, not a new one. **gh#160 paid for itself here.** Its `resolve_session_root(session_key)` seam was re-pointed and nothing else in the delegation path moved: sandbox roots, restore targets and the readOnlyHint refusal all followed, and the per-handle swap lock became per WORKSPACE, which is what lets two sessions in different repositories delegate in parallel instead of serializing on each other. **What is deliberately not done.** Per-workspace `app_context` is a follow-up. A workspace's external servers now spawn with `cwd` = its root (`posix_spawn_file_actions_addchdir_np`, with the pre-2.13.0 inherit-the-host's-cwd behaviour and a log line where that call is unavailable) and connect at workspace creation, so their `readOnlyHint` annotations already exist by the time gh#160's delegation gate reads them — no lazy spawn at the gate. Metadata lookups in `ToolExecutor` (schema, permission level, duplicate-skip) still consult the default manager: they are workspace-invariant for the built-ins and fail SAFE for anything else (unknown tool ⇒ WRITE required), while `execute` — the call that touches the filesystem — routes.

71. **A prompt that cannot fit the tier context is REFUSED, before any decode (v2.13.0).** The release gate for this version staged 27 tools (18,594 bytes) into a tier configured `context_length: 2048`, rendered 5,026 tokens, and llama.cpp answered every turn with `Decode chunk failed (slot=0, start=287, off=1536, chunk=512)` and `Cache restore failed, falling back to full prefill` — while forty assertions in that test passed. The engine knew both numbers and proceeded anyway. **The check lives in `LlamaCppBackend::run_prefill_cached`**, which is the one point every text decode path reaches immediately after tokenizing its render and the last point before `llama_decode`: it is the earliest place holding BOTH the rendered prompt size and `context_length`, and by the time it runs the engine's `check_compaction` and `prune_old_tool_results` have already done everything they can, so what it measures is the IRREDUCIBLE prompt. The staged tool block is the part compaction provably cannot shrink — it is rebuilt from tier config on every render. **The rule is the unarguable one**: refuse when `prompt_tokens >= context_length`, i.e. zero positions left to emit into. It deliberately does NOT reserve room for `max_tokens`; a prompt that fits but leaves less headroom than the caller asked for is a "length" finish, and refusing it would break every caller that passes a generous `max_tokens` against a small window. **The error names the knob, not just the total.** `ENTROPIC_ERROR_EVAL_CONTEXT_FULL` ("Sequence exceeds model context window", v1.9.10) already meant exactly this, so no new code was added to the public header; the message carries the token count, the `context_length`, the overshoot, the largest contributor and the per-part breakdown, because tool block / system prompt / history have three different fixes. **Explicitly rejected: trimming tools, truncating the prompt, or raising the context to fit.** All three are silent clamps, and a silent clamp is what made this cost a release gate instead of a config edit. **The refusal is terminal for the loop**, carried as `finish_reason: "context_overflow"` rather than `"error"`: `"error"` is not terminal, so it fell through to the empty-turn ladder, which appends "you must end every turn with exactly one tool call, retry" — making a prompt that already did not fit LARGER — three times, and then reported `zero_tool_calls_with_explicit_completion`, sending the operator after a model that was never given a chance to answer.

72. **A boundary nobody chose is not a boundary — outside-root file access now asks, and the answer can be written down (v2.13.0).** `mcp.filesystem.allow_outside_root` was a bool whose C++ default was `false`, but `data/default_config.yaml` shipped it `true` — and the bundled default is what every consumer without its own config runs, and what seeds an auto-created `~/.entropic/config.yaml`. So the effective default gave the model unconfined READ and WRITE of the whole filesystem through `read_file`, `write_file`, `edit_file` and `list_directory`; `ef6516d` had already had to force workspaces back to confinement because that default let one workspace read another's files. **The user's design, implemented as ratified:** (1) the key is tri-state, `true` | `false` | `optional`, with `true`/`false` parsing to exactly their old meaning and **`optional` the default in BOTH the C++ type (`OutsideRootAccess::optional`) and the bundled YAML**; (2) under `optional` an escaping path goes to the host's approver with the canonical path, the root, the fully-qualified tool and READ/WRITE, and with no approver registered the call is refused with a typed `outside_root_approval_required` message that names the path and the three ways to configure access — fail loud, never open; (3) two lists, `outside_root_allow` (pre-approved, no prompt) and `outside_root_deny` (always refused, even under `true`), **deny beats allow beats the tri-state**, matched by canonical SUBTREE through the same `lexically_relative` test the root uses, `~`-expanded, with a relative entry or an unknown mode failing the config load; (4) a named workspace's servers stay HARD-confined — `confined_mcp_config()` forces `false` AND clears the allow list, and the approver is wired onto the handle's default set only, so a sibling workspace can never become "just approve it"; (5) **`permissions.auto_approve` does NOT approve a root escape** — it exists to skip per-tool permission prompts, not to widen the filesystem boundary, and an auto_approve host that wants outside access says so with `outside_root_allow` or `allow_outside_root: true`. Item (5) was the implementing session's call, not the user's, and is recorded as such so it can be overruled. **There was no approver to reuse.** The brief assumed the tool executor's `on_tool_call` was one; it is an internal `EngineCallbacks` slot that returns `void`, cannot deny (`check_approval` sets `approved = true` after calling it), and has no facade setter, so no consumer could ever register it. A decision surface therefore had to be added: `entropic_set_path_approval_callback` with `ent_path_approval_request_t` and the existing `ent_decision_t`, stored on the handle (REQ-API-010: pre-configure registration survives) and read per call by a facade thunk installed on the default set's `FilesystemServer`. It is deliberately scoped to the filesystem boundary rather than retrofitted into a general per-tool approval API, which is a different and larger design. **Refusals are still thrown,** not returned as structured JSON like the other filesystem errors, because `MCPServerBase::execute`'s barrier is what skips the ContextAnchor on a refused read (gh#143); each new refusal leads with a stable type token and keeps the pre-2.13 "Path escapes project root: <path>" phrase, and legacy `false` is byte-identical. **The lists govern outside-root paths only** — the literal design — so a deny entry inside the root is WARNED about at server construction and on every re-root rather than silently doing nothing. **Not migrated:** a `~/.entropic/config.yaml` auto-created from an older bundled default still says `true`, explicitly, and keeps meaning it; the server logs that posture at WARN.
