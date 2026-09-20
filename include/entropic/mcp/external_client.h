// SPDX-License-Identifier: Apache-2.0
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

#include <atomic>
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

    /**
     * @brief Abort any pending execute() by interrupting the transport.
     *
     * Delegates to Transport::interrupt(). Safe to call concurrently
     * from the engine's interrupt thread. (P1-10, 2.0.6-rc16)
     *
     * @utility
     * @version 2.0.6-rc16
     */
    void interrupt() { if (transport_) { transport_->interrupt(); } }

    /**
     * @brief Release an interrupt so this client's transport works again.
     * @req REQ-MCP-025
     * @version 2.12.1
     */
    void clear_interrupt() { if (transport_) { transport_->clear_interrupt(); } }

    /**
     * @brief Whether this client's transport is currently interrupted.
     * @return true while calls are being short-circuited by an interrupt.
     * @req REQ-MCP-025
     * @version 2.12.1
     */
    bool is_interrupted() const {
        return transport_ && transport_->is_interrupted();
    }

private:
    std::string name_;                           ///< Server name (tool prefix)
    std::unique_ptr<Transport> transport_;        ///< Wire transport
    std::string cached_tools_json_;              ///< Cached tools/list result
    std::vector<std::string> cached_tool_names_; ///< Cached prefixed tool names
    mutable std::mutex tools_mutex_;             ///< Guards cached tool state
    /// @brief JSON-RPC request ID counter.
    ///
    /// gh#158 (v2.13.0): atomic. `build_request` does `next_id_++` and, once
    /// runs are keyed, two run threads call `execute()` on the SAME client
    /// concurrently — plus the HealthMonitor's reconnect thread, which has
    /// issued `initialize`/`tools/list` through this counter since v1.8.7.
    /// A plain `int++` from several threads is a data race, and two requests
    /// sharing an id is precisely what defeats the pairing check below.
    std::atomic<int> next_id_{1};

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
     * @brief Build a request and report the id it carries (gh#158).
     * @param method JSON-RPC method name.
     * @param params JSON-RPC params (as string, or empty for {}).
     * @param[out] id The id stamped into the request.
     * @return JSON-RPC request string.
     * @utility
     * @version 2.13.0
     */
    std::string build_request_id(const std::string& method,
                                 const std::string& params,
                                 int& id);

    /**
     * @brief Whether a response belongs to the request that asked (gh#158).
     *
     * MCP over a stdio pipe is request/response on ONE stream, and nothing
     * in this client ever checked that the line it read answered the line it
     * wrote. `StdioTransport::send_request` holds `io_mutex_` across
     * write-then-read, so bytes never interleave — but an ABANDONED request
     * (timeout, or an interrupt tripping `request_cancelled()` mid-read)
     * returns while the server's reply is still in flight. That reply then
     * sits in the pipe and is read as the answer to the NEXT request.
     *
     * Serialized runs made that a stale answer to yourself. Keyed runs make
     * it a CROSS-SESSION leak: session A's interrupt hands session B the
     * content of A's tool call, and B cannot tell. The transport drains
     * orphaned lines on the next call; this is the check that catches
     * whatever the drain missed rather than trusting it.
     *
     * A response with NO id is rejected: JSON-RPC 2.0 makes it REQUIRED and
     * MCP is JSON-RPC 2.0, so accepting one would reopen the hole for every
     * server at once. A string id spelling the same number IS accepted — the
     * one deviation common enough to be worth tolerating, and unambiguous.
     *
     * @param response_json Raw JSON-RPC response line.
     * @param expected_id The id of the request that was sent.
     * @return true when the response carries `expected_id`.
     * @utility
     * @version 2.13.0
     */
    static bool response_matches(const std::string& response_json,
                                 int expected_id);

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
    /**
     * @brief Failure envelope for an empty transport response (gh#150).
     * @param tool_name Local tool name (without server prefix).
     * @return An is_error envelope naming the actual condition.
     * @version 2.12.1
     */
    std::string empty_response_envelope(
        const std::string& tool_name) const;

    /**
     * @brief Failure envelope for a response that answered another
     *        request (gh#158).
     * @param tool_name Local tool name (without server prefix).
     * @param expected_id The id this call sent.
     * @return An is_error envelope naming the desynchronization.
     * @version 2.13.0
     */
    std::string mismatched_response_envelope(
        const std::string& tool_name, int expected_id) const;

    /**
     * @brief The failure envelope a response warrants, or empty (gh#158).
     * @param tool_name Local tool name (without server prefix).
     * @param response Raw response line from the transport.
     * @param expected_id The id the request carried.
     * @return An is_error envelope, or "" when the response is usable.
     * @version 2.13.0
     */
    std::string response_problem(const std::string& tool_name,
                                 const std::string& response,
                                 int expected_id) const;

    static std::string build_response(const std::string& result_text,
                                       bool is_error = false);
};

} // namespace entropic
