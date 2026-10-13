// SPDX-License-Identifier: Apache-2.0
/**
 * @file external_bridge.cpp
 * @brief Unix socket MCP bridge implementation.
 *
 * Listens on a unix domain socket, serves every connected client on its
 * own thread, and dispatches JSON-RPC 2.0 (MCP) to the engine handle. Turns
 * queue per session (v2.14.0); a session's async completions, clears and
 * cancels touch only that session and that client.
 *
 * @version 2.14.0
 */

#include <entropic/mcp/external_bridge.h>
#include <entropic/entropic.h>
#include <entropic/types/logging.h>

#include "engine_handle.h"
#include "final_text.h"  // gh#130 (v2.10.2)
#include "tool_namespace.h"  // gh#145 (v2.12.0)

#include <nlohmann/json.hpp>

#include <atomic>
#include <cerrno>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <poll.h>

using json = nlohmann::json;

namespace entropic {

static auto logger = entropic::log::get("mcp.external_bridge");

// gh#58: process-wide set of socket paths currently bound by an
// ExternalBridge in this process. Without this, a second handle whose
// project_dir hashes to the same socket path as a first handle would
// unlink the live socket out from under handle #1 (see the
// std::filesystem::remove call in create_listen_socket). With this
// guard, the second handle declines to start its bridge instead.
static std::mutex s_bound_sockets_mu;
static std::unordered_set<std::string> s_bound_sockets;

// Forward declaration
std::filesystem::path compute_socket_path(
    const std::filesystem::path& project_dir);

// ── JSON-RPC helpers ─────────────────────────────────────

/**
 * @brief Build a JSON-RPC success response.
 * @param id Request ID.
 * @param result Result payload.
 * @return Serialized JSON-RPC response.
 * @utility
 * @version 2.0.8
 */
static std::string rpc_ok(const json& id, const json& result) {
    json r = {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
    return r.dump();
}

/**
 * @brief Build a JSON-RPC error response.
 * @param id Request ID.
 * @param code Error code.
 * @param msg Error message.
 * @return Serialized JSON-RPC response.
 * @utility
 * @version 2.0.8
 */
static std::string rpc_err(const json& id, int code,
                           const std::string& msg) {
    json r = {{"jsonrpc", "2.0"}, {"id", id},
              {"error", {{"code", code}, {"message", msg}}}};
    return r.dump();
}

/**
 * @brief Wrap text in MCP tool result shape.
 * @param text Result text.
 * @return MCP content array JSON.
 * @utility
 * @version 2.0.8
 */
static json tool_text(const std::string& text) {
    return {{"content", json::array({{{"type", "text"}, {"text", text}}})}};
}

// ── Tool definitions ─────────────────────────────────────

/**
 * @brief The bare tool suffixes this bridge exposes, in advertised order.
 *
 * gh#145 (v2.12.0). SINGLE SOURCE OF TRUTH. Both `tool_definitions()` and the
 * name-resolution in `dispatch_tool()` derive from this table, so a name can no
 * longer be advertised under one spelling and dispatched under another — before
 * this the five names were written out twice, ~470 lines apart.
 *
 * @version 2.12.0
 */
namespace tool_suffix {
constexpr const char* kAsk           = "ask";
constexpr const char* kAskStatus     = "ask_status";
constexpr const char* kStatus        = "status";
constexpr const char* kContextClear  = "context_clear";
constexpr const char* kContextCount  = "context_count";
} // namespace tool_suffix

/**
 * @brief Resolve a tool's description, honouring any consumer override.
 *
 * gh#145 (v2.12.0): the description is what a MODEL reasons over when choosing
 * a tool, and the stock text describes the transport rather than the capability
 * — so a host with one specific job can replace it. An absent or unknown key
 * leaves the built-in text untouched.
 *
 * @param cfg External MCP config carrying any overrides.
 * @param suffix Bare tool suffix.
 * @param fallback Built-in description.
 * @return The override when one is configured, else `fallback`.
 * @utility
 * @version 2.12.0
 */
static std::string describe(const ExternalMCPConfig& cfg,
                            const char* suffix,
                            const char* fallback) {
    auto it = cfg.tool_descriptions.find(suffix);
    if (it != cfg.tool_descriptions.end() && !it->second.empty()) {
        return it->second;
    }
    return fallback;
}

/**
 * @brief Definition for a tool that takes no arguments.
 *
 * gh#145 (v2.12.0): three of the five tools share an empty inputSchema, so
 * they share a builder. Also keeps tool_definitions() inside the SLOC gate.
 *
 * @param cfg External MCP config supplying prefix and description overrides.
 * @param suffix Bare tool suffix.
 * @param fallback Built-in description.
 * @return One MCP tool definition object.
 * @utility
 * @version 2.12.0
 */
static json no_args_tool(const ExternalMCPConfig& cfg,
                         const char* suffix,
                         const char* fallback) {
    return {{"name", facade::qualify_tool_name(cfg.tool_prefix, suffix)},
            {"description", describe(cfg, suffix, fallback)},
            {"inputSchema", {{"type", "object"},
                             {"properties", json::object()}}}};
}

/**
 * @brief Definition for a tool scoped to an optional session (gh#144).
 *
 * `session` is optional and NOT in `required`: omitting it means the shared
 * default session, which is exactly what a pre-2.12.0 client already gets.
 *
 * @param cfg External MCP config supplying prefix and description overrides.
 * @param suffix Bare tool suffix.
 * @param fallback Built-in description.
 * @return One MCP tool definition object.
 * @utility
 * @version 2.12.0
 */
static json session_scoped_tool(const ExternalMCPConfig& cfg,
                                const char* suffix,
                                const char* fallback) {
    return {{"name", facade::qualify_tool_name(cfg.tool_prefix, suffix)},
            {"description", describe(cfg, suffix, fallback)},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"session", {{"type", "string"},
                                 {"description",
                                  "Conversation to act on; omit for the "
                                  "shared default session."}}}
                }}
            }}};
}

/**
 * @brief Definition for the ask tool (prompt + optional async).
 * @param cfg External MCP config supplying prefix and description overrides.
 * @return One MCP tool definition object.
 * @utility
 * @version 2.14.0
 */
static json ask_tool(const ExternalMCPConfig& cfg) {
    return {
        {"name", facade::qualify_tool_name(cfg.tool_prefix,
                                           tool_suffix::kAsk)},
        {"description",
         describe(cfg, tool_suffix::kAsk,
                  "Submit a prompt to the running entropic engine. "
                  "Set async=true to return immediately with a task_id; "
                  "the engine pushes a notification when done.")},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"prompt", {{"type", "string"},
                            {"description", "User message"}}},
                {"async", {{"type", "boolean"},
                           {"description", "Run asynchronously"},
                           {"default", false}}},
                // gh#144: optional, and NOT in `required`. Absent means the
                // shared default session, which is exactly pre-2.12.0
                // behaviour for every existing client.
                {"session", {{"type", "string"},
                             {"description",
                              "Conversation to run under. Callers passing "
                              "the same value share a conversation; "
                              "omit for the shared default session."}}},
                // gh#205 (v2.14.0): both optional; absent = pre-2.14.0.
                {"tier", {{"type", "string"},
                          {"description",
                           "Tier or identity to run this turn under; omit "
                           "to route as usual."}}},
                {"workspace", {{"type", "string"},
                               {"description",
                                "Repository this session works in: an "
                                "absolute directory (a workspace is created "
                                "for it on first use) or the name of a "
                                "workspace the host created. Fixed for the "
                                "session once it has messages."}}}
            }},
            {"required", json::array({"prompt"})}
        }}};
}

/**
 * @brief Definition for the ask_status tool (task_id lookup).
 * @param cfg External MCP config supplying prefix and description overrides.
 * @return One MCP tool definition object.
 * @utility
 * @version 2.12.0
 */
static json ask_status_tool(const ExternalMCPConfig& cfg) {
    return {
        {"name", facade::qualify_tool_name(cfg.tool_prefix,
                                           tool_suffix::kAskStatus)},
        {"description",
         describe(cfg, tool_suffix::kAskStatus,
                  "Check status of an async ask task.")},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {{"task_id", {
                {"type", "string"},
                {"description", "Task ID from async ask"}
            }}}},
            {"required", json::array({"task_id"})}
        }}};
}

/**
 * @brief MCP tool definitions exposed by the bridge.
 *
 * gh#145 (v2.12.0): names and descriptions are consumer-configurable. The
 * defaults ("entropic" prefix, no description overrides) reproduce the
 * pre-2.12.0 payload exactly.
 *
 * @param cfg External MCP config supplying prefix and description overrides.
 * @return JSON array of tool schemas.
 * @utility
 * @version 2.12.0-rc1
 */
static json tool_definitions(const ExternalMCPConfig& cfg) {
    return json::array({
        ask_tool(cfg),
        ask_status_tool(cfg),
        no_args_tool(cfg, tool_suffix::kStatus,
                     "Engine version and message count."),
        session_scoped_tool(cfg, tool_suffix::kContextClear,
                            "Clear conversation history."),
        session_scoped_tool(cfg, tool_suffix::kContextCount,
                            "Return the message count."),
    });
}

// ── Tool handlers ────────────────────────────────────────

// gh#130 (v2.10.2): extract_final_text moved to the private facade header
// final_text.h so its selection rule is unit-testable — this was a `static`
// helper with no reachable test, and it shipped a scanning bug that returned
// "(no response)" to operators while the answer sat earlier in the same
// conversation. See facade_text::extract_final_text for the rule.

/**
 * @brief Write a JSON-RPC line to a socket fd.
 * @param fd Socket file descriptor.
 * @param msg JSON object to serialize and write.
 * @utility
 * @version 2.0.10
 */
