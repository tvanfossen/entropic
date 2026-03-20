/**
 * @file transport_sse.h
 * @brief SSE transport for external MCP servers.
 *
 * Uses cpp-httplib for HTTP communication. SSE stream read on a
 * background thread; JSON-RPC requests sent via HTTP POST.
 * Responses matched to pending requests by JSON-RPC id field.
 *
 * @version 1.8.7
 */

#pragma once

#include <entropic/mcp/transport.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace httplib { class Client; }

namespace entropic {

/**
 * @brief SSE transport: HTTP GET for event stream, POST for requests.
 *
 * Background thread reads SSE events; data fields parsed as JSON-RPC
 * responses and matched to pending requests by id. send_request()
 * POSTs the request and waits on a condition variable for the response.
 *
 * @version 1.8.7
 */
class SSETransport : public Transport {
public:
    /**
     * @brief Construct with SSE endpoint URL.
     * @param url SSE endpoint (e.g., "http://127.0.0.1:6277/sse").
     * @param default_timeout_ms Default timeout for send_request.
     * @version 1.8.7
     */
    explicit SSETransport(std::string url,
                          uint32_t default_timeout_ms = 30000);

    ~SSETransport() override;

    /**
     * @brief Connect to SSE endpoint and start reader thread.
     * @return true on success.
     * @version 1.8.7
     */
    bool open() override;

    /**
     * @brief Stop SSE reader and close HTTP client.
     * @version 1.8.7
     */
    void close() override;

    /**
     * @brief POST a JSON-RPC request and wait for matching response.
     * @param request_json JSON-RPC request string.
     * @param timeout_ms Timeout (0 = default).
     * @return Response string, or empty on error/timeout.
     * @version 1.8.7
     */
    std::string send_request(
        const std::string& request_json,
        uint32_t timeout_ms = 0) override;

    /**
     * @brief Check if SSE stream is connected.
     * @return true if connected.
     * @version 1.8.7
     */
    bool is_connected() const override;

private:
    std::string url_;                            ///< Full SSE endpoint URL
    std::string host_;                           ///< Parsed host:port
    std::string sse_path_;                       ///< Parsed SSE path
    std::string message_endpoint_;               ///< POST endpoint for requests
    uint32_t default_timeout_ms_;                ///< Default request timeout

    std::unique_ptr<httplib::Client> client_;    ///< HTTP client
    std::atomic<bool> connected_{false};         ///< Connection state
    std::atomic<bool> running_{false};           ///< Reader thread running
    std::thread sse_reader_thread_;              ///< SSE event reader

    std::mutex pending_mutex_;                   ///< Guards pending_responses_
    std::condition_variable pending_cv_;          ///< Signals response arrival
    std::map<int, std::string> pending_responses_; ///< id -> response JSON
    std::atomic<int> next_request_id_{1};        ///< JSON-RPC request ID counter

    /**
     * @brief Parse URL into host, path components.
     * @return true if URL is valid.
     * @utility
     * @version 1.8.7
     */
    bool parse_url();

    /**
     * @brief Warn if using cleartext HTTP to non-localhost host.
     * @utility
     * @version 1.8.8
     */
    void warn_if_cleartext() const;

    /**
     * @brief Parse request ID from JSON-RPC request string.
     * @param request_json JSON-RPC request.
     * @param[out] request_id Extracted ID.
     * @return true if parsed successfully.
     * @utility
     * @version 1.8.8
     */
    static bool parse_request_id(
        const std::string& request_json, int& request_id);

    /**
     * @brief POST request to message endpoint.
     * @param request_json JSON body.
     * @return true if POST succeeded with 200.
     * @utility
     * @version 1.8.8
     */
    bool post_request(const std::string& request_json);

    /**
     * @brief Wait for SSE response with matching request ID.
     * @param request_id JSON-RPC request ID to match.
     * @param timeout_ms Maximum wait time.
     * @return Response string, or empty on timeout/disconnect.
     * @utility
     * @version 1.8.8
     */
    std::string await_response(int request_id, uint32_t timeout_ms);

    /**
     * @brief SSE stream reader loop (runs on background thread).
     * @callback
     * @version 1.8.7
     */
    void sse_reader_loop();

    /**
     * @brief Process a single SSE data line (JSON-RPC response).
     * @param data SSE data field content.
     * @utility
     * @version 1.8.7
     */
    void handle_sse_data(const std::string& data);

    /**
     * @brief Extract the message endpoint from SSE endpoint event.
     * @param data SSE data containing endpoint info.
     * @utility
     * @version 1.8.7
     */
    void handle_endpoint_event(const std::string& data);

    /**
     * @brief Process a single SSE line (event: or data: prefix).
     * @param line Raw SSE line.
     * @utility
     * @version 1.8.7
     */
    void process_sse_line(const std::string& line);

    std::string current_event_type_;  ///< Current SSE event type being parsed
};

} // namespace entropic
