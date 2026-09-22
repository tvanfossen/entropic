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
#include <entropic/entropic.h>  // ent_path_approval_cb (v2.13.0)
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
#include <entropic/types/run_scope.h>

#include <atomic>
#include <filesystem>
#include <map>
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
 * @brief A named repository a session's tools operate in (gh#166).
 *
 * One resident model, many projects. A workspace owns its own tool root,
 * its own built-in + plugin server INSTANCES (they hold a working
 * directory each, so two workspaces cannot share one set), its own
 * external MCP servers, and its own delegation swap lock. Weights, tiers
 * and identity stay handle-wide — which is the entire point: switching
 * repository must not cost a 13 GiB reload.
 *
 * Per-workspace `app_context` is deliberately out of scope (follow-up).
 *
 * @version 2.13.0
 */
struct EntropicWorkspace {
    std::string name;                    ///< Caller-chosen workspace name
    std::filesystem::path root;          ///< Tool root for bound sessions
    std::unique_ptr<entropic::ServerManager> servers; ///< Its own instances
    /// @brief gh#160's swap lock, now per workspace — which is what lets
    /// two sessions in DIFFERENT repositories delegate concurrently.
    std::recursive_mutex swap_mutex;
};

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

    // ── Phase 3b: Delegation isolation + workspaces (gh#160/gh#166) ──
    /// @brief Swap lock for the DEFAULT workspace (gh#160).
    ///
    /// `set_working_dir` is a single field on each in-process server, so a
    /// sandboxed delegation OWNS the registry for its duration. Recursive
    /// because a nested delegation re-enters on the same thread; held from
    /// `ScopedSandbox` construction to destruction. With
    /// `delegation.isolation: none` (the default) it is never taken. A
    /// named workspace has its own (`EntropicWorkspace::swap_mutex`), so
    /// two repositories delegate in parallel.
    std::recursive_mutex sandbox_swap_mutex;

    /// @brief gh#166: named workspaces, by name. The DEFAULT workspace is
    /// `server_manager` above and is not in this map.
    std::map<std::string, std::unique_ptr<EntropicWorkspace>> workspaces;
    /// @brief gh#166: session key → workspace name. A key that is absent
    /// resolves to the default workspace, so every existing consumer —
    /// which binds nothing — is unaffected.
    std::map<std::string, std::string> session_workspace;
    /// @brief gh#166: guards both maps above. Held for lookups only.
    mutable std::mutex workspace_mutex;

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

    // ── Outside-root path approver (v2.13.0) ───────────────────
    /// @brief Consumer's approver for filesystem access outside the root.
    /// Stored on the handle so pre-configure registration survives server
    /// construction; the default server set's FilesystemServer holds a
    /// facade thunk that reads this slot per call. Workspaces never do.
    ent_path_approval_cb path_approval_cb = nullptr;
    void* path_approval_data = nullptr;       ///< Forwarded to the approver
    /// @brief Guards the pair above: the thunk reads it from run threads
    /// while the setter may write it from any thread.
    std::mutex path_approval_mutex;
};

