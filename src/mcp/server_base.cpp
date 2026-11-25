// SPDX-License-Identifier: Apache-2.0
/**
 * @file server_base.cpp
 * @brief MCPServerBase implementation.
 * @version 1.8.5
 */

#include <entropic/mcp/server_base.h>
#include <entropic/types/logging.h>

#include <nlohmann/json.hpp>

#include <exception>

static auto logger = entropic::log::get("mcp.server_base");

namespace entropic {

/**
 * @brief Construct with server name.
 * @param name Server name.
 * @utility
 * @version 1.8.5
 */
MCPServerBase::MCPServerBase(std::string name)
    : name_(std::move(name)) {}

/**
 * @brief Get the server name.
 * @return Server name.
 * @utility
 * @version 1.8.5
 */
const std::string& MCPServerBase::name() const {
    return name_;
}

/**
 * @brief Register a tool with this server.
 *
 * Registration is the one thing every concrete server does in its
 * constructor; the base owns the registry so a subclass cannot lose
 * dispatch, envelope shape, or anchoring by forgetting to wire them.
 *
 * @param tool Tool pointer (non-owning; the server retains ownership of
 *             the tool objects themselves).
 * @req REQ-MCP-001
 * @req REQ-MCP-003
 * @version 1.8.5
 */
void MCPServerBase::register_tool(ToolBase* tool) {
    registry_.register_tool(tool);
}

/**
 * @brief List all registered tools as JSON array.
 *
 * Non-virtual: ServerManager concatenates this across every in-process
 * server so the model sees one flat tool list.
 *
 * @return JSON array string — one object per registered tool carrying
 *         name, description and inputSchema; "[]" when nothing is
 *         registered.
 * @req REQ-MCP-001
 * @req REQ-MCP-007
 * @version 1.8.5
 */
std::string MCPServerBase::list_tools() const {
    return registry_.get_tools_json();
}

/**
 * @brief Coerce arguments that are valid JSON but not an object to `{}`.
 *
 * gh#143 (v2.12.0). Normalises exactly two payloads that mean "no
 * arguments" into the empty object every tool's `inputSchema` declares:
 * an empty/whitespace-only string, and a literal JSON `null`. MCP treats
 * a tool call's `arguments` as optional, and a client that serialises an
 * absent field as `null` is making a well-formed statement — "I sent no
 * arguments" — not a malformed one.
 *
 * `ToolExecutor::serialize_args` fixes the case where ENTROPIC produced
 * the payload. This covers the cases it cannot see: a plugin `.so`, an
 * external MCP server, or any other caller whose "no arguments" reaches
 * us spelled as `null`.
 *
 * DELIBERATELY NOT NORMALISED — an array, a scalar, or anything that
 * fails to parse. Those are malformed emissions, and rewriting them to
 * `{}` would substitute a silent default for a caller error: the model
 * sent something wrong and would never be told. `EntropicServer`'s
 * followup tool already demonstrates the better contract, answering a
 * non-object with a typed "invalid args" naming the actual problem.
 * Masking that would be exactly the silent-fallback failure the repo's
 * fail-fast rule exists to prevent.
 *
 * Those payloads stay unmodified and reach the tool, which either reports
 * them itself or throws — and a throw is caught by the barrier in
 * execute() and returned to the model as a tool error. Survivable either
 * way; never silently reinterpreted.
 *
 * @param tool_name Tool name, for the log line only.
 * @param raw Arguments exactly as the caller supplied them.
 * @return `{}` when `raw` is empty/whitespace or a literal JSON null;
 *         otherwise `raw` unchanged.
 * @req REQ-MCP-013
 * @utility
 * @version 2.12.0
 */
std::string MCPServerBase::normalize_args(
    const std::string& tool_name,
    const std::string& raw) const {
    const auto first = raw.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "{}";
    }
    // Non-throwing parse: a discarded value is unparseable and is left
    // alone on purpose, as is any non-null non-object.
    const auto parsed = nlohmann::json::parse(raw, nullptr, false);
    if (!parsed.is_null()) {
        return raw;
    }
    logger->info(
        "[{}.{}] arguments arrived as JSON null — reading as \"no "
        "arguments\" and dispatching with {{}}",
        name_, tool_name);
    return "{}";
}

