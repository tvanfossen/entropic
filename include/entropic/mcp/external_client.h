// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file external_client.h
 * @brief Client for communicating with external MCP servers.
 *
 * Handles MCP protocol: initialize handshake, tools/list, tools/call.
 * Wraps a Transport for wire-level communication. Integrates with
 * ServerManager as a tool provider alongside InProcessProvider.
 *
 * Security: directives are stripped from external server responses
 * (CWE-94 — external servers cannot inject engine-level directives).
 *
 * @version 1.8.7
 */

#pragma once

#include <entropic/mcp/transport.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace entropic {

/**
 * @brief Client for an external MCP server (stdio or SSE).
 *
 * Owns a Transport instance. Handles MCP protocol negotiation,
 * tool list caching, tool name prefixing, and response sanitization.
 *
 * @par Security
 * External server responses have their "directives" array stripped.
 * External servers CANNOT inject delegate, stop_processing,
 * phase_change, or other engine-level directives.
 *
 * @version 1.8.7
 */
class ExternalMCPClient {
public:
    /**
     * @brief Construct with name and transport.
     * @param name Server name (unique key, used as tool prefix).
     * @param transport Ownership transferred.
     * @version 1.8.7
     */
    ExternalMCPClient(std::string name,
                      std::unique_ptr<Transport> transport);

    /**
     * @brief Connect: open transport + MCP initialize + tools/list.
     * @return true on success.
     * @version 1.8.7
     */
    bool connect();

    /**
     * @brief Disconnect: close transport.
     * @version 1.8.7
     */
    void disconnect();

    /**
     * @brief List tools as JSON array string (cached).
     * @return JSON array of tool definitions with prefixed names.
     * @version 1.8.7
     */
    std::string list_tools() const;

    /**
     * @brief Execute a tool call via the external server.
     * @param tool_name Local name (without server prefix).
     * @param args_json JSON arguments string.
     * @return ServerResponse JSON envelope (directives always empty).
     * @version 1.8.7
     */
    std::string execute(const std::string& tool_name,
                        const std::string& args_json);

    /**
     * @brief Re-query tools/list and diff against cache.
     * @return Pair of (added_names, removed_names).
     * @version 1.8.7
     */
    std::pair<std::vector<std::string>, std::vector<std::string>>
    refresh_tools();

    /**
     * @brief Check connection state.
     * @return true if transport is connected.
     * @version 1.8.7
     */
    bool is_connected() const;

    /**
     * @brief Get server name.
     * @return Server name string.
     * @utility
     * @version 1.8.7
     */
    const std::string& name() const { return name_; }

private:
    std::string name_;                           ///< Server name (tool prefix)
    std::unique_ptr<Transport> transport_;        ///< Wire transport
    std::string cached_tools_json_;              ///< Cached tools/list result
    std::vector<std::string> cached_tool_names_; ///< Cached prefixed tool names
    mutable std::mutex tools_mutex_;             ///< Guards cached tool state
    int next_id_{1};                             ///< JSON-RPC request ID counter

    static constexpr uint32_t DEFAULT_TIMEOUT_MS = 30000; ///< Default tool call timeout
    static constexpr uint32_t INIT_TIMEOUT_MS = 10000;    ///< Initialize handshake timeout

    /**
     * @brief Build a JSON-RPC 2.0 request envelope.
     * @param method JSON-RPC method name.
     * @param params JSON-RPC params (as string, or empty for {}).
     * @return JSON-RPC request string.
     * @utility
     * @version 1.8.7
     */
    std::string build_request(const std::string& method,
                              const std::string& params = "{}");

    /**
     * @brief Send MCP initialize handshake.
     * @return true on success.
     * @utility
     * @version 1.8.8
     */
    bool send_initialize();

    /**
     * @brief Validate an initialize response for errors.
     * @param response Raw JSON-RPC response string.
     * @return true if response is valid and error-free.
     * @utility
     * @version 1.8.8
     */
    bool validate_init_response(const std::string& response);

    /**
     * @brief Query tools/list and update cache.
     * @return true on success.
     * @utility
     * @version 1.8.7
     */
    bool query_tools();

    /**
     * @brief Extract text content from tools/call response.
     * @param response_json Raw JSON-RPC response.
     * @return Extracted text, or error message.
     * @utility
     * @version 1.8.7
     */
    static std::string extract_tool_result(
        const std::string& response_json);

    /**
     * @brief Build ServerResponse JSON with empty directives.
     * @param result_text Result text from external server.
     * @param is_error true if the result is an error.
     * @return ServerResponse JSON envelope string.
     * @utility
     * @version 1.8.7
     */
    static std::string build_response(const std::string& result_text,
                                       bool is_error = false);
};

} // namespace entropic