static void write_json_line(int fd, const json& msg) {
    auto s = msg.dump() + "\n";
    ::write(fd, s.c_str(), s.size());
}

/**
 * @brief Send an MCP progress notification with a text token.
 * @param fd Client socket fd.
 * @param token_text Token string to send (carried in `message`).
 * @param progress_token The caller's `_meta.progressToken`, else the
 *        request id (pre-2.14.0 behaviour, kept for clients that never
 *        send one).
 * @param seq Monotonic notification count (MCP's numeric `progress`).
 * @utility
 * @version 2.0.10
 */
static void send_progress(int fd, const std::string& token_text,
                          const json& progress_token, uint64_t seq) {
    // v2.14.0: MCP's `progress` is a NUMBER that increases with each
    // notification; the token text rides in `message`. Pre-2.14.0 put the
    // text in `progress`, which spec-validating clients reject.
    json notif = {
        {"jsonrpc", "2.0"},
        {"method", "notifications/progress"},
        {"params", {
            {"progressToken", progress_token},
            {"progress", seq},
            {"message", token_text}
        }}
    };
    write_json_line(fd, notif);
}

/**
 * @brief Read the live conversation and render the operator-visible answer.
 *
 * gh#130 (v2.10.2): shared by both ask paths so the answer-vs-diagnostic rule
 * cannot drift between them.
 *
 * gh#144 (v2.12.0): reads THIS session's conversation. The unscoped read it
 * replaced would have extracted one caller's answer from whichever
 * conversation the shared accessor happened to return.
 *
 * @param handle Engine handle.
 * @param session_key Session whose answer to render.
 * @return MCP tool result JSON.
 * @req REQ-API-012
 * @req REQ-BRIDGE-001
 * @version 2.12.0
 */
static json final_answer_from_context(entropic_handle_t handle,
                                      const std::string& session_key) {
    char* msgs_json = nullptr;
    entropic_session_context_get(handle, session_key.c_str(), &msgs_json);
    auto answer = facade_text::final_text_or_reason(msgs_json);
    entropic_free(msgs_json);
    return tool_text(answer);
}

/**
 * @brief Run one non-streaming turn, under `tier` when one is given (gh#205).
 * @param handle Engine handle.
 * @param session_key Session key.
 * @param tier Tier or identity; "" routes as usual.
 * @param prompt User prompt.
 * @param[out] result_json Serialized result (free with entropic_free).
 * @return The run's error code.
 * @utility
 * @version 2.14.0
 */
static entropic_error_t run_plain_turn(entropic_handle_t handle,
                                       const std::string& session_key,
                                       const std::string& tier,
                                       const std::string& prompt,
                                       char** result_json) {
    return tier.empty()
        ? entropic_run_session(handle, session_key.c_str(), prompt.c_str(),
                               result_json)
        : entropic_run_session_as(handle, session_key.c_str(), tier.c_str(),
                                  prompt.c_str(), result_json);
}

/**
 * @brief Run one streaming turn, under `tier` when one is given (gh#205).
 * @param handle Engine handle.
 * @param session_key Session key.
 * @param tier Tier or identity; "" routes as usual.
 * @param prompt User prompt.
 * @param on_token Token callback.
 * @param ud Forwarded to on_token.
 * @return The run's error code.
 * @utility
 * @version 2.14.0
 */
static entropic_error_t run_stream_turn(
    entropic_handle_t handle, const std::string& session_key,
    const std::string& tier, const std::string& prompt,
    void (*on_token)(const char*, size_t, void*), void* ud) {
    return tier.empty()
        ? entropic_run_session_streaming(handle, session_key.c_str(),
                                         prompt.c_str(), on_token, ud, nullptr)
        : entropic_run_session_streaming_as(handle, session_key.c_str(),
                                            tier.c_str(), prompt.c_str(),
                                            on_token, ud, nullptr);
}

/**
 * @brief Handle entropic.ask — non-streaming path (gh#115, v2.9.12).
 *
 * Routes through entropic_run (no on_token callback) so that MTP can
 * engage without hitting the streaming-incompatibility guard. Also
 * avoids the run_streaming dump-site 316 throw. Used when the bridge
 * is configured with mcp.external.ask_streaming: false.
 *
 * @param handle Engine handle.
 * @param args Tool arguments (must contain "prompt" string).
 * @return MCP tool result JSON (final clean text from entropic_run).
 * @req REQ-API-012
 * @req REQ-BRIDGE-001
 * @version 2.14.0
 */
static json handle_ask_plain(entropic_handle_t handle, const json& args,
                             const std::string& session_key) {
    auto it = args.find("prompt");
    if (it == args.end() || !it->is_string()) {
        return tool_text("error: missing 'prompt' argument");
    }
    char* result_json = nullptr;
    auto err = run_plain_turn(handle, session_key,
                              args.value("tier", std::string{}),  // gh#205
                              it->get<std::string>(), &result_json);
    if (err != ENTROPIC_OK) {
        const char* msg = entropic_last_error(handle);
        entropic_free(result_json);
        return tool_text(std::string("error: ") + (msg ? msg : "unknown"));
    }
    // Resolve before the free — final_text_or_reason reads that buffer.
    auto answer = facade_text::final_text_or_reason(result_json);
    entropic_free(result_json);
    return tool_text(answer);
}

/**
 * @brief Handle entropic.ask — stream tokens then return final text.
 *
 * Uses entropic_run_streaming to send MCP progress notifications
 * for each token (streaming to client), then extracts the final
 * clean assistant text from the conversation for the tool result.
 * Used when mcp.external.ask_streaming: true (the default).
 *
 * @param handle Engine handle.
 * @param args Tool arguments.
 * @param client_fd Socket fd for streaming progress notifications.
 * @param call_id JSON-RPC request id (for progress token correlation).
 * @return MCP tool result JSON (final clean text).
 * @req REQ-API-012
 * @req REQ-BRIDGE-001
 * @version 2.10.2
 */
static json handle_ask(entropic_handle_t handle, const json& args,
                       const std::string& session_key,
                       int client_fd, const json& progress_token) {
    auto it = args.find("prompt");
    if (it == args.end() || !it->is_string()) {
        return tool_text("error: missing 'prompt' argument");
    }
    std::string prompt = it->get<std::string>();

    // Stream tokens as progress notifications
    struct StreamCtx { int fd; json token; uint64_t seq; };
    StreamCtx sctx{client_fd, progress_token, 0};
    auto on_token = [](const char* tok, size_t len, void* ud) {
        auto* ctx = static_cast<StreamCtx*>(ud);
        send_progress(ctx->fd, std::string(tok, len), ctx->token, ++ctx->seq);
    };
    // gh#205: a requested tier locks the turn; absent routes as before.
    auto err = run_stream_turn(handle, session_key,
                               args.value("tier", std::string{}), prompt,
                               on_token, &sctx);
    if (err != ENTROPIC_OK) {
        const char* msg = entropic_last_error(handle);
        return tool_text(std::string("error: ") + (msg ? msg : "unknown"));
    }

    return final_answer_from_context(handle, session_key);
}

/**
 * @brief Handle the status tool.
 *
 * gh#144 (v2.12.0) adds the occupancy lines. The issue's second half was
 * that a caller queued behind another's turn is indistinguishable from one
 * talking to a hung engine — on a 4B-class local model a review is 15-90
 * seconds, so a routine wait was presented as an indefinite one. `busy` and
 * `queue_depth` are what make the difference visible.
 *
 * Appended as text lines rather than restructured into JSON on purpose: the
 * result is an unstructured blob with an embedded `metrics:` payload that
 * consumers already parse, and changing its shape would break them.
 *
 * @param handle Engine handle.
 * @param bridge Bridge instance, for the turn-queue counters.
 * @return MCP tool result JSON.
 * @req REQ-BRIDGE-001
 * @version 2.12.0-rc1
 */
static json handle_status(entropic_handle_t handle,
                          const ExternalBridge* bridge) {
    size_t count = 0;
    entropic_context_count(handle, &count);
    std::ostringstream os;
    os << "entropic " << entropic_version()
       << "\nmessages: " << count
       << "\nbusy: " << (bridge->turn_in_flight() ? "true" : "false")
       << "\nqueue_depth: " << bridge->turn_queue_depth();
    // gh#144 (v2.12.0): which callers this host is serving, and how much
    // history each holds.
    char* sessions = nullptr;
    if (entropic_session_list(handle, &sessions) == ENTROPIC_OK
        && sessions != nullptr) {
        os << "\nsessions: " << sessions;
        entropic_free(sessions);
    }
    // Metrics + per-tier breakdown (P2-15 follow-up, 2.0.6-rc16.2)
    char* mjson = nullptr;
    if (entropic_metrics_json(handle, &mjson) == ENTROPIC_OK
        && mjson != nullptr) {
        os << "\nmetrics: " << mjson;
        entropic_free(mjson);
    }
    return tool_text(os.str());
}

/**
 * @brief Handle entropic.context_clear.
 * @param handle Engine handle.
 * @return MCP tool result JSON.
 * @dg_internal
 * @version 2.0.8
 */
/**
 * @brief Cancel any async tasks currently running on the bridge.
 *
 * Marks tasks "cancelling", raises engine interrupt so the generation
 * loop unwinds, and polls up to ~1s for their worker threads to move
 * into a terminal state. The wait is best-effort — detached workers
 * may still be in flight when this returns, but the new context clear
 * will race them cleanly because each run owns its own result_json.
 *
 * @param handle Engine handle (for interrupt).
 * @param bridge Bridge whose tasks_ registry is being canceled.
 * @dg_internal (P1-8, 2.0.6-rc16)
 * @version 2.0.6-rc16
 */
/**
 * @brief Final-state projection for an async entropic_run.
 *
 * Groups the three status/phase/text outputs so run_async_ask stays
 * under the 50-SLOC quality gate. (2.0.6-rc16)
 *
 * @dg_internal
 */