/**
 * @brief Execute a tool and return ServerResponse JSON.
 *
 * The base owns the whole dispatch path — registry lookup, automatic
 * ContextAnchor injection for any tool declaring an anchor_key, and
 * envelope serialisation — so a concrete server that overrides nothing
 * still produces the exact shape the DirectiveProcessor parses.
 *
 * gh#143 (v2.12.0): a tool that THROWS is answered the same way as an
 * unknown one — the exception becomes error text in `result`. This is the
 * in-process counterpart of gh#133's `ServerManager::route_plugin_call`,
 * which already guarantees that a misbehaving PLUGIN server "becomes a
 * normal tool error rather than an exception unwinding through the loop".
 * In-process servers had no equivalent, so an unhandled throw — a
 * `.value()` on a non-object (type_error.306), a missing `.at()` key
 * (out_of_range.403) — escaped four uncaught hops up to `run_as_inner` and
 * aborted the entire turn. The model never saw the failure and could not
 * correct it.
 *
 * The barrier is HERE rather than in ToolRegistry::dispatch so that a
 * throwing tool also skips `inject_anchor_if_needed`. A tool that failed
 * must not have its arguments recorded as a ContextAnchor: for
 * FilesystemServer a rejected path traversal would otherwise be anchored
 * into context as though it had been legitimately read.
 *
 * @param tool_name Tool name (without server prefix).
 * @param args_json JSON arguments.
 * @return ServerResponse JSON envelope: a string `result` plus a
 *         `directives` array (empty when the tool has no side effects).
 *         An unknown tool, or one that throws, yields the same envelope
 *         with error text in `result` rather than a throw — and with no
 *         directives, anchor included.
 * @req REQ-MCP-001
 * @req REQ-MCP-002
 * @version 2.12.0
 */
std::string MCPServerBase::execute(
    const std::string& tool_name,
    const std::string& raw_args_json) {
    logger->info("[EXECUTE] {}.{}", name_, tool_name);

    const std::string args_json = normalize_args(tool_name, raw_args_json);

    ServerResponse response;
    try {
        response = registry_.dispatch(tool_name, args_json);

        auto* tool = registry_.get_tool(tool_name);
        if (tool != nullptr) {
            inject_anchor_if_needed(*tool, args_json, response);
        }
    } catch (const std::exception& e) {
        // Full args are logged, never truncated — for this failure class
        // the arguments are the whole diagnosis.
        logger->error("Tool '{}.{}' threw: {} — args: {}",
                      name_, tool_name, e.what(), args_json);
        response.directives.clear();
        response.result =
            "Error: tool '" + name_ + "." + tool_name + "' failed: "
            + e.what();
    }

    return serialize_response(response);
}

/**
 * @brief Default permission pattern: tool-level.
 *
 * One of the four extension points with a safe default. A server that
 * wants coarser or finer "always allow" granularity (BashServer keys on
 * the base command) overrides this; everyone else inherits tool-level.
 *
 * @param tool_name Fully-qualified tool name.
 * @param args_json Tool arguments (unused in the default — the default
 *                  granularity is deliberately argument-independent).
 * @return tool_name as-is, i.e. tool-level permission granularity.
 * @req REQ-MCP-001
 * @req REQ-MCP-009
 * @version 1.8.5
 */
std::string MCPServerBase::get_permission_pattern(
    const std::string& tool_name,
    const std::string& /*args_json*/) const {
    return tool_name;
}

/**
 * @brief Default: do not skip duplicate check.
 *
 * Extension point with a safe default — a server opts an individual tool
 * out only when the tool must always run for its side effect
 * (filesystem.read_file updates the read tracker; entropic.delegate and
 * entropic.pipeline are legitimately repeatable).
 *
 * @param tool_name Tool name (unused in the default).
 * @return false — every tool is duplicate-checked unless a subclass says
 *         otherwise.
 * @req REQ-MCP-001
 * @req REQ-MCP-015
 * @version 1.8.5
 */
bool MCPServerBase::skip_duplicate_check(
    const std::string& /*tool_name*/) const {
    return false;
}

/**
 * @brief Default approval requirement: every tool is gated.
 *
 * Safe default: a server must opt a tool OUT of the host's approval gate.
 *
 * @param tool_name Local tool name (unused in the default).
 * @return true — the approval gate applies.
 * @req REQ-MCP-012
 * @version 2.14.6
 */
