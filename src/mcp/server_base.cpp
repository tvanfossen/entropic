/**
 * @file server_base.cpp
 * @brief MCPServerBase implementation.
 * @version 1.8.5
 */

#include <entropic/mcp/server_base.h>
#include <entropic/types/logging.h>

#include <nlohmann/json.hpp>

static auto logger = entropic::log::get("mcp.server_base");

namespace entropic {

/**
 * @brief Construct with server name.
 * @param name Server name.
 * @internal
 * @version 1.8.5
 */
MCPServerBase::MCPServerBase(std::string name)
    : name_(std::move(name)) {}

/**
 * @brief Get the server name.
 * @return Server name.
 * @internal
 * @version 1.8.5
 */
const std::string& MCPServerBase::name() const {
    return name_;
}

/**
 * @brief Register a tool with this server.
 * @param tool Tool pointer (non-owning).
 * @internal
 * @version 1.8.5
 */
void MCPServerBase::register_tool(ToolBase* tool) {
    registry_.register_tool(tool);
}

/**
 * @brief List all registered tools as JSON array.
 * @return JSON array string.
 * @internal
 * @version 1.8.5
 */
std::string MCPServerBase::list_tools() const {
    return registry_.get_tools_json();
}

/**
 * @brief Execute a tool and return ServerResponse JSON.
 * @param tool_name Tool name (without server prefix).
 * @param args_json JSON arguments.
 * @return ServerResponse JSON envelope.
 * @internal
 * @version 1.8.5
 */
std::string MCPServerBase::execute(
    const std::string& tool_name,
    const std::string& args_json) {
    logger->info("[EXECUTE] {}.{}", name_, tool_name);

    auto response = registry_.dispatch(tool_name, args_json);

    auto* tool = registry_.get_tool(tool_name);
    if (tool != nullptr) {
        inject_anchor_if_needed(*tool, args_json, response);
    }

    return serialize_response(response);
}

/**
 * @brief Default permission pattern: tool-level.
 * @param tool_name Fully-qualified tool name.
 * @param args_json Tool arguments (unused in default).
 * @return tool_name as-is.
 * @internal
 * @version 1.8.5
 */
std::string MCPServerBase::get_permission_pattern(
    const std::string& tool_name,
    const std::string& /*args_json*/) const {
    return tool_name;
}

/**
 * @brief Default: do not skip duplicate check.
 * @param tool_name Tool name (unused in default).
 * @return false.
 * @internal
 * @version 1.8.5
 */
bool MCPServerBase::skip_duplicate_check(
    const std::string& /*tool_name*/) const {
    return false;
}

/**
 * @brief Default configure: no-op.
 * @param config_json Configuration JSON (unused in default).
 * @return true.
 * @internal
 * @version 1.8.5
 */
bool MCPServerBase::configure(const std::string& /*config_json*/) {
    return true;
}

/**
 * @brief Default set_working_dir: no-op.
 * @param path Working directory (unused in default).
 * @return true.
 * @internal
 * @version 1.8.5
 */
bool MCPServerBase::set_working_dir(const std::string& /*path*/) {
    return true;
}

/**
 * @brief Serialize ServerResponse to JSON envelope.
 * @param response Response to serialize.
 * @return JSON string.
 * @internal
 * @version 1.8.5
 */
/**
 * @brief Map directive type enum to wire-format string.
 * @param type Directive type.
 * @return Type string.
 * @internal
 * @version 1.8.5
 */
/**
 * @brief Directive type enum → wire-format string lookup.
 * @internal
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
 * @param type Directive type.
 * @return Type string.
 * @internal
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
 * @param response Response to serialize.
 * @return JSON string.
 * @internal
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
 * @param tool Tool that was executed.
 * @param args_json Original arguments.
 * @param response Response to augment.
 * @internal
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
