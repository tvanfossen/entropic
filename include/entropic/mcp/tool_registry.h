/**
 * @file tool_registry.h
 * @brief Tool registration, lookup, and dispatch.
 *
 * Each MCPServerBase owns a ToolRegistry. Tools register at server
 * construction time. The registry handles list_tools and dispatch.
 *
 * @version 1.8.5
 */

#pragma once

#include <entropic/mcp/tool_base.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace entropic {

/**
 * @brief Manages a collection of ToolBase instances.
 *
 * Replaces the dict-dispatch boilerplate repeated across servers.
 * Servers call register_tool() during construction and the registry
 * handles get_tools() and dispatch() automatically.
 *
 * @version 1.8.5
 */
class ToolRegistry {
public:
    /**
     * @brief Register a tool instance.
     * @param tool Non-owning pointer to a ToolBase. Caller retains ownership.
     * @version 1.8.5
     */
    void register_tool(ToolBase* tool);

    /**
     * @brief Check if a tool is registered by name.
     * @param name Tool name.
     * @return true if registered.
     * @version 1.8.5
     */
    bool has_tool(const std::string& name) const;

    /**
     * @brief Get all registered tool definitions as a JSON array string.
     * @return JSON array of tool definitions. Caller-owned string.
     * @version 1.8.5
     */
    std::string get_tools_json() const;

    /**
     * @brief Get all registered tool definitions.
     * @return Vector of ToolDefinition references.
     * @version 1.8.5
     */
    std::vector<const ToolDefinition*> get_definitions() const;

    /**
     * @brief Dispatch a tool call to the registered tool's execute().
     * @param name Tool name.
     * @param args_json JSON arguments string.
     * @return ServerResponse from the tool, or error response if unknown.
     * @version 1.8.5
     */
    ServerResponse dispatch(const std::string& name,
                            const std::string& args_json);

    /**
     * @brief Get a registered tool by name.
     * @param name Tool name.
     * @return Tool pointer, or nullptr if not found.
     * @version 1.8.5
     */
    ToolBase* get_tool(const std::string& name) const;

private:
    std::unordered_map<std::string, ToolBase*> tools_; ///< Name → tool
};

} // namespace entropic
