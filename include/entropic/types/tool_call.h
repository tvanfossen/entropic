/**
 * @file tool_call.h
 * @brief Tool call and tool result types.
 *
 * Internal C++ representation. At .so boundaries, tool calls are serialized
 * as JSON strings.
 *
 * @version 1.8.2
 */

#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace entropic {

/**
 * @brief A tool call request parsed from model output.
 *
 * Maps to Python ToolCall dataclass. The adapter parses these from
 * model output content (XML or JSON format depending on adapter).
 *
 * Arguments are stored as string key-value pairs at this level.
 * JSON-typed values are stored as their JSON string representation.
 *
 * @version 1.8.2
 */
struct ToolCall {
    std::string id;                                            ///< Unique call ID (UUID)
    std::string name;                                          ///< Tool name (e.g. "filesystem.read_file")
    std::unordered_map<std::string, std::string> arguments;    ///< Tool arguments as string key-value pairs
    std::string arguments_json;                                ///< Original JSON string (for passthrough dispatch)
};

/**
 * @brief Result of a tool execution.
 *
 * Maps to Python ToolResult dataclass.
 *
 * @version 1.8.2
 */
struct ToolResult {
    std::string call_id;           ///< Matching ToolCall ID
    std::string name;              ///< Tool name
    std::string result;            ///< Result text
    bool is_error = false;         ///< True if tool execution failed
    double duration_ms = 0.0;      ///< Execution time in milliseconds
};

} // namespace entropic