namespace entropic {

/**
 * @brief The workspace a session is bound to, or nullptr for the default.
 *
 * gh#166 (v2.13.0). An unbound session — every session that existed
 * before this release — resolves to nullptr and therefore to the
 * handle-wide `server_manager`, which is why binding nothing changes
 * nothing.
 *
 * @param h Engine handle.
 * @param key Session key ("" = default session).
 * @return Borrowed workspace, or nullptr when unbound/unknown.
 * @req REQ-MCP-027
 * @version 2.13.0
 */
inline EntropicWorkspace* workspace_for(entropic_handle_t h,
                                        const std::string& key) {
    if (h == nullptr) { return nullptr; }
    std::lock_guard<std::mutex> lock(h->workspace_mutex);
    auto bound = h->session_workspace.find(key);
    if (bound == h->session_workspace.end()) { return nullptr; }
    auto ws = h->workspaces.find(bound->second);
    return ws == h->workspaces.end() ? nullptr : ws->second.get();
}

/**
 * @brief The MCP servers a session's tools must run against (gh#166).
 * @param h Engine handle.
 * @param key Session key ("" = default session).
 * @return The bound workspace's servers, else the handle's default set.
 * @req REQ-MCP-027
 * @version 2.13.0
 */
inline entropic::ServerManager* workspace_servers(entropic_handle_t h,
                                                  const std::string& key) {
    auto* ws = workspace_for(h, key);
    if (ws != nullptr && ws->servers) { return ws->servers.get(); }
    return h != nullptr ? h->server_manager.get() : nullptr;
}

/**
 * @brief Point the DEFAULT server set's filesystem server at the handle's
 *        path-approval slot (v2.13.0).
 *
 * Installs a thunk, not the consumer's callback, so a later
 * `entropic_set_path_approval_callback` takes effect without touching the
 * server. Called once the default set exists; never for a workspace.
 *
 * @param h Engine handle with `server_manager` constructed.
 * @req REQ-MCP-021
 * @version 2.13.0
 */
void wire_outside_root_approver(entropic_handle_t h);

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
 * @req REQ-API-003
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

/**
 * @brief gh#144 (v2.12.0): RAII turn claim + log scope, for the RUN paths.
 *
 * Sibling of HandleApiLock, and deliberately NOT built on it. HandleApiLock
 * takes `api_mutex`; gh#109 removed that mutex from all six run entry points
 * precisely so a long turn could not block `entropic_interrupt()` called from
 * another thread. Reusing it here would silently re-break gh#109, which is
 * the single mistake this refactor is most likely to make — hence a separate
 * type rather than a subclass or a flag.
 *
 * The claim is one compare-exchange on the engine's `running_flag_`. A second
 * thread interrupting a 40-second turn still touches only atomics.
 *
 * Usage at every run entry point:
 * @code
 *   HandleTurnGuard turn(handle, session_key);  // key optional; "" = default
 *   if (!turn.claim()) { return ENTROPIC_ERROR_ALREADY_RUNNING; }
 * @endcode
 *
 * gh#158 (v2.13.0): the claim is scoped to a SESSION KEY. The keyed run
 * entry points pass theirs; the unkeyed ones claim the default session,
 * which is what they have always effectively done. Whether a second key may
 * proceed concurrently is the engine's decision (`concurrent_sessions`), not
 * the guard's — the guard only makes sure the key it claims is the key it
 * releases.
 *
 * Holding the claim across the WHOLE entry point — not just the engine call —
 * is the point: the facade serialises results out of the same conversation
 * the engine appends to, so releasing early would let a second thread mutate
 * that vector mid-read. That is the race gh#144 reported.
 *
 * @req REQ-API-009
 * @version 2.12.0
 */
class HandleTurnGuard {
public:
    /**
     * @brief Enter the handle's log scope. Does NOT claim the turn.
     *
     * Construction is deliberately side-effect-free beyond logging so the
     * guard can be declared ahead of argument validation; claim() is then
     * evaluated last in the precondition chain. A call that is going to be
     * rejected for a null argument must not momentarily claim the engine and
     * bounce a legitimate concurrent caller.
     *
     * Null-handle safe: log id 0 is the reserved "no handle scope" sentinel.
     *
     * @param h Engine handle, possibly null.
     * @param session_key Session this guard claims for (gh#158), and
     *        publishes to this thread for the call's duration (gh#166).
     * @version 2.13.0
     */
    explicit HandleTurnGuard(entropic_handle_t h,
                             const char* session_key = nullptr)
        : engine_(h != nullptr ? h->engine.get() : nullptr),
          log_scope_(h != nullptr ? h->log_id : 0),
          key_(session_key != nullptr ? session_key : ""),
          // gh#166: publish the key to THIS thread for the whole call, so
          // the tool-prompt callback — which arrives through an interface
          // header that takes only a tier name — can resolve the
          // workspace this turn belongs to.
          session_scope_(session_key != nullptr ? session_key : "") {}

    /**
     * @brief Release the turn if this guard claimed it.
     * @version 2.13.0 [reviewed]
     */
    ~HandleTurnGuard() {
        if (claimed_) { engine_->end_turn(key_); }
    }

    HandleTurnGuard(const HandleTurnGuard&) = delete;
    HandleTurnGuard& operator=(const HandleTurnGuard&) = delete;

    /**
     * @brief Claim the turn. Idempotent for one guard.
     * @return true when this guard now owns the turn; false when another
     *         turn is already in flight on this handle.
     * @req REQ-API-009
     * @version 2.13.0
     */
    bool claim() {
        if (!claimed_ && engine_ != nullptr) {
            claimed_ = engine_->try_begin_turn(key_);
        }
        return claimed_;
    }

    /**
     * @brief The session this guard claims for.
     * @return Session key; `""` for the default session.
     * @utility
     * @version 2.13.0
     */
    const std::string& key() const { return key_; }

    /**
     * @brief Whether this guard owns the turn.
     * @utility
     * @version 2.12.0
     */
    bool claimed() const { return claimed_; }

private:
    entropic::AgentEngine* engine_;
    entropic::log::HandleLogScope log_scope_;
    /// @brief gh#158 (v2.13.0): the session this guard claims and releases.
    /// Claim and release MUST name the same key, which is why the guard
    /// carries it rather than each call site remembering to pass it twice.
    std::string key_;
    /// @brief gh#166 (v2.13.0): publishes `key_` to this thread for the
    /// duration of the call. Declared AFTER `key_` so it is destroyed
    /// first — the reverse order would restore a key from a member that
    /// is already gone.
    entropic::RunSessionScope session_scope_;
    bool claimed_ = false;
};

}  // namespace entropic
