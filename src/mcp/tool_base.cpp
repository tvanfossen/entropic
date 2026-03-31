/**
 * @file tool_base.cpp
 * @brief ToolBase implementation + load_tool_definition.
 * @version 1.8.5
 */

#include <entropic/mcp/tool_base.h>
#include <entropic/mcp/server_base.h>
#include <entropic/types/logging.h>

#include <nlohmann/json.hpp>

#include <fstream>
#include <stdexcept>

static auto logger = entropic::log::get("mcp.tool_base");

namespace entropic {

/**
 * @brief Construct with a pre-built definition.
 * @param def Tool definition.
 * @internal
 * @version 1.8.5
 */
ToolBase::ToolBase(ToolDefinition def)
    : definition_(std::move(def)) {}

/**
 * @brief Get the tool name.
 * @return Tool name from definition.
 * @internal
 * @version 1.8.5
 */
const std::string& ToolBase::name() const {
    return definition_.name;
}

/**
 * @brief Get the full tool definition.
 * @return Tool definition reference.
 * @internal
 * @version 1.8.5
 */
const ToolDefinition& ToolBase::definition() const {
    return definition_;
}

/**
 * @brief Default anchor_key — no anchoring.
 * @param args_json Tool call arguments (unused).
 * @return Empty string.
 * @internal
 * @version 1.8.5
 */
std::string ToolBase::anchor_key(
    const std::string& /*args_json*/) const {
    return "";
}

/**
 * @brief Default required access level — WRITE (safe default).
 * @return MCPAccessLevel::WRITE.
 * @internal
 * @version 1.9.4
 */
MCPAccessLevel ToolBase::required_access_level() const {
    return MCPAccessLevel::WRITE;
}

/**
 * @brief Load a tool definition from a JSON file.
 * @param tool_name Tool name (e.g., "read_file").
 * @param server_prefix Server directory name (e.g., "filesystem").
 * @param data_dir Base directory for tool JSON files.
 * @return Parsed ToolDefinition.
 * @utility
 * @version 1.8.5
 */
ToolDefinition load_tool_definition(
    const std::string& tool_name,
    const std::string& server_prefix,
    const std::string& data_dir) {

    std::string path = data_dir;
    if (!server_prefix.empty()) {
        path += "/" + server_prefix;
    }
    path += "/" + tool_name + ".json";

    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error(
            "Tool definition not found: " + path);
    }

    auto json = nlohmann::json::parse(file);

    ToolDefinition def;
    def.name = json.at("name").get<std::string>();
    def.description = json.value("description", "");
    def.input_schema = json.at("inputSchema").dump();

    logger->info("Loaded tool definition: {} from {}",
                 def.name, path);
    return def;
}

} // namespace entropic
