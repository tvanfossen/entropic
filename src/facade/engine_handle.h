// SPDX-License-Identifier: Apache-2.0
/**
 * @file engine_handle.h
 * @brief Private definition of the entropic_engine struct.
 *
 * The public C API uses `entropic_handle_t` (opaque pointer to
 * `struct entropic_engine`). This header defines the struct body.
 * It is private to the facade — no external consumer or test
 * should include it.
 *
 * Subsystems are nullable unique_ptrs for lazy init: created during
 * entropic_configure(), not entropic_create(). Phase 0 owns only
 * hook_registry, api_mutex, last_error, and state flags.
 *
 * @version 2.0.0
 */

#pragma once

#include <entropic/config/bundled_models.h>
#include <entropic/core/compactor_registry.h>
#include <entropic/core/constitutional_validator.h>
#include <entropic/core/engine.h>
#include <entropic/core/hook_registry.h>
#include <entropic/core/identity_manager.h>
#include <entropic/inference/orchestrator.h>
#include <entropic/mcp/mcp_authorization.h>
#include <entropic/mcp/external_bridge.h>
#include <entropic/mcp/server_manager.h>
#include <entropic/mcp/tool_executor.h>
#include <entropic/storage/audit_logger.h>
#include <entropic/types/session_logger.h>
#include <entropic/storage/backend.h>
#include <entropic/types/config.h>
#include <entropic/types/error.h>
#include <entropic/types/logging.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

// Forward declarations for Phase 1+ subsystems.
// Headers included as each phase wires them.
namespace entropic {
class ModelOrchestrator;
class ServerManager;
class ToolExecutor;
class IdentityManager;
class MCPAuthorizationManager;
class AgentEngine;
class SqliteStorageBackend;
class AuditLogger;
class ConstitutionalValidator;
class CompactorRegistry;
struct ParsedConfig;
class BundledModels;
struct InterfaceContext;  // gh#58 follow-up (v2.2.6): per-handle iface ctx
} // namespace entropic

/**
 * @brief Engine handle struct — owns all subsystems.
 *
 * The public C API casts `entropic_handle_t` to/from this struct.
 * Members are grouped by the phase that wires them.
 *
 * @par Destruction order
 * Reverse of creation: engine, storage, audit, constitutional,
 * MCP, inference, config, hooks. Each pointer is null-safe.
 *
 * @version 2.0.0
 */
struct entropic_engine {
    // ── Phase 0: Lifecycle ──────────────────────────────────
    entropic::HookRegistry hook_registry;     ///< Hook dispatch
    std::mutex api_mutex;                     ///< Serializes API calls
    std::string last_error;                   ///< Per-handle error message
    std::atomic<bool> configured{false};      ///< True after configure()
    std::atomic<bool> running{false};         ///< True during run()
    /// @brief gh#59 (v2.3.1): unique handle id for per-handle log
    /// routing via `entropic::log::HandleAwareSink`. Monotonic
    /// 1.. (0 is reserved for "no handle scope"). Set in
    /// `entropic_create`.
    int log_id = 0;

    // ── Phase 1: Configuration ────────────────────────────────
    entropic::ParsedConfig config;                    ///< Parsed config
    entropic::config::BundledModels bundled_models;   ///< Model registry

    // ── Phase 2: Inference ─────────────────────────────────────
    std::unique_ptr<entropic::ModelOrchestrator> orchestrator; ///< Model pool + routing
    entropic::InferenceInterface inference_iface;              ///< Stable copy for validator lifetime
    /// Per-handle owned context backing inference_iface.user_data.
    /// Pre-v2.2.6 this was a process-global static — gh#58 follow-up.
    entropic::InterfaceContext* inference_iface_ctx = nullptr;

    // ── Phase 3: MCP + Identity + Authorization ──────────────
    std::unique_ptr<entropic::ServerManager> server_manager;           ///< MCP server lifecycle
    std::unique_ptr<entropic::ToolExecutor> tool_executor;             ///< Tool dispatch
    std::unique_ptr<entropic::IdentityManager> identity_manager;       ///< Identity lifecycle
    std::unique_ptr<entropic::MCPAuthorizationManager> mcp_auth;       ///< Per-identity tool auth