struct AsyncFinalState {
    std::string status;   ///< done | error | cancelled
    std::string phase;    ///< done | failed | cancelled
    std::string text;     ///< result or error message
};

/**
 * @brief Translate entropic_run's return code into a final task state.
 *
 * On success, extracts the final clean assistant text from result_json
 * and frees it. On cancellation/error, reports the last-error message.
 * (P1-5, 2.0.6-rc16)
 *
 * @param handle Engine handle (for last-error lookup).
 * @param err Return code from entropic_run.
 * @param result_json JSON result (owned; freed on success, else NULL).
 * @return AsyncFinalState ready to be stored on the task.
 * @req REQ-BRIDGE-001
 * @version 2.10.2
 */
static AsyncFinalState derive_async_final_state(
    entropic_handle_t handle,
    entropic_error_t err,
    char* result_json) {
    AsyncFinalState s;
    if (err == ENTROPIC_ERROR_CANCELLED
        || err == ENTROPIC_ERROR_INTERRUPTED) {
        const char* msg = entropic_last_error(handle);
        s.text = msg ? msg : "cancelled";
        s.status = "cancelled";
        s.phase = "cancelled";
    } else if (err != ENTROPIC_OK) {
        const char* msg = entropic_last_error(handle);
        s.text = msg ? msg : "unknown error";
        s.status = "error";
        s.phase = "failed";
    } else {
        // gh#130 (v2.10.2): same selection rule as the sync paths. This one
        // had no fallback at all, so a stalled async ask reported
        // status="done" with empty text — even less diagnosable than the
        // sync path's "(no response)".
        s.text = facade_text::final_text_or_reason(result_json);
        entropic_free(result_json);
        s.status = "done";
        s.phase = "done";
    }
    return s;
}

/**
 * @brief Mark one session's queued/running tasks as cancelling.
 * @param bridge Bridge holding the task registry.
 * @param session_key Session whose tasks are cancelled (v2.14.0).
 * @return true if any task was still in-flight.
 * @utility
 * @version 2.0.6-rc16
 */
static bool mark_tasks_cancelling(ExternalBridge* bridge,
                                  const std::string& session_key) {
    std::lock_guard<std::mutex> lock(bridge->tasks_mutex_);
    bool any = false;
    for (auto& [_, task] : bridge->tasks_for_cancel()) {
        // v2.14.0: only the cleared session's tasks.
        if (task.session_key != session_key) { continue; }
        if (task.status == "queued" || task.status == "running") {
            task.status = "cancelled";
            task.phase = "cancelling";
            any = true;
        }
    }
    return any;
}

/**
 * @brief True while one session's task is still phase=cancelling.
 * @param bridge Bridge holding the task registry.
 * @param session_key Session being cleared (v2.14.0).
 * @return true when a task of that session is still cancelling.
 * @utility
 * @version 2.0.6-rc16
 */
static bool any_cancelling_left(ExternalBridge* bridge,
                                const std::string& session_key) {
    std::lock_guard<std::mutex> lock(bridge->tasks_mutex_);
    bool left = false;
    for (auto& [_, task] : bridge->tasks_for_cancel()) {
        left = left || (task.session_key == session_key
                        && task.phase == "cancelling");
    }
    return left;
}

/**
 * @brief Cancel any async tasks currently running on the bridge.
 *
 * Raises engine interrupt, flips still-running tasks into
 * phase="cancelling", and polls up to ~1s for detached workers to
 * finish. After the poll, unconditionally bumps the observer generation
 * via detach_phase_observer() so any post-poll callbacks from a still-
 * running worker are silently discarded. (P1-8, 2.0.6-rc16;
 * observer-gen force-detach: E5+E6, 2.1.0)
 *
 * @param handle Engine handle (for interrupt).
 * @param bridge Bridge whose tasks_ registry is being canceled.
 * @param session_key Session being cleared; nothing else is touched.
 * @req REQ-BRIDGE-001
 * @version 2.1.0
 */
