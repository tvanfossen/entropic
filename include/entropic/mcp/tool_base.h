// SPDX-License-Identifier: Apache-2.0
/**
 * @file tool_base.h
 * @brief Abstract base class for individual MCP tools.
 *
 * Each tool owns its definition (name, description, JSON schema) and
 * implements execute(). Tools are registered with a ToolRegistry inside
 * an MCPServerBase.
 *
 * @version 2.14.0
 */

#pragma once

#include <entropic/types/config.h>

#include <string>

namespace entropic {

struct ServerResponse; // forward declaration (server_base.h)

/**
 * @brief Parsed tool definition from JSON schema file.
 *
 * v2.14.0 (gh#203): `output_schema` is the tool's declared response
 * contract — MCP's `outputSchema`. For a bundled tool it is required, and
 * the description's "Response format" section is GENERATED from it, so the
 * shape a model is told and the shape a handler emits have one source.
 *
 * @version 2.14.0
 */
struct ToolDefinition {
    std::string name;            ///< Tool name (e.g., "read_file")
    std::string description;     ///< Human-readable description
    std::string input_schema;    ///< JSON Schema for arguments (raw JSON string)
    std::string output_schema;   ///< JSON Schema for the result ("" = undeclared). v2.14.0
};

/**
 * @brief Abstract base class for individual MCP tools.
 *
 * Subclass to create tools. Implement execute() with the tool's
 * behavior. Provide the definition via constructor (from JSON file
 * or inline).
 *
 * Dependencies (file tracker, config, etc.) are injected via the
 * subclass constructor.
 *
 * @version 1.8.5
 */
class ToolBase {
public:
    virtual ~ToolBase() = default;

    /**
     * @brief Construct with a pre-built definition.
     * @param def Tool definition.
     * @version 1.8.5
     */
    explicit ToolBase(ToolDefinition def);

    /**
     * @brief Get the tool name.
     * @return Tool name from definition.
     * @version 1.8.5
     */
    const std::string& name() const;

    /**
     * @brief Get the full tool definition.
     * @return Tool definition reference.
     * @version 1.8.5
     */
    const ToolDefinition& definition() const;

    /**
     * @brief Execute this tool with the given arguments.
     * @param args_json JSON string of arguments.
     * @return ServerResponse with result text and optional directives.
     * @version 1.8.5
     */
    virtual ServerResponse execute(const std::string& args_json) = 0;

    /**
     * @brief Generate anchor key for this tool result.
     * @param args_json Tool call arguments as JSON string.
     * @return Anchor key string, or empty string for no anchor.
     * @version 1.8.5
     *
     * Override in tools whose results should REPLACE previous results
     * with the same key rather than accumulating in context.
     * Default returns "" (no anchoring — results append normally).
     */
    virtual std::string anchor_key(const std::string& args_json) const;

    /**
     * @brief Minimum access level required to execute this tool.
     * @return MCPAccessLevel::WRITE by default (safe default).
     * @version 1.9.4
     *
     * Override in read-only tools (e.g., read_file, list_directory).
     * The ToolExecutor checks this against the caller's MCPKeySet
     * before executing.
     */
    virtual MCPAccessLevel required_access_level() const;

protected:
    ToolDefinition definition_; ///< Tool definition
};

/**
 * @brief Load a tool definition from a JSON file.
 * @param tool_name Tool name (e.g., "read_file").
 * @param server_prefix Server directory name (e.g., "filesystem").
 * @param data_dir Base directory for tool JSON files.
 * @return Parsed ToolDefinition.
 * @throws std::runtime_error if file not found or invalid.
 * @utility
 * @version 1.8.5
 */
ToolDefinition load_tool_definition(
    const std::string& tool_name,
    const std::string& server_prefix,
    const std::string& data_dir);

/**
 * @brief Render an outputSchema as the compact "Response format" text a
 *        model reads (gh#203).
 *
 * Every shape, key, optional marker (`"key"?`), enumerated error code and
 * field description in the schema appears in the text; nothing else does.
 *
 * @param output_schema_json JSON Schema (raw string).
 * @return The rendered section (leading blank line included), or "" for an
 *         empty or unparseable schema.
 * @utility
 * @version 2.14.0
 */
std::string render_response_format(const std::string& output_schema_json);

/**
 * @brief A declared tool error: `{"error": code, "message": message}`.
 *
 * v2.14.0 (gh#203): the one shape every bundled tool refusal takes, with a
 * code its outputSchema enumerates.
 *
 * @param code Error code.
 * @param message Human-readable message.
 * @return Serialized JSON object.
 * @utility
 * @version 2.14.0
 */
std::string tool_error_json(const std::string& code, const std::string& message);

} // namespace entropic
