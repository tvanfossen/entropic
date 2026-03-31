/**
 * @file server_base.h
 * @brief MCPServerBase concrete base class + ServerResponse.
 *
 * MCPServerBase provides 80% of MCP server logic: tool registration,
 * dispatch, permission pattern generation, response wrapping with
 * automatic ContextAnchor injection. Concrete servers (filesystem,
 * bash, etc.) override only the 20% that differs.
 *
 * @version 1.8.5
 */

#pragma once

#include <entropic/core/directives.h>
#include <entropic/mcp/tool_base.h>
#include <entropic/mcp/tool_registry.h>

#include <string>
#include <vector>

namespace entropic {

/**
 * @brief Structured result from tool execution.
 *
 * result is the text shown to the model. directives are typed objects
 * processed by the engine's DirectiveProcessor.
 *
 * @version 1.8.5
 */
struct ServerResponse {
    std::string result;                    ///< Human-readable result
    std::vector<Directive> directives;     ///< Engine-level side effects
};

/**
 * @brief Concrete base class for MCP servers (80% logic).
 *
 * Owns a ToolRegistry. Provides:
 * - Tool registration via register_tool()
 * - Tool listing via list_tools() → JSON array
 * - Tool dispatch via execute() → ServerResponse JSON envelope
 * - Automatic ContextAnchor injection when a tool declares anchor_key()
 * - Permission pattern generation (virtual, default: "server.tool_name")
 * - Duplicate check skip (virtual, default: false)
 * - Working directory management (virtual, default: no-op)
 * - Configuration via configure() (virtual, default: no-op)
 *
 * Concrete servers (FilesystemServer, BashServer, etc.) subclass and:
 * 1. Create ToolBase subclasses for each tool
 * 2. Register them in the constructor
 * 3. Override get_permission_pattern() / skip_duplicate_check() as needed
 *
 * @par Lifecycle
 * @code
 *   auto server = std::make_unique<FilesystemServer>();
 *   server->configure(config_json);  // server-specific init
 *   server->list_tools();            // JSON array of tool defs
 *   auto result = server->execute("read_file", args_json);
 * @endcode
 *
 * @version 1.8.5
 */
class MCPServerBase {
public:
    virtual ~MCPServerBase() = default;

    /**
     * @brief Construct with server name.
     * @param name Server name (e.g., "filesystem", "bash", "git").
     * @version 1.8.5
     */
    explicit MCPServerBase(std::string name);

    /**
     * @brief Get the server name.
     * @return Server name string.
     * @version 1.8.5
     */
    const std::string& name() const;

    /**
     * @brief Register a tool with this server.
     * @param tool Non-owning pointer. Server does NOT take ownership.
     * @version 1.8.5
     */
    void register_tool(ToolBase* tool);

    /**
     * @brief List all registered tools as a JSON array string.
     * @return JSON array of tool definitions.
     * @version 1.8.5
     */
    std::string list_tools() const;

    /**
     * @brief Execute a tool and wrap result in ServerResponse JSON.
     * @param tool_name Tool name (without server prefix).
     * @param args_json JSON string of arguments.
     * @return ServerResponse JSON envelope string.
     * @version 1.8.5
     *
     * Flow:
     * 1. Dispatch to registered ToolBase::execute()
     * 2. If tool declares anchor_key(): auto-inject ContextAnchor directive
     * 3. Serialize to JSON ServerResponse envelope
     */
    std::string execute(const std::string& tool_name,
                        const std::string& args_json);

    /**
     * @brief Generate permission pattern for 'Always Allow/Deny'.
     * @param tool_name Fully-qualified tool name (e.g., "filesystem.read_file").
     * @param args_json Tool call arguments as JSON string.
     * @return Permission pattern string.
     * @version 1.8.5
     *
     * Default: returns tool_name (tool-level granularity).
     * Override in subclasses for finer granularity.
     */
    virtual std::string get_permission_pattern(
        const std::string& tool_name,
        const std::string& args_json) const;

    /**
     * @brief Check if a tool should skip duplicate detection.
     * @param tool_name Local tool name (without server prefix).
     * @return true if duplicate check should be skipped.
     * @version 1.8.5
     *
     * Override for tools with side effects that must always execute
     * (e.g., read_file updates FileAccessTracker).
     */
    virtual bool skip_duplicate_check(
        const std::string& tool_name) const;

    /**
     * @brief Configure the server after creation.
     * @param config_json JSON configuration string.
     * @return true on success.
     * @version 1.8.5
     *
     * Default: no-op, returns true. Override for server-specific init.
     */
    virtual bool configure(const std::string& config_json);

    /**
     * @brief Set the working directory.
     * @param path New working directory path.
     * @return true on success.
     * @version 1.8.5
     *
     * Default: no-op, returns true. Override in directory-aware servers.
     */
    virtual bool set_working_dir(const std::string& path);

    /**
     * @brief Get the tool registry (const).
     * @return Reference to this server's tool registry.
     * @version 1.9.4
     */
    const ToolRegistry& registry() const { return registry_; }

protected:
    std::string name_;         ///< Server name
    ToolRegistry registry_;    ///< Tool registry

private:
    /**
     * @brief Serialize a ServerResponse to JSON envelope.
     * @param response ServerResponse to serialize.
     * @return JSON string.
     * @version 1.8.5
     */
    static std::string serialize_response(const ServerResponse& response);

    /**
     * @brief Inject ContextAnchor directive if tool declares anchor_key.
     * @param tool Tool that was executed.
     * @param args_json Original arguments.
     * @param response Response to potentially augment.
     * @version 1.8.5
     */
    static void inject_anchor_if_needed(
        const ToolBase& tool,
        const std::string& args_json,
        ServerResponse& response);
};

} // namespace entropic