static void cancel_inflight_async_tasks(
    entropic_handle_t handle, ExternalBridge* bridge,
    const std::string& session_key) {
    if (bridge == nullptr) { return; }
    // v2.14.0: THIS session's run only. entropic_interrupt is handle-wide —
    // clearing session A used to cancel session B's turn.
    entropic_interrupt_session(handle, session_key.c_str());
    if (!mark_tasks_cancelling(bridge, session_key)) { return; }
    for (int i = 0; i < 20 && any_cancelling_left(bridge, session_key); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    bridge->detach_phase_observer();  // bump gen; silences any stale observer
}

/**
 * @brief entropic.context_clear MCP tool handler.
 *
 * Before clearing, cancels any in-flight async ask tasks so they do
 * not continue running against a stale context. (P1-8, 2.0.6-rc16)
 *
 * @param handle Engine handle.
 * @param bridge Bridge instance (used to reach the task registry).
 * @return MCP tool result JSON.
 * @req REQ-BRIDGE-001
 * @version 2.14.0
 */
static json handle_clear(entropic_handle_t handle,
                         ExternalBridge* bridge,
                         const std::string& session_key) {
    cancel_inflight_async_tasks(handle, bridge, session_key);
    // gh#144 (v2.12.0): clear ONLY the named session. The unscoped clear
    // wiped every caller's context, which the issue correctly observed "is
    // not isolation" — with two callers active there is no ordering in which
    // it is correct.
    auto err = entropic_session_context_clear(handle, session_key.c_str());
    if (err != ENTROPIC_OK) {
        return tool_text("error: clear failed");
    }
    return tool_text(session_key.empty()
        ? "conversation cleared"
        : "conversation cleared for session '" + session_key + "'");
}

/**
 * @brief Handle entropic.context_count.
 * @param handle Engine handle.
 * @return MCP tool result JSON.
 * @req REQ-BRIDGE-001
 * @version 2.0.8
 */
static json handle_count(entropic_handle_t handle,
                         const std::string& session_key) {
    size_t count = 0;
    // gh#144 (v2.12.0): count only the named session.
    entropic_session_context_count(handle, session_key.c_str(), &count);
    return tool_text(std::to_string(count));
}

// ── Async ask status ─────────────────────────────────────

/**
 * @brief Handle entropic.ask_status — check async task state.
 *
 * Returns coarse status + granular phase so pollers see progression
 * through queued → running → running:<tier> → done|failed|cancelled.
 *
 * @param args Tool arguments (must contain "task_id").
 * @return MCP tool result JSON with status/phase/result/error.
 * @req REQ-BRIDGE-001
 * @version 2.0.6-rc16
 */
json ExternalBridge::handle_ask_status(const json& args) {
    auto tid = args.value("task_id", std::string{});
    std::lock_guard<std::mutex> lock(tasks_mutex_);
    auto it = tasks_.find(tid);
    if (it == tasks_.end()) {
        return tool_text("error: unknown task_id");
    }
    json status = {{"status", it->second.status},
                   {"phase",  it->second.phase}};
    if (!it->second.result.empty()) {
        auto key = (it->second.status == "error") ? "error" : "result";
        status[key] = it->second.result;
    }
    return tool_text(status.dump());
}

// ── UUID generation ──────────────────────────────────────

/**
 * @brief Generate a simple UUID-like task ID.
 * @return Hex string (16 chars from random_device + pid).
 * @utility
 * @version 2.0.11
 */
static std::string generate_task_id() {
    static std::atomic<uint64_t> counter{0};
    auto n = counter.fetch_add(1);
    auto t = std::chrono::steady_clock::now().time_since_epoch().count();
    std::ostringstream ss;
    ss << std::hex << (t ^ (n * 2654435761ULL));
    return "task-" + ss.str();
}

namespace {

/// @brief How long a bridge caller waits before being told the engine is busy.
constexpr long kTurnWaitTimeoutMs = 300000;  // 5 minutes

/**
 * @brief RAII wrapper over begin_turn_wait / end_turn_wait (gh#144).
 * @dg_internal
 * @version 2.12.0
 */
class TurnTicket {
public:
    /**
     * @brief Take a ticket and wait for this session's turn.
     *
     * v2.14.0: logs how long admission took. Per-session lanes are only
     * observable this way — two sessions admitted with no wait while both
     * were in flight is the engagement evidence the multi-client test
     * reads; a bridge-wide queue shows the second waiting out the first.
     *
     * @param b Bridge owning the queue.
     * @param session_key Session the turn runs under (v2.14.0).
     * @dg_internal
     * @version 2.14.0
     */
    TurnTicket(ExternalBridge* b, std::string session_key)
        : bridge_(b), session_(std::move(session_key)),
          held_(b->begin_turn_wait(kTurnWaitTimeoutMs, &waited_ms_, session_)) {
        logger->info("[turn] session '{}' {} after {} ms", session_,
                     held_ ? "admitted" : "gave up", waited_ms_);
    }

    /**
     * @brief Hand the turn on, if this ticket held it.
     * @dg_internal
     * @version 2.14.0
     */
    ~TurnTicket() { if (held_) { bridge_->end_turn_wait(session_); } }

    TurnTicket(const TurnTicket&) = delete;
    TurnTicket& operator=(const TurnTicket&) = delete;

    /**
     * @brief Whether the turn was acquired before the deadline.
     * @return true when this ticket owns the engine.
     * @dg_internal
     * @version 2.12.0
     */
    bool held() const { return held_; }

    /**
     * @brief Time spent waiting in line.
     * @return Milliseconds waited.
     * @dg_internal
     * @version 2.12.0
     */
    long waited_ms() const { return waited_ms_; }

private:
    ExternalBridge* bridge_;
    std::string session_;
    long waited_ms_ = 0;
    bool held_;
};

/**
 * @brief The tool result returned when the queue deadline expired.
 * @dg_internal
 * @version 2.12.0
 */
json turn_timeout_result(const ExternalBridge* bridge, long waited_ms) {
    std::ostringstream os;
    os << "error: engine busy — waited " << waited_ms << " ms with "
       << bridge->turn_queue_depth() << " request(s) still ahead. "
       << "Another caller's turn is still running; retry shortly.";
    return tool_text(os.str());
}

} // namespace

// ── Dispatch ─────────────────────────────────────────────

/**
 * @brief The session's current workspace binding, or "" (gh#205).
 * @param handle Engine handle.
 * @param session_key Session key.
 * @return Bound workspace name, or "" when unbound.
 * @dg_internal
 * @version 2.14.0
 */
static std::string bound_workspace(entropic_handle_t handle,
                                   const std::string& session_key) {
    std::lock_guard<std::mutex> guard(handle->workspace_mutex);
    auto it = handle->session_workspace.find(session_key);
    return it == handle->session_workspace.end() ? std::string{} : it->second;
}

/**
 * @brief Resolve `workspace` to a workspace name, creating one for a path.
 *
 * An absolute directory names the repository the caller is in; its
 * canonical path IS the workspace name, so every client in that repository
 * lands in the same workspace. Creating one no longer spawns the
 * repository's `.mcp.json` (gh#199), and the caller is already an
 * authenticated same-user client, so a path is safe to act on. Anything
 * else must name a workspace the host created.
 *
 * @param handle Engine handle.
 * @param workspace Path or name from the ask arguments.
 * @param[out] err Set on failure.
 * @return Workspace name, or "" with `err` set.
 * @dg_internal
 * @version 2.14.0
 */
static std::string resolve_workspace(entropic_handle_t handle,
                                     const std::string& workspace,
                                     std::string& err) {
    namespace fs = std::filesystem;
    std::string name = workspace;
    if (fs::path(workspace).is_absolute()) {
        std::error_code ec;
        name = fs::weakly_canonical(workspace, ec).string();
        bool exists = false;
        {
            std::lock_guard<std::mutex> guard(handle->workspace_mutex);
            exists = handle->workspaces.count(name) > 0;
        }
        if (!exists && entropic_workspace_create(handle, name.c_str(),
                                                 name.c_str()) != ENTROPIC_OK) {
            const char* msg = entropic_last_error(handle);
            err = "workspace '" + workspace + "': "
                + (msg ? msg : "create failed");
        }
    }
    return err.empty() ? name : std::string{};
}

/**
 * @brief Bind a session to the requested workspace, idempotently (gh#205).
 *
 * A client sends `workspace` on every call, so a session already bound to
 * the same workspace is left alone — the engine refuses ANY bind once a
 * session holds messages. A different workspace for a session with history
 * is a typed error: its context belongs to the other repository.
 *
 * @param handle Engine handle.
 * @param session_key Session key.
 * @param workspace Requested workspace path or name ("" = none).
 * @return "" on success, else a message for the caller.
 * @dg_internal
 * @version 2.14.0
 */
static std::string bind_session_workspace(entropic_handle_t handle,
                                          const std::string& session_key,
                                          const std::string& workspace) {
    std::string err;
    if (workspace.empty()) { return err; }
    const std::string name = resolve_workspace(handle, workspace, err);
    const std::string current = err.empty()
        ? bound_workspace(handle, session_key) : std::string{};
    if (err.empty() && current != name) {
        if (entropic_session_bind_workspace(handle, session_key.c_str(),
                                            name.c_str()) != ENTROPIC_OK) {
            const char* msg = entropic_last_error(handle);
            err = msg ? msg : "workspace bind failed";
        }
    }
    return err;
}

/**
 * @brief Run a sync ask once this session's turn comes up (gh#144, gh#205).
 *
 * Queue rather than race: the engine admits one turn per session and
 * refuses the loser with ALREADY_RUNNING; from a shared host the right
 * answer is to WAIT, so the refusal is never reached from here.
 *
 * @param handle Engine handle.
 * @param bridge Bridge owning the queue.
 * @param args Tool arguments.
 * @param session_key Session key.
 * @param client_fd Socket fd (streaming progress).
 * @param progress_token Progress token for streaming.
 * @return MCP tool result JSON.
 * @req REQ-BRIDGE-001
 * @version 2.14.0
 */
static json ask_in_turn(entropic_handle_t handle, ExternalBridge* bridge,
                        const json& args, const std::string& session_key,
                        int client_fd, const json& progress_token) {
    TurnTicket ticket(bridge, session_key);
    if (!ticket.held()) {
        return turn_timeout_result(bridge, ticket.waited_ms());
    }
    return bridge->ask_streaming()
        ? handle_ask(handle, args, session_key, client_fd, progress_token)
        : handle_ask_plain(handle, args, session_key);
}

/**
 * @brief Route entropic.ask — sync (streaming) or async.
 * @param handle Engine handle.
 * @param bridge Bridge (for async task registry).
 * @param args Tool arguments.
 * @param client_fd Socket fd.
 * @param call_id Request id.
 * @return MCP tool result JSON.
 * @req REQ-BRIDGE-001
 * @version 2.12.0-rc1
 */
static json dispatch_ask(entropic_handle_t handle,
                         ExternalBridge* bridge,
                         const json& args,
                         int client_fd,
                         const json& progress_token) {
    // gh#144 (v2.12.0): optional. Absent means the shared default session,
    // i.e. exactly what every pre-2.12.0 client already gets.
    const std::string session_key = args.value("session", std::string{});
    // gh#205 (v2.14.0): bind the session to its repository before it runs.
    const std::string ws_err = bind_session_workspace(
        handle, session_key, args.value("workspace", std::string{}));
    json result;
    if (!ws_err.empty()) {
        result = tool_text("error: " + ws_err);
    } else if (args.value("async", false)) {
        // The async worker takes its own ticket inside run_async_ask — it
        // must not hold one here, or registering the task would block behind
        // an in-flight turn and defeat the point of async.
        auto task_id = generate_task_id();
        bridge->run_async_ask(
            args.value("prompt", ""), task_id, client_fd, session_key,
            args.value("tier", std::string{}));
        result = tool_text("async task started: " + task_id);
    } else {
        result = ask_in_turn(handle, bridge, args, session_key, client_fd,
                             progress_token);
    }
    return result;
}

/**
 * @brief Dispatch a tools/call to the appropriate handler.
 *
 * entropic.ask streams progress notifications to client_fd during
 * generation, then returns the final clean text as the tool result.
 * When async=true, spawns a background thread and returns task_id.
 *
 * @param handle Engine handle.
 * @param bridge Bridge instance (for async task registry).
 * @param params JSON-RPC params (name + arguments).
 * @param client_fd Socket fd for streaming (the ask tool only).
 * @param call_id JSON-RPC request id for progress correlation.
 * @return MCP tool result JSON.
 * @req REQ-BRIDGE-001
 * @version 2.14.0
 */
static json dispatch_tool(entropic_handle_t handle,
                          ExternalBridge* bridge,
                          const json& params,
                          int client_fd,
                          const std::string& call_id) {
    std::string name = params.value("name", std::string{});
    json args = params.value("arguments", json::object());
    // gh#145: strip the configured namespace, then match bare suffixes
    // against the same table tool_definitions() advertised from.
    const std::string suffix =
        facade::strip_tool_prefix(
            bridge->config().tool_prefix, name);
    if (suffix == tool_suffix::kAsk) {
        // v2.14.0: the client's own _meta.progressToken when it sent one
        // (MCP), else the request id so pre-2.14.0 clients keep streaming.
        json token = call_id;
        if (params.contains("_meta") && params["_meta"].is_object()
            && params["_meta"].contains("progressToken")) {
            token = params["_meta"]["progressToken"];
        }
        return dispatch_ask(handle, bridge, args, client_fd, token);
    }
    json result;
    if      (suffix == tool_suffix::kAskStatus)    { result = bridge->handle_ask_status(args); }
    else if (suffix == tool_suffix::kStatus)       { result = handle_status(handle, bridge); }
    else if (suffix == tool_suffix::kContextClear) {
        result = handle_clear(handle, bridge,
                              args.value("session", std::string{}));
    } else if (suffix == tool_suffix::kContextCount) {
        result = handle_count(handle, args.value("session", std::string{}));
    }
    else { result = tool_text("error: unknown tool '" + name + "'"); }
    return result;
}

// ── Turn queue (gh#144, v2.12.0) ─────────────────────────

/**
 * @brief Take a ticket and wait for this caller's turn on the engine.
 * @param timeout_ms Give up after this long rather than hang the client.
 * @param waited_ms_out Optional; receives time actually spent waiting.
 * @return true when it is this caller's turn (caller must end_turn_wait()).
 * @req REQ-BRIDGE-001
 * @version 2.12.0
 */
bool ExternalBridge::begin_turn_wait(long timeout_ms, long* waited_ms_out,
                                     const std::string& session_key) {
    const auto t0 = std::chrono::steady_clock::now();
    const std::string key = lane_for(session_key);
    std::unique_lock<std::mutex> lock(turn_mutex_);
    TurnLane& lane = lanes_[key];  // map nodes are stable across inserts
    const uint64_t ticket = lane.next_ticket++;
    const bool first = (ticket == lane.now_serving);
    if (!first) { ++lane.waiters; }

    const bool ours = turn_cv_.wait_for(
        lock, std::chrono::milliseconds(timeout_ms),
        [&] { return lane.now_serving == ticket; });

    if (!first) { --lane.waiters; }
    if (waited_ms_out != nullptr) {
        *waited_ms_out = static_cast<long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count());
    }
    if (!ours) {
        // Timed out. Record the ticket as abandoned so that when the line
        // reaches it, it is skipped rather than waited on forever. Do NOT
        // advance now_serving_ here: it currently names ANOTHER caller's
        // in-flight turn, and stepping over it would hand the engine to two
        // callers at once — the exact race this queue exists to prevent.
        lane.abandoned.insert(ticket);
        return false;
    }
    lane.busy = true;
    return true;
}