bool MCPServerBase::requires_approval(
    const std::string& /*tool_name*/) const {
    return true;
}

/**
 * @brief Default configure: no-op.
 *
 * Extension point with a safe default so a server needing no
 * configuration is not forced to implement one.
 *
 * @param config_json Configuration JSON (unused in the default).
 * @return true — the no-op default always succeeds.
 * @req REQ-MCP-001
 * @version 1.8.5
 */
bool MCPServerBase::configure(const std::string& /*config_json*/) {
    return true;
}

/**
 * @brief Default set_working_dir: no-op.
 *
 * Extension point with a safe default — only directory-aware servers
 * (filesystem, bash, git) need to re-root.
 *
 * @param path Working directory (unused in the default).
 * @return true — the no-op default always succeeds.
 * @req REQ-MCP-001
 * @version 1.8.5
 */
bool MCPServerBase::set_working_dir(const std::string& /*path*/) {
    return true;
}

/**
 * @brief Serialize ServerResponse to JSON envelope.
 * @param response Response to serialize.
 * @return JSON string.
 * @utility
 * @version 1.8.5
 */
/**
 * @brief Map directive type enum to wire-format string.
 * @param type Directive type.
 * @return Type string.
 * @utility
 * @version 1.8.5
 */
/**
 * @brief Directive type enum → wire-format string lookup.
 * @utility
 * @version 1.8.5
 */
static const char* const DIRECTIVE_NAMES[] = {
    "stop_processing",   // 0
    "tier_change",       // 1
    "delegate",          // 2
    "pipeline",          // 3
    "complete",          // 4
    "clear_self_todos",  // 5
    "inject_context",    // 6
    "prune_messages",    // 7
    "context_anchor",    // 8
    "phase_change",      // 9
    "notify_presenter",  // 10
};

/**
 * @brief Map directive type enum to wire-format string.
 *
 * Range-checked on both ends so an out-of-range enum value serialises as
 * "unknown" rather than reading past the name table.
 *
 * @param type Directive type.
 * @return The fixed wire name for an in-range value, "unknown" otherwise.
 * @req REQ-MCP-002
 * @version 1.8.5
 */
static const char* directive_type_name(
    entropic_directive_type_t type) {
    auto idx = static_cast<int>(type);
    constexpr int count = sizeof(DIRECTIVE_NAMES)
                        / sizeof(DIRECTIVE_NAMES[0]);
    if (idx < 0 || idx >= count) {
        return "unknown";
    }
    return DIRECTIVE_NAMES[idx];
}

/**
 * @brief Serialize ServerResponse to JSON envelope.
 *
 * The single envelope shape every server kind — in-process, dlopen
 * plugin, external MCP — answers in, so the routing layer never has to
 * know which kind produced it.
 *
 * @param response Response to serialize.
 * @return JSON object string `{"result":"<text>","directives":[{"type":...}]}`;
 *         `directives` is an empty array for a tool with no side effects.
 * @req REQ-MCP-002
 * @version 1.8.5
 */
std::string MCPServerBase::serialize_response(
    const ServerResponse& response) {
    nlohmann::json j;
    j["result"] = response.result;

    auto directives = nlohmann::json::array();
    for (const auto& d : response.directives) {
        nlohmann::json dj;
        dj["type"] = directive_type_name(d.type);
        directives.push_back(std::move(dj));
    }
    j["directives"] = std::move(directives);

    return j.dump();
}

/**
 * @brief Inject ContextAnchor if tool declares anchor_key.
 *
 * Automatic in the base so anchoring cannot be lost by a new server
 * forgetting to emit the directive itself. A tool whose anchor_key
 * returns empty leaves the directives array untouched.
 *
 * @param tool Tool that was executed.
 * @param args_json Original arguments.
 * @param response Response to augment in place.
 * @req REQ-MCP-001
 * @version 1.8.5
 */
void MCPServerBase::inject_anchor_if_needed(
    const ToolBase& tool,
    const std::string& args_json,
    ServerResponse& response) {
    auto key = tool.anchor_key(args_json);
    if (key.empty()) {
        return;
    }
    Directive anchor;
    anchor.type = ENTROPIC_DIRECTIVE_CONTEXT_ANCHOR;
    response.directives.push_back(std::move(anchor));
    logger->info("Auto-injected ContextAnchor: {}", key);
}

} // namespace entropic
