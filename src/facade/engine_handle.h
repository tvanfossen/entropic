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
#include <entropic/mcp/server_manager.h>
#include <entropic/mcp/tool_executor.h>
#include <entropic/storage/audit_logger.h>
#include <entropic/types/session_logger.h>
#include <entropic/storage/backend.h>
#include <entropic/types/config.h>
#include <entropic/types/error.h>

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

    // ── Phase 1: Configuration ────────────────────────────────
    entropic::ParsedConfig config;                    ///< Parsed config
    entropic::config::BundledModels bundled_models;   ///< Model registry

    // ── Phase 2: Inference ─────────────────────────────────────
    std::unique_ptr<entropic::ModelOrchestrator> orchestrator; ///< Model pool + routing

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
};