/**
 * @brief Hand the turn to the next ticket in line.
 * @req REQ-BRIDGE-001
 * @version 2.12.0
 */
void ExternalBridge::end_turn_wait(const std::string& session_key) {
    {
        std::lock_guard<std::mutex> lock(turn_mutex_);
        auto it = lanes_.find(lane_for(session_key));
        if (it != lanes_.end()) {
            TurnLane& lane = it->second;
            lane.busy = false;
            ++lane.now_serving;
            // Step over anyone who gave up while waiting, or the line stalls
            // on a ticket whose owner is long gone.
            while (lane.abandoned.erase(lane.now_serving) > 0) {
                ++lane.now_serving;
            }
            // An idle lane holds nothing worth keeping.
            if (lane.now_serving == lane.next_ticket) { lanes_.erase(it); }
        }
    }
    turn_cv_.notify_all();
}

/**
 * @brief The lane a session's turns queue in (v2.14.0).
 * @param session_key Session key.
 * @return The key when the engine runs sessions concurrently, else "".
 * @dg_internal
 * @version 2.14.0
 */
std::string ExternalBridge::lane_for(const std::string& session_key) const {
    const bool concurrent = handle_ == nullptr
        || handle_->config.concurrent_sessions;
    return concurrent ? session_key : std::string{};
}

/**
 * @brief Waiters summed over every lane (v2.14.0).
 * @return Callers waiting behind a held turn.
 * @utility
 * @version 2.14.0
 */
size_t ExternalBridge::turn_queue_depth() const {
    std::lock_guard<std::mutex> lock(turn_mutex_);
    size_t n = 0;
    for (const auto& [_, lane] : lanes_) { n += lane.waiters; }
    return n;
}

/**
 * @brief Whether any lane holds a turn (v2.14.0).
 * @return true while some session's turn runs.
 * @utility
 * @version 2.14.0
 */
bool ExternalBridge::turn_in_flight() const {
    std::lock_guard<std::mutex> lock(turn_mutex_);
    bool busy = false;
    for (const auto& [_, lane] : lanes_) { busy = busy || lane.busy; }
    return busy;
}


// ── ExternalBridge ───────────────────────────────────────

/**
 * @brief Construct with engine handle and config.
 * @param handle Engine handle (must outlive the bridge).
 * @param config External MCP configuration.
 * @param project_dir Project directory (for socket path derivation).
 * @req REQ-BRIDGE-001
 * @version 2.0.8
 */
ExternalBridge::ExternalBridge(
    entropic_handle_t handle,
    const ExternalMCPConfig& config,
    const std::filesystem::path& project_dir)
    : handle_(handle), config_(config) {
    socket_path_ = config.socket_path.has_value()
        ? config.socket_path.value()
        : compute_socket_path(project_dir);
}

/**
 * @brief Destructor — stop if running.
 * @req REQ-BRIDGE-001
 * @version 2.0.8
 */
ExternalBridge::~ExternalBridge() {
    stop();
}

/**
 * @brief Prepare the socket containing directory with 0700 perms.
 *
 * Creates the parent directory if absent and ensures it is mode 0700
 * so only the engine owner can enumerate sockets. v2.1.7 hardening
 * (gh#34): explicit perms rather than relying on umask.
 *
 * @param parent Directory path.
 * @req REQ-BRIDGE-001
 * @version 2.1.7
 */
static void prepare_socket_dir(const std::filesystem::path& parent) {
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    ::chmod(parent.c_str(), S_IRWXU);  // 0700
}

/**
 * @brief Reject a pre-existing path that is a symlink or non-socket.
 *
 * Defends against swap-the-socket-file races where an attacker plants
 * a symlink at the expected path before the engine starts. If the
 * path exists and is anything other than a regular unix socket, the
 * bind is refused. v2.1.7 hardening (gh#34).
 *
 * @param path Socket path.
 * @return true if safe to bind (path absent or is a socket).
 * @req REQ-BRIDGE-001
 * @version 2.1.7
 */
static bool socket_path_safe(const std::filesystem::path& path) {
    struct stat st{};
    if (::lstat(path.c_str(), &st) != 0) {
        return errno == ENOENT;  // absent is fine
    }
    bool is_symlink = S_ISLNK(st.st_mode);
    bool is_socket = S_ISSOCK(st.st_mode);
    if (!is_socket || is_symlink) {
        logger->error(
            "Refusing to bind: {} is a {} (expected unix socket)",
            path.string(),
            is_symlink ? "symlink" : "non-socket file");
    }
    return is_socket && !is_symlink;
}

/**
 * @brief Bind+listen on an AF_UNIX socket, applying 0600 perms.
 * @param fd Pre-created socket fd.
 * @param path Bind path (already validated by socket_path_safe).
 * @return true on success.
 * @req REQ-BRIDGE-001
 * @version 2.14.0
 */
static bool bind_and_listen(int fd, const std::filesystem::path& path) {
    auto s = path.string();
    if (s.size() >= sizeof(sockaddr_un::sun_path)) { return false; }
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, s.c_str(),
                 sizeof(addr.sun_path) - 1);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        return false;
    }
    ::chmod(s.c_str(), S_IRUSR | S_IWUSR);  // 0600 — owner only
    // v2.14.0: several clients may connect at once; a backlog of 1 refused
    // the second while the accept loop was busy with the first.
    return listen(fd, SOMAXCONN) == 0;
}

/**
 * @brief Create, bind, and listen on a unix domain socket.
 *
 * v2.1.7 hardening (gh#34): containing dir 0700, socket file 0600,
 * symlink-and-non-socket-safe bind. Path canonicalization + the hash
 * naming scheme (see `compute_socket_path`) already provide a unique
 * per-project path inside `~/.entropic/socks/`; these checks defend
 * against the residual cases where the path is squatted or perms
 * have been loosened.
 *
 * @param path Socket filesystem path.
 * @return Listening fd, or -1 on failure.
 * @req REQ-BRIDGE-001
 * @version 2.14.0
 */
static int create_listen_socket(const std::filesystem::path& path) {
    prepare_socket_dir(path.parent_path());
    if (!socket_path_safe(path)) { return -1; }
    std::filesystem::remove(path);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    bool ok = (fd >= 0) && bind_and_listen(fd, path);
    if (!ok) {
        logger->error("Socket setup failed for {}: {}",
                      path.string(), std::strerror(errno));
        if (fd >= 0) { ::close(fd); }
        return -1;
    }
    // v2.14.0: the old line labelled ~/.entropic as "project_dir(canonical)".
    logger->info("External MCP bridge ready: socket={}", path.string());
    return fd;
}

/**
 * @brief Validate that the connecting peer shares the engine's UID.
 *
 * Defense-in-depth against same-machine cross-user attempts that
 * somehow reach the socket. Even with 0700 containing dir + 0600
 * socket, an SO_PEERCRED check is cheap and pins the trust boundary
 * to "same uid only" explicitly rather than relying on file perms
 * being correctly applied. v2.1.7 hardening (gh#34).
 *
 * @param client_fd Newly accepted client fd.
 * @return true if peer uid matches our euid.
 * @req REQ-BRIDGE-001
 * @version 2.1.7
 */
static bool peer_uid_matches(int client_fd) {
    struct ucred cred{};
    socklen_t len = sizeof(cred);
    if (getsockopt(client_fd, SOL_SOCKET, SO_PEERCRED,
                   &cred, &len) != 0) {
        logger->warn("SO_PEERCRED failed on fd={}: {}",
                     client_fd, std::strerror(errno));
        return false;
    }
    if (cred.uid != ::geteuid()) {
        logger->warn(
            "Rejecting MCP client on fd={}: peer uid {} != engine uid {}",
            client_fd, cred.uid, ::geteuid());
        return false;
    }
    return true;
}

/**
 * @brief Start the background accept loop.
 * @return true if the socket was created and listening.
 * @req REQ-BRIDGE-001
 * @version 2.14.0
 */
bool ExternalBridge::start() {
    // v2.14.0: a key that bounds nothing must say so rather than look like
    // a control (cf. gh#175).
    if (config_.rate_limit != ExternalMCPConfig{}.rate_limit) {
        logger->warn("mcp.external.rate_limit={} is not enforced and is "
                     "ignored", config_.rate_limit);
    }
    // Refuse to start if another handle in this process already owns
    // this socket path. The pre-fix create_listen_socket call unlinks
    // any existing socket file before binding — that's correct for
    // crashed-prior-process recovery, but if a live in-process bridge
    // owned that file, the unlink stole its binding silently. The
    // claim must happen *before* create_listen_socket so a concurrent
    // start() on a colliding path cannot pass the check and then race
    // into the unlink.
    std::error_code ec;
    auto canonical =
        std::filesystem::weakly_canonical(socket_path_, ec).string();
    if (ec) { canonical = socket_path_.string(); }
    {
        std::lock_guard lk(s_bound_sockets_mu);
        if (!s_bound_sockets.insert(canonical).second) {
            logger->warn(
                "External MCP bridge: socket {} already bound by another "
                "handle in this process; declining to start. Set "
                "external.socket_path to a distinct path per handle "
                "if you need per-handle bridges.",
                canonical);
            return false;
        }
    }

    listen_fd_ = create_listen_socket(socket_path_);
    if (listen_fd_ < 0) {
        std::lock_guard lk(s_bound_sockets_mu);
        s_bound_sockets.erase(canonical);
        return false;
    }
    bound_canonical_ = canonical;

    running_.store(true);
    // gh#59 (v2.3.1): the accept thread (and the per-client serve
    // threads it spawns) need to log on this handle's behalf — install
    // a HandleLogScope at the top of each thread body so spdlog lines
    // route through HandleAwareSink to this handle's session.log.
    int log_id = handle_ ? handle_->log_id : 0;
    accept_thread_ = std::thread([this, log_id]() {
        entropic::log::HandleLogScope scope(log_id);
        accept_loop();
    });

    logger->info("External MCP bridge listening on {}",
                 socket_path_.string());
    return true;
}

