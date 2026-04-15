// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file tool_registry.cpp
 * @brief ToolRegistry implementation.
 * @version 1.8.5
 */

#include <entropic/mcp/tool_registry.h>
#include <entropic/mcp/server_base.h>
#include <entropic/types/logging.h>

#include <nlohmann/json.hpp>

static auto logger = entropic::log::get("mcp.tool_registry");

namespace entropic {

/**
 * @brief Register a tool instance.
 * @param tool Non-owning pointer to a ToolBase.
 * @internal
 * @version 1.8.5
 */
void ToolRegistry::register_tool(ToolBase* tool) {
    if (tool == nullptr) {
        logger->warn("Attempted to register null tool");
        return;
    }
    const auto& name = tool->name();
    if (tools_.count(name) > 0) {
        logger->warn("Tool '{}' already registered — replacing", name);
    }
    tools_[name] = tool;
    logger->info("Registered tool: {}", name);
}

/**
 * @brief Check if a tool is registered by name.
 * @param name Tool name.
 * @return true if registered.
 * @internal
 * @version 1.8.5
 */
bool ToolRegistry::has_tool(const std::string& name) const {
    return tools_.count(name) > 0;
}

/**
 * @brief Get all registered tool definitions as JSON array.
 * @return JSON array string.
 * @internal
 * @version 1.8.5
 */
std::string ToolRegistry::get_tools_json() const {
    auto arr = nlohmann::json::array();
    for (const auto& [name, tool] : tools_) {
        nlohmann::json entry;
        entry["name"] = tool->definition().name;
        entry["description"] = tool->definition().description;
        entry["inputSchema"] = nlohmann::json::parse(
            tool->definition().input_schema);
        arr.push_back(std::move(entry));
    }
    return arr.dump();
}

/**
 * @brief Get all registered tool definitions.
 * @return Vector of ToolDefinition pointers.
 * @internal
 * @version 1.8.5
 */
std::vector<const ToolDefinition*> ToolRegistry::get_definitions() const {
    std::vector<const ToolDefinition*> defs;
    defs.reserve(tools_.size());
    for (const auto& [name, tool] : tools_) {
        defs.push_back(&tool->definition());
    }
    return defs;
}

/**
 * @brief Dispatch a tool call to the registered tool.
 * @param name Tool name.
 * @param args_json JSON arguments string.
 * @return ServerResponse from the tool.
 * @internal
 * @version 2.0.0
 */
ServerResponse ToolRegistry::dispatch(
    const std::string& name,
    const std::string& args_json) {
    auto it = tools_.find(name);
    if (it == tools_.end()) {
        logger->warn("Unknown tool: {}", name);
        ServerResponse resp;
        resp.result = "Error: Unknown tool '" + name + "'";
        return resp;
    }
    logger->info("Dispatch: tool='{}'", name);
    return it->second->execute(args_json);
}

/**
 * @brief Get a registered tool by name.
 * @param name Tool name.
 * @return Tool pointer, or nullptr if not found.
 * @internal
 * @version 1.8.5
 */
ToolBase* ToolRegistry::get_tool(const std::string& name) const {
    auto it = tools_.find(name);
    if (it == tools_.end()) {
        return nullptr;
    }
    return it->second;
}

} // namespace entropic