    // ── Phase 4: Engine Loop + Storage + Audit ─────────────────
    std::unique_ptr<entropic::AgentEngine> engine;                     ///< Agentic loop (owns conversation state)
    std::unique_ptr<entropic::SqliteStorageBackend> storage;           ///< SQLite persistence
    std::unique_ptr<entropic::AuditLogger> audit_logger;               ///< Audit log
    std::unique_ptr<entropic::SessionLogger> session_logger;           ///< Model transcript log

    // ── Phase 5: Constitutional + Compaction ────────────────────
    std::unique_ptr<entropic::ConstitutionalValidator> validator;       ///< Constitutional validation
    std::unique_ptr<entropic::CompactorRegistry> compactor_registry;   ///< Compaction strategies

    // ── Phase 6: Tier metadata (v2.0.4) ───────────────────────
    /// @brief Per-tier allowed_tools from identity frontmatter.
    std::unordered_map<std::string, std::vector<std::string>> tier_allowed_tools;
    /// @brief Per-tier validation_rules from identity frontmatter (v2.0.6).
    std::unordered_map<std::string, std::vector<std::string>> tier_validation_rules;

    // ── Phase 7: External MCP bridge (v2.0.8) ──────────────────
    std::unique_ptr<entropic::ExternalBridge> external_bridge;   ///< Unix socket MCP bridge

    // ── Stream observer (v2.0.10) ────────────────────────────
    /// @brief Global stream observer — fires for all streaming output.
    void (*stream_observer)(const char*, size_t, void*) = nullptr;
    void* stream_observer_data = nullptr;                        ///< Observer user_data

    // ── State-change observer (P1-5 follow-up, 2.0.6-rc16.2) ─
    /// @brief Observer for engine state transitions. Forwarded to
    /// engine callbacks so bridge async tasks can project
    /// VERIFYING → "validating"/"revising" phases.
    void (*state_observer)(int, void*) = nullptr;
    void* state_observer_data = nullptr;

    // ── Mid-gen queue observer (gh#40, v2.1.10) ───────────────
    /// @brief Observer fired when a queued mid-gen user message is
    /// consumed and seeded as the next turn. Stored on the handle
    /// so pre-configure registration survives engine construction.
    void (*queue_observer)(const char*, size_t, void*) = nullptr;
    void* queue_observer_data = nullptr;

    // ── Critique start/end callbacks (gh#50, v2.1.12) ─────────
    /// @brief Fires before the constitutional validator's critique
    /// generate begins. Stored on the handle so pre-configure
    /// registration survives validator reconstruction (the
    /// `rewire_critique_callbacks` helper re-applies the slot to
    /// any newly-built ConstitutionalValidator).
    void (*critique_start_cb)(void*) = nullptr;
    /// @brief Fires after the critique generate returns.
    void (*critique_end_cb)(void*) = nullptr;
    /// @brief Forwarded to both callbacks.
    void* critique_cb_data = nullptr;
};

namespace entropic {

/**
 * @brief gh#59 (v2.3.1): RAII guard combining api_mutex + log scope.
 *
 * Drop-in replacement for the v2.0.0–v2.3.0 pattern
 * `std::lock_guard lock(handle->api_mutex);`. Acquires the per-handle
 * mutex AND installs the per-handle log scope so any spdlog line
 * emitted from this thread routes through the HandleAwareSink to the
 * right session.log. Destructor releases both in correct order
 * (log scope first, then mutex).
 *
 * Single-call refactor target — every facade entry point that used
 * `std::lock_guard lock(handle->api_mutex)` now uses this.
 *
 * @utility
 * @version 2.3.1
 */
class HandleApiLock {
public:
    /** @brief Lock handle mutex + enter log scope. @version 2.3.1 */
    explicit HandleApiLock(entropic_handle_t h)
        : lock_(h->api_mutex), log_scope_(h->log_id) {}
    HandleApiLock(const HandleApiLock&) = delete;
    HandleApiLock& operator=(const HandleApiLock&) = delete;
private:
    // Order matters: api_mutex acquired first, log_scope second.
    // Destruction reverses (scope first, mutex second) — fine.
    std::lock_guard<std::mutex> lock_;
    entropic::log::HandleLogScope log_scope_;
};

}  // namespace entropic