/**
 * @brief Stop the accept loop, drain client threads, and close the socket.
 *
 * Issue #4 (v2.1.2, part D): pre-2.1.2 stop() only joined the accept
 * thread because serve_client ran inline. Now each connected client
 * has its own thread blocking in read(); we wake them via
 * shutdown(SHUT_RDWR) on the client fd (which causes the pending
 * read to return EOF) and then join. Order matters:
 *   1. running_ = false  (accept_loop will exit on next poll cycle)
 *   2. close(listen_fd_) (stops new connections; in-flight clients
 *      are unaffected)
 *   3. join accept_thread_ (no more clients spawn after this)
 *   4. shutdown each client fd to wake blocking read()s
 *   5. join each client thread
 *   6. close client fds (the threads' RAII guards do this too, but
 *      we run it again as a defensive measure on shutdown errors)
 *
 * @req REQ-BRIDGE-001
 * @version 2.14.2
 */
void ExternalBridge::stop() {
    running_.store(false);
    // v2.14.0: the accept thread READS listen_fd_ every poll, so it must be
    // joined before listen_fd_ is written. shutdown() wakes a blocked
    // poll/accept without touching the variable; the loop then sees
    // running_ == false (it re-checks at least every 500 ms regardless).
    // Closing first was a data race TSan reported the first time a test
    // destroyed a handle with the bridge enabled.
    if (listen_fd_ >= 0) { ::shutdown(listen_fd_, SHUT_RDWR); }
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }

    // Wake every connected client's blocking read so the per-client
    // thread can observe the disconnect and exit. shutdown(SHUT_RDWR)
    // on a connected socket returns EOF to the peer; the read in
    // serve_client returns 0 and read_line returns "" → exits.
    std::vector<std::unique_ptr<ClientThread>> drained;
    {
        std::lock_guard<std::mutex> lock(client_threads_mutex_);
        drained = std::move(client_threads_);
        client_threads_.clear();
    }
    for (auto& ct : drained) {
        // v2.14.2: under the client's fd_mutex — its thread may be closing
        // the same descriptor right now (TSan).
        std::lock_guard<std::mutex> fd_lock(ct->fd_mutex);
        if (ct->fd >= 0) { ::shutdown(ct->fd, SHUT_RDWR); }
    }
    for (auto& ct : drained) {
        if (ct->thread.joinable()) { ct->thread.join(); }
    }
    join_async_workers();  // v2.14.2: no worker may outlive the bridge

    // Clean up socket file
    std::error_code ec;
    std::filesystem::remove(socket_path_, ec);

    // Release our claim on the canonical path so a later handle in
    // this process can re-bind. Use the exact string we inserted in
    // start(); recomputing weakly_canonical here could differ if the
    // path was deleted mid-run.
    if (!bound_canonical_.empty()) {
        std::lock_guard lk(s_bound_sockets_mu);
        s_bound_sockets.erase(bound_canonical_);
        bound_canonical_.clear();
    }
}

/**
 * @brief Reap finished client threads (called under client_threads_mutex_).
 *
 * Long-running bridges can see many connect/disconnect cycles. Without
 * reaping, the vector grows indefinitely with finished but
 * still-joinable thread handles. Each per-client thread sets its own
 * ``finished`` flag right before exit; the accept loop calls this
 * after every new connection to harvest exited entries.
 *
 * @dg_internal
 * @version 2.1.2
 */
void ExternalBridge::reap_finished_clients_locked() {
    auto it = client_threads_.begin();
    while (it != client_threads_.end()) {
        if ((*it)->finished.load()) {
            if ((*it)->thread.joinable()) { (*it)->thread.join(); }
            it = client_threads_.erase(it);
        } else {
            ++it;
        }
    }
}

/**
 * @brief Background thread: accept connections and dispatch to per-client
 *        serve threads.
 *
 * Issue #4 (v2.1.2, part D): pre-2.1.2 this called ``serve_client(fd)``
 * inline on the accept thread. While one client was connected, the
 * loop blocked here and no further ``accept()`` happened — so a
 * single wedged client also blocked every other operator's recovery
 * attempt and made it impossible for two MCP clients (e.g. TUI +
 * Claude Code) to be connected simultaneously. Now each accepted
 * connection gets its own thread and the accept loop returns
 * immediately to ``poll`` for the next.
 *
 * Lifecycle: the per-client thread closes its fd before exit and
 * sets ``finished``; ``reap_finished_clients_locked()`` then joins
 * and erases. ``stop()`` covers the race where shutdown happens
 * mid-serve by ``shutdown(fd, SHUT_RDWR)`` to wake any blocking
 * read.
 *
 * v2.1.7 (gh#34): every accepted fd is gated by SO_PEERCRED — non-
 * matching uid is closed before a serve thread is spawned.
 *
 * @req REQ-BRIDGE-001
 * @version 2.14.2
 */
void ExternalBridge::accept_loop() {
    while (running_.load()) {
        struct pollfd pfd{};
        pfd.fd = listen_fd_;
        pfd.events = POLLIN;

        int rc = poll(&pfd, 1, 500);  // 500ms timeout for shutdown check
        if (rc <= 0) { continue; }

        int client_fd = accept(listen_fd_, nullptr, nullptr);
        if (client_fd < 0) { continue; }

        if (!peer_uid_matches(client_fd)) {
            ::close(client_fd);  // v2.1.7 (gh#34): cross-uid attempt
            continue;
        }

        logger->info("External MCP client connected (fd={})", client_fd);

        auto ct = std::make_unique<ClientThread>();
        ct->fd = client_fd;
        // Capture a raw pointer for the thread body so the unique_ptr
        // can stay in client_threads_ without aliasing.
        ClientThread* raw = ct.get();
        // gh#59 (v2.3.1): propagate the handle log scope into the
        // per-client thread so its `serve_client` logs route to the
        // owning handle's session.log.
        int log_id = handle_ ? handle_->log_id : 0;
        ct->thread = std::thread([this, raw, log_id]() {
            entropic::log::HandleLogScope scope(log_id);
            serve_client(raw->fd);
            {
                std::lock_guard<std::mutex> fd_lock(raw->fd_mutex);
                ::close(raw->fd);
                raw->fd = -1;
            }
            logger->info("External MCP client disconnected");
            raw->finished.store(true);
        });

        std::lock_guard<std::mutex> lock(client_threads_mutex_);
        client_threads_.push_back(std::move(ct));
        reap_finished_clients_locked();
    }
}

/**
 * @brief Read one newline-delimited line from a socket fd.
 * @param fd Socket file descriptor.
 * @return Line content, or empty on error/disconnect.
 * @utility
 * @version 2.0.8
 */
static std::string read_line(int fd) {
    std::string line;
    char c;
    while (true) {
        ssize_t n = read(fd, &c, 1);
        if (n <= 0) { return {}; }
        if (c == '\n') { return line; }
        line += c;
    }
}

/**
 * @brief Serve a single connected client until disconnect.
 *
 * Dispatches each line as a JSON-RPC message. Notifications
 * (no id) produce an empty response — nothing is written back.
 *
 * @param client_fd Connected socket file descriptor.
 * @req REQ-BRIDGE-001
 * @version 2.0.6-rc16
 */
void ExternalBridge::serve_client(int client_fd) {
    subscribe(client_fd);
    struct Unsub {
        ExternalBridge* self;
        int fd;
        ~Unsub() { self->unsubscribe(fd); }
    } guard{this, client_fd};

    while (running_.load()) {
        auto line = read_line(client_fd);
        if (line.empty()) { break; }

        auto response = dispatch(line, client_fd);
        if (response.empty()) { continue; }  // notification — no reply
        response += '\n';

        ssize_t written = write(client_fd, response.c_str(),
                                response.size());
        if (written < 0) { break; }
    }
}

/**
 * @brief Build the MCP initialize response payload.
 *
 * gh#145 (v2.12.0): `serverInfo.name` comes from `mcp.external.server_name`
 * (default "entropic"). This is the string a PERSON reads in their MCP server
 * list, which is why it is configured separately from `tool_prefix` — the
 * latter is what a MODEL reasons over. `version` remains the engine's own
 * version: the host owns its identity, but must not misreport the engine
 * behind it.
 *
 * @param cfg External MCP config supplying the server name.
 * @return JSON result object.
 * @utility
 * @version 2.12.0
 */
static json initialize_result(const ExternalMCPConfig& cfg) {
    const std::string name =
        cfg.server_name.empty() ? "entropic" : cfg.server_name;
    return {
        {"protocolVersion", "2025-06-18"},
        {"serverInfo", {{"name", name},
                        {"version", entropic_version()}}},
        {"capabilities", {{"tools", json::object()}}}
    };
}

/**
 * @brief Dispatch a JSON-RPC request and return the response.
 *
 * Per JSON-RPC 2.0 spec, messages without an "id" field are
 * notifications and MUST NOT receive a response. Returns empty
 * string for notifications so the caller knows not to write back.
 *
 * @param request Raw JSON-RPC request string.
 * @param client_fd Socket fd for streaming progress (entropic.ask).
 * @return JSON-RPC response string, or empty for notifications.
 * @req REQ-BRIDGE-001
 * @version 2.12.0
 */
std::string ExternalBridge::dispatch(
    const std::string& request, int client_fd) {
    auto req = json::parse(request, nullptr, false);
    // Parse error or notification (no id) → no dispatch
    if (req.is_discarded() || !req.contains("id")) {
        return req.is_discarded()
            ? rpc_err(nullptr, -32700, "Parse error")
            : std::string{};
    }

    json id = req["id"];
    std::string method = req.value("method", std::string{});
    json params = req.value("params", json::object());
    json result;

    if      (method == "initialize")  { result = initialize_result(config_); }
    else if (method == "tools/list")  { result = {{"tools", tool_definitions(config_)}}; }
    else if (method == "tools/call")  {
        auto id_str = id.is_string() ? id.get<std::string>()
                                     : id.dump();
        result = dispatch_tool(handle_, this, params, client_fd, id_str);
    }
    else if (method == "shutdown" || method == "exit") { result = json::object(); }
    else { return rpc_err(id, -32601, "Unknown method: " + method); }
    return rpc_ok(id, result);
}

// ── Async task support ───────────────────────────────────

/**
 * @brief State observer that projects VERIFYING onto task phase.
 * @dg_internal
 * @version 2.1.0
 */
static void phase_observer_cb(int state, void* ud) {
    auto* self = static_cast<ExternalBridge*>(ud);
    if (state != ENTROPIC_AGENT_STATE_VERIFYING) { return; }
    std::lock_guard<std::mutex> lock(self->tasks_mutex_);
    // E5+E6 (2.1.0): discard stale callbacks fired after detach_phase_observer
    // incremented observer_gen_. attached_gen_ was captured at attach time.
    if (self->observer_call_is_stale()) { return; }
    auto it = self->tasks_for_cancel().find(self->active_task_id_for_observer());
    if (it == self->tasks_for_cancel().end()) { return; }
    // First VERIFYING = "validating"; subsequent VERIFYING transitions
    // on the same task indicate revision retries → "revising".
    it->second.phase = (it->second.phase == "validating"
                         || it->second.phase == "revising")
        ? "revising" : "validating";
}

/**
 * @brief Install phase observer scoped to one task.
 *
 * Increments observer_gen_ under tasks_mutex_ and captures it as
 * attached_gen_ so phase_observer_cb can detect stale post-detach
 * fires via a simple generation comparison. (E5+E6, 2.1.0)
 *
 * @param task_id Task whose phase transitions the observer tracks.
 * @req REQ-BRIDGE-001
 * @version 2.1.0
 */
void ExternalBridge::attach_phase_observer(const std::string& task_id) {
    {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        active_task_id_ = task_id;
        attached_gen_ = ++observer_gen_;
    }
    entropic_set_state_observer(handle_, phase_observer_cb, this);
}

/**
 * @brief Clear phase observer and the active-task pointer.
 *
 * Increments observer_gen_ under the lock before clearing
 * active_task_id_ — any in-flight phase_observer_cb will see a
 * generation mismatch and return immediately without accessing
 * stale state. entropic_set_state_observer(nullptr) is called after
 * the lock is released. (E5+E6, 2.1.0)
 *
 * @req REQ-BRIDGE-001
 * @version 2.1.0
 */
void ExternalBridge::detach_phase_observer() {
    {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        ++observer_gen_;
        active_task_id_.clear();
    }
    entropic_set_state_observer(handle_, nullptr, nullptr);
}

/**
 * @brief Record an async task's terminal state and write its sentinel.
 *
 * Extracted from run_async_ask (gh#144, v2.12.0) to keep it inside the SLOC
 * and ABC gates once the turn-queue wait was added.
 *
 * Issue #12 (v2.1.4): the sentinel is written UNDER tasks_mutex_, before the
 * MCP notification fires, so an external monitor reacting to the sentinel can
 * immediately call the ask_status tool and see the same terminal state.
 *
 * @param task_id Task reaching a terminal state.
 * @param status Terminal status string.
 * @param phase Terminal phase string.
 * @param text Result or error text.
 * @dg_internal
 * @version 2.12.0
 */
void ExternalBridge::commit_async_final_state(
    const std::string& task_id, const std::string& status,
    const std::string& phase, const std::string& text) {
    std::lock_guard<std::mutex> lock(tasks_mutex_);
    auto it = tasks_.find(task_id);
    if (it != tasks_.end()) {
        it->second.status = status;
        it->second.phase = phase;
        it->second.result = text;
    }
    write_sentinel(task_id, status);
}

/**
 * @brief Mark an async task failed because its turn never came (gh#144).
 *
 * The task stays "queued" until the ticket is actually held, so ask_status
 * reports the truth rather than claiming to run while blocked. On timeout it
 * becomes a terminal error with the wait recorded, and the sentinel is
 * written under tasks_mutex_ like every other terminal transition.
 *
 * @param task_id Task that gave up waiting.
 * @param waited_ms How long it waited before giving up.
 * @dg_internal
 * @version 2.12.0
 */
void ExternalBridge::fail_task_engine_busy(const std::string& task_id,
                                           long waited_ms) {
    update_task_phase(task_id, "error", "engine busy");
    std::lock_guard<std::mutex> lock(tasks_mutex_);
    auto it = tasks_.find(task_id);
    if (it != tasks_.end()) {
        it->second.status = "error";
        it->second.result = "engine busy — waited "
            + std::to_string(waited_ms) + " ms for a turn";
        write_sentinel(task_id, "error");
    }
}

/**
 * @brief The completion notification for an async task (Issue #4, v2.1.2).
 *
 * The spec-defined notifications/progress rather than the old non-spec
 * notifications/ask_complete, which some clients silently buffered or
 * stalled on. progressToken is the task_id, correlating it back to the
 * ask that returned that id. The result body is NOT shipped inline —
 * consumers fetch it via ask_status — which caps the notification at ~200
 * bytes whatever was generated; `status` rides in `message` so a consumer
 * can branch on done / error / cancelled without a round-trip.
 *
 * @param task_id Task id.
 * @param status Terminal status.
 * @return JSON-RPC notification.
 * @utility
 * @version 2.14.0
 */
static json task_complete_notification(const std::string& task_id,
                                       const std::string& status) {
    return {
        {"jsonrpc", "2.0"},
        {"method", "notifications/progress"},
        {"params", {
            {"progressToken", task_id},
            {"progress", 100},
            {"total", 100},
            {"message", status}
        }}
    };
}

/**
 * @brief Run an async entropic.ask in a detached background thread.
 *
 * Creates a task registry entry, runs the engine, stores the result,
 * writes the per-task sentinel file (#12, v2.1.4), and broadcasts
 * notifications/progress to subscribed clients.
 *
 * @param prompt User prompt.
 * @param task_id Assigned task ID.
 * @param client_fd Socket fd for completion notification.
 * @req REQ-BRIDGE-001
 * @version 2.14.2
 */
void ExternalBridge::run_async_ask(
    const std::string& prompt,
    const std::string& task_id,
    int client_fd,
    const std::string& session_key,
    const std::string& tier) {
    {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        AsyncTask t;
        t.status = "queued";
        t.phase = "queued";
        t.created = std::chrono::steady_clock::now();
        t.session_key = session_key;  // v2.14.0: clear cancels per session
        t.owner_fd = client_fd;       // v2.14.0: only the owner is notified
        tasks_[task_id] = std::move(t);
    }

    // gh#59 (v2.3.1): the async-ask worker runs entropic_run on this
    // handle's behalf — scope its logs to the owning handle.
    int log_id = handle_ ? handle_->log_id : 0;
    // v2.14.2: owned and joined by stop(), no longer detached — a detached
    // worker outliving the bridge touched freed memory (TSan).
    auto worker = std::make_unique<AsyncWorker>();
    AsyncWorker* raw = worker.get();
    std::lock_guard<std::mutex> lock(async_workers_mutex_);
    reap_finished_async_locked();
    worker->thread = std::thread([this, raw, prompt, task_id, client_fd,
                                  log_id, session_key, tier]() {
        entropic::log::HandleLogScope scope(log_id);
        async_worker(prompt, task_id, client_fd, session_key, tier);
        raw->finished.store(true);
    });
    async_workers_.push_back(std::move(worker));
}

/**
 * @brief Join and forget async workers that have finished (v2.14.2).
 * @dg_internal
 * @version 2.14.2
 */
void ExternalBridge::reap_finished_async_locked() {
    for (auto it = async_workers_.begin(); it != async_workers_.end();) {
        const bool done = (*it)->finished.load();
        if (done && (*it)->thread.joinable()) { (*it)->thread.join(); }
        it = done ? async_workers_.erase(it) : std::next(it);
    }
}

/**
 * @brief Stop and join every async worker (v2.14.2).
 *
 * A running worker is mid-turn on the engine; interrupting bounds the wait
 * to a token instead of a whole turn. Shutdown is handle-wide by nature,
 * so the handle-wide interrupt is the right one here.
 *
 * @dg_internal
 * @version 2.14.2
 */
void ExternalBridge::join_async_workers() {
    std::vector<std::unique_ptr<AsyncWorker>> workers;
    {
        std::lock_guard<std::mutex> lock(async_workers_mutex_);
        workers = std::move(async_workers_);
        async_workers_.clear();
    }
    const bool running = std::any_of(workers.begin(), workers.end(),
        [](const auto& w) { return !w->finished.load(); });
    if (running && handle_ != nullptr && handle_->engine) {
        logger->info("Bridge stopping with {} async ask(s) in flight — "
                     "interrupting the engine", workers.size());
        handle_->engine->interrupt();
    }
    for (auto& w : workers) {
        if (w->thread.joinable()) { w->thread.join(); }
    }
}

/**
 * @brief Body of one async ask: wait, run, record, notify the owner.
 *
 * gh#144: the worker reaches the engine like any other caller and takes the
 * same ticket, so an async ask cannot bypass the queue; the task stays
 * "queued" until the ticket is actually held, so ask_status reports the
 * truth. v2.14.0: it queues in ITS session's lane, may run under a tier,
 * and notifies only the client that started it.
 *
 * @param prompt User prompt.
 * @param task_id Task id.
 * @param client_fd Owning client.
 * @param session_key Session key.
 * @param tier Tier or identity; "" routes as usual.
 * @req REQ-BRIDGE-001
 * @version 2.14.0
 */
void ExternalBridge::async_worker(const std::string& prompt,
                                  const std::string& task_id, int client_fd,
                                  const std::string& session_key,
                                  const std::string& tier) {
    long waited_ms = 0;
    if (!begin_turn_wait(kTurnWaitTimeoutMs, &waited_ms, session_key)) {
        fail_task_engine_busy(task_id, waited_ms);
        return;
    }
    update_task_phase(task_id, "running", "running");
    attach_phase_observer(task_id);
    char* result_json = nullptr;
    auto err = run_plain_turn(handle_, session_key, tier, prompt,
                              &result_json);
    detach_phase_observer();
    end_turn_wait(session_key);

    auto final_state = derive_async_final_state(handle_, err, result_json);
    commit_async_final_state(task_id, final_state.status,
                             final_state.phase, final_state.text);
    notify_client(client_fd,
                  task_complete_notification(task_id, final_state.status));
    cleanup_expired_tasks();
    logger->info("Async task {} completed: {}", task_id, final_state.status);
}

/**
 * @brief Remove tasks older than 15 minutes from the registry.
 * @dg_internal
 * @version 2.0.11
 */
/**
 * @brief Add a connected fd to the subscriber set.
 * @param fd Client socket fd.
 * @req REQ-BRIDGE-001
 * @version 2.0.6-rc16
 */
void ExternalBridge::subscribe(int fd) {
    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    subscribers_.insert(fd);
}

/**
 * @brief Remove an fd from the subscriber set.
 * @param fd Client socket fd being closed.
 * @req REQ-BRIDGE-001
 * @version 2.0.6-rc16
 */
void ExternalBridge::unsubscribe(int fd) {
    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    subscribers_.erase(fd);
}

/**
 * @brief One non-blocking write of a whole payload to one fd.
 *
 * EAGAIN / EWOULDBLOCK (a slow consumer's full buffer), EBADF / EPIPE /
 * ECONNRESET (a dead peer) and a partial write all collapse to "drop" —
 * the long-term per-subscriber queue lives in proposal P2-20260429-001.
 *
 * @param fd Destination.
 * @param payload Bytes to write.
 * @return true when every byte was written.
 * @utility
 * @version 2.14.0
 */
static bool send_nonblocking(int fd, const std::string& payload) {
    ssize_t rc = ::send(fd, payload.c_str(), payload.size(),
                        MSG_DONTWAIT | MSG_NOSIGNAL);
    if (rc < 0) {
        logger->warn("Subscriber fd {} send failed (errno={}) — dropping",
                     fd, errno);
    } else if (static_cast<size_t>(rc) < payload.size()) {
        logger->warn("Subscriber fd {} partial send ({}/{}) — dropping",
                     fd, rc, payload.size());
    }
    return rc >= 0 && static_cast<size_t>(rc) == payload.size();
}

/**
 * @brief Write a notification to ONE subscribed client (v2.14.0).
 *
 * v2.14.2: the membership check and the send happen under ONE hold of
 * subscribers_mutex_. They were split, so a client could unsubscribe and
 * close between them — and once the descriptor number was reused, the
 * notification went to a different client (TSan; same class as gh#208).
 * The send is non-blocking, so holding the lock across it stalls nobody;
 * a client's thread unsubscribes before it closes, so a sender holding the
 * lock never sees a closed or reused fd.
 *
 * @param fd Owning client's fd; negative = nobody.
 * @param notif JSON-RPC notification object.
 * @dg_internal
 * @version 2.14.2
 */
void ExternalBridge::notify_client(int fd, const json& notif) {
    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    if (fd >= 0 && subscribers_.count(fd) > 0
        && !send_nonblocking(fd, notif.dump() + "\n")) {
        subscribers_.erase(fd);
    }
}

/**
 * @brief Broadcast a JSON-RPC notification to every subscriber.
 *
 * Issue #4 (v2.1.2, part C): write is non-blocking via
 * ``send(MSG_DONTWAIT)``. A subscriber whose recv buffer is full
 * (slow / non-draining consumer) returns ``EAGAIN``/``EWOULDBLOCK``
 * and is dropped on the same path as ``EBADF``/``EPIPE``, as is a
 * partial write. Pre-2.1.2 the broadcast used blocking ``write()``, which
 * let one stalled consumer wedge the async-task thread (issue #4). The
 * longer-term replacement is a per-subscriber outbound queue + writer
 * thread (proposal P2-20260429-001).
 *
 * v2.14.2: the sends happen under subscribers_mutex_. The old snapshot-
 * then-send let a subscriber unsubscribe and close mid-broadcast, and a
 * reused descriptor then received another client's notification (TSan).
 * Non-blocking sends make holding the lock cheap, which is why the
 * snapshot was never needed to avoid stalls.
 *
 * @param notif JSON-RPC notification object.
 * @req REQ-BRIDGE-001
 * @version 2.14.2
 */
void ExternalBridge::broadcast_notification(const json& notif) {
    const auto payload = notif.dump() + "\n";
    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    for (auto it = subscribers_.begin(); it != subscribers_.end();) {
        it = send_nonblocking(*it, payload) ? std::next(it) : subscribers_.erase(it);
    }
}

/**
 * @brief Update the status/phase of a tracked task.
 * @param task_id Task identifier.
 * @param status New coarse status string.
 * @param phase New granular phase string.
 * @req REQ-BRIDGE-001
 * @version 2.0.6-rc16
 */
void ExternalBridge::update_task_phase(const std::string& task_id,
                                       const std::string& status,
                                       const std::string& phase) {
    std::lock_guard<std::mutex> lock(tasks_mutex_);
    auto it = tasks_.find(task_id);
    if (it == tasks_.end()) { return; }
    it->second.status = status;
    it->second.phase = phase;
}

/**
 * @brief Remove tasks older than 15 minutes from the registry, AND
 *        delete their sentinel files if present.
 *
 * Issue #12 (v2.1.4): sentinel files would otherwise accumulate in
 * `<log_dir>/async/` indefinitely. Cleanup uses the same TTL as the
 * in-memory registry so an external monitor that consumed the
 * sentinel within 15 minutes still sees a consistent picture.
 *
 * @req REQ-BRIDGE-001
 * @version 2.1.4
 */
void ExternalBridge::cleanup_expired_tasks() {
    auto cutoff = std::chrono::steady_clock::now()
        - std::chrono::minutes(15);
    auto sentinel_dir = async_sentinel_dir();
    std::lock_guard<std::mutex> lock(tasks_mutex_);
    for (auto it = tasks_.begin(); it != tasks_.end(); ) {
        if (it->second.created < cutoff) {
            if (!sentinel_dir.empty()) {
                for (const char* suffix :
                     {".done", ".failed", ".cancelled"}) {
                    std::error_code ec;
                    std::filesystem::remove(
                        sentinel_dir / (it->first + suffix), ec);
                }
            }
            it = tasks_.erase(it);
        } else {
            ++it;
        }
    }
}

/**
 * @brief Resolve the async sentinel directory.
 *
 * Override (set via set_async_sentinel_root) takes precedence; falls
 * back to `<handle_->config.log_dir>/async`. Returns an empty path if
 * neither source is configured. Issue #12 (v2.1.4).
 *
 * @return Sentinel directory path, or an empty path when neither an
 *        override nor a configured log_dir is available.
 * @req REQ-BRIDGE-001
 * @version 2.1.4
 */
std::filesystem::path ExternalBridge::async_sentinel_dir() const {
    std::filesystem::path root = async_sentinel_root_override_;
    if (root.empty() && handle_ != nullptr) {
        root = handle_->config.log_dir;
    }
    return root.empty() ? std::filesystem::path{} : (root / "async");
}

/**
 * @brief Override the async sentinel root directory. Issue #12 (v2.1.4).
 * @req REQ-BRIDGE-001
 * @version 2.1.4
 */
void ExternalBridge::set_async_sentinel_root(
    const std::filesystem::path& root) {
    async_sentinel_root_override_ = root;
}

/**
 * @brief Map a terminal status string to a sentinel filename suffix.
 * @dg_internal
 * @version 2.1.4
 */
static const char* sentinel_suffix_for_status(
    const std::string& status) {
    const char* suffix = ".done";
    if (status == "error") {
        suffix = ".failed";
    } else if (status == "cancelled") {
        suffix = ".cancelled";
    }
    return suffix;
}

/**
 * @brief Write the sentinel file for an async task completion.
 *
 * Issue #12 (v2.1.4). Caller MUST hold tasks_mutex_ — this is invoked
 * inside run_async_ask's terminal critical section so external
 * monitors observe a consistent sentinel + registry pair.
 *
 * Failure to create the parent directory or open the file is logged
 * at WARN and swallowed; the MCP notification path remains the
 * primary signal and a missing sentinel is non-fatal for callers
 * that don't depend on it.
 *
 * @req REQ-BRIDGE-001
 * @version 2.1.4
 */
void ExternalBridge::write_sentinel(const std::string& task_id,
                                    const std::string& status) {
    auto dir = async_sentinel_dir();
    if (dir.empty()) { return; }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        logger->warn("write_sentinel: mkdir {} failed: {}",
                     dir.string(), ec.message());
        return;
    }
    auto path = dir / (task_id + sentinel_suffix_for_status(status));
    std::ofstream out(path);
    if (!out.is_open()) {
        logger->warn("write_sentinel: open {} failed", path.string());
        return;
    }
    out << status << '\n';
}

} // namespace entropic
