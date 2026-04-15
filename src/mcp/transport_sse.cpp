// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file transport_sse.cpp
 * @brief SSETransport implementation.
 * @version 1.8.7
 */

#include <entropic/mcp/transport_sse.h>
#include <entropic/types/logging.h>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <sstream>

static auto logger = entropic::log::get("mcp.transport.sse");

namespace entropic {

/**
 * @brief Construct with SSE endpoint URL.
 * @param url SSE endpoint URL.
 * @param default_timeout_ms Default request timeout.
 * @internal
 * @version 1.8.7
 */
SSETransport::SSETransport(
    std::string url,
    uint32_t default_timeout_ms)
    : url_(std::move(url)),
      default_timeout_ms_(default_timeout_ms) {}

/**
 * @brief Destructor — ensures reader thread is stopped.
 * @internal
 * @version 1.8.7
 */
SSETransport::~SSETransport() {
    close();
}

/**
 * @brief Parse URL into host and path components.
 * @return true if valid.
 * @utility
 * @version 1.8.7
 */
bool SSETransport::parse_url() {
    // Expected: http[s]://host[:port]/path
    auto scheme_end = url_.find("://");
    if (scheme_end == std::string::npos) {
        return false;
    }

    auto host_start = scheme_end + 3;
    auto path_start = url_.find('/', host_start);
    if (path_start == std::string::npos) {
        host_ = url_.substr(0, host_start) +
                url_.substr(host_start);
        sse_path_ = "/";
    } else {
        host_ = url_.substr(0, path_start);
        sse_path_ = url_.substr(path_start);
    }
    return true;
}

/**
 * @brief Warn if using cleartext HTTP to non-localhost host.
 * @utility
 * @version 1.8.8
 */
void SSETransport::warn_if_cleartext() const {
    bool is_http = url_.substr(0, 5) == "http:";
    bool is_localhost = host_.find("localhost") != std::string::npos ||
                        host_.find("127.0.0.1") != std::string::npos ||
                        host_.find("[::1]") != std::string::npos;
    if (is_http && !is_localhost) {
        logger->warn("SSE connection to {} uses HTTP (cleartext). "
                     "Tool call data will be unencrypted.", host_);
    }
}

/**
 * @brief Connect to SSE endpoint and start reader thread.
 * @return true on success.
 * @internal
 * @version 1.8.8
 */
bool SSETransport::open() {
    if (connected_) {
        return true;
    }

    if (!parse_url()) {
        logger->error("Invalid SSE URL: {}", url_);
        return false;
    }

    warn_if_cleartext();

    client_ = std::make_unique<httplib::Client>(host_);
    client_->set_read_timeout(std::chrono::seconds(0));
    client_->set_connection_timeout(std::chrono::seconds(10));

    running_ = true;
    sse_reader_thread_ = std::thread(
        &SSETransport::sse_reader_loop, this);

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    bool ok = connected_.load();
    if (!ok) {
        logger->error("SSE connection to {} failed", url_);
        close();
    } else {
        logger->info("SSE transport connected to {}", url_);
    }
    return ok;
}

/**
 * @brief Stop SSE reader and close HTTP client.
 * @internal
 * @version 1.8.7
 */
void SSETransport::close() {
    running_ = false;
    connected_ = false;

    if (client_) {
        client_->stop();
    }

    // Wake any waiting send_request callers
    pending_cv_.notify_all();

    if (sse_reader_thread_.joinable()) {
        sse_reader_thread_.join();
    }

    client_.reset();
    logger->info("SSE transport closed for {}", url_);
}

/**
 * @brief Parse request ID from JSON-RPC request string.
 * @param request_json JSON-RPC request.
 * @param[out] request_id Extracted ID.
 * @return true if parsed successfully.
 * @utility
 * @version 1.8.8
 */
bool SSETransport::parse_request_id(
    const std::string& request_json, int& request_id) {

    try {
        auto req = nlohmann::json::parse(request_json);
        request_id = req.value("id", 0);
        return true;
    } catch (...) {
        return false;
    }
}

/**
 * @brief POST request to message endpoint.
 * @param request_json JSON body.
 * @return true if POST succeeded with 200.
 * @utility
 * @version 1.8.8
 */
bool SSETransport::post_request(const std::string& request_json) {
    auto result = client_->Post(
        message_endpoint_, request_json, "application/json");

    if (!result || result->status != 200) {
        logger->error("POST to {} failed: {}",
                      message_endpoint_,
                      result ? std::to_string(result->status)
                             : "no response");
        return false;
    }
    return true;
}

/**
 * @brief Wait for SSE response with matching request ID.
 * @param request_id JSON-RPC request ID to match.
 * @param timeout_ms Maximum wait time.
 * @return Response string, or empty on timeout/disconnect.
 * @utility
 * @version 1.8.8
 */
std::string SSETransport::await_response(
    int request_id, uint32_t timeout_ms) {

    std::unique_lock<std::mutex> lock(pending_mutex_);
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);

    while (pending_responses_.find(request_id) ==
           pending_responses_.end()) {
        if (pending_cv_.wait_until(lock, deadline) ==
            std::cv_status::timeout) {
            logger->warn("SSE response timeout for id={}",
                         request_id);
            return "";
        }
        if (!connected_) {
            return "";
        }
    }

    std::string response = std::move(
        pending_responses_[request_id]);
    pending_responses_.erase(request_id);
    return response;
}

/**
 * @brief POST a JSON-RPC request and wait for matching response.
 * @param request_json JSON-RPC request string.
 * @param timeout_ms Timeout (0 = default).
 * @return Response string, or empty on error/timeout.
 * @internal
 * @version 2.0.0
 */
std::string SSETransport::send_request(
    const std::string& request_json,
    uint32_t timeout_ms) {

    logger->info("SSE request: {} bytes", request_json.size());
    int request_id = 0;
    bool ready = connected_ && !message_endpoint_.empty() &&
                 parse_request_id(request_json, request_id) &&
                 post_request(request_json);

    if (!ready) {
        return "";
    }

    uint32_t actual_timeout = timeout_ms > 0
        ? timeout_ms : default_timeout_ms_;
    return await_response(request_id, actual_timeout);
}

/**
 * @brief Check if SSE stream is connected.
 * @return true if connected.
 * @internal
 * @version 1.8.7
 */
bool SSETransport::is_connected() const {
    return connected_;
}

/**
 * @brief SSE stream reader loop.
 * @callback
 * @version 1.8.7
 */
void SSETransport::sse_reader_loop() {
    auto content_receiver = [this](
        const char* data, size_t len) -> bool {
        std::string chunk(data, len);
        // Parse SSE events from chunk
        std::istringstream stream(chunk);
        std::string line;
        while (std::getline(stream, line)) {
            if (line.empty() || line == "\r") {
                continue;
            }
            process_sse_line(line);
        }
        return running_.load();
    };

    auto res = client_->Get(
        sse_path_,
        httplib::Headers{{"Accept", "text/event-stream"}},
        content_receiver);

    connected_ = false;
    if (running_) {
        logger->warn("SSE stream disconnected from {}", url_);
    }
}

/**
 * @brief Process a single SSE line.
 * @param line Raw SSE line.
 * @utility
 * @version 1.8.7
 */
void SSETransport::process_sse_line(const std::string& line) {
    // Strip trailing \r
    std::string clean = line;
    if (!clean.empty() && clean.back() == '\r') {
        clean.pop_back();
    }

    if (clean.substr(0, 6) == "data: ") {
        handle_sse_data(clean.substr(6));
    } else if (clean.substr(0, 7) == "event: ") {
        current_event_type_ = clean.substr(7);
    }
}

/**
 * @brief Process SSE data field as JSON-RPC response.
 * @param data SSE data content.
 * @utility
 * @version 1.8.7
 */
void SSETransport::handle_sse_data(const std::string& data) {
    if (current_event_type_ == "endpoint") {
        handle_endpoint_event(data);
        current_event_type_.clear();
        return;
    }
    current_event_type_.clear();

    try {
        auto j = nlohmann::json::parse(data);
        int id = j.value("id", 0);
        if (id > 0) {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_responses_[id] = data;
            pending_cv_.notify_all();
        }
    } catch (const nlohmann::json::exception& e) {
        logger->warn("Failed to parse SSE data: {}", e.what());
    }
}

/**
 * @brief Extract POST endpoint from SSE endpoint event.
 * @param data Endpoint URL or path.
 * @utility
 * @version 1.8.7
 */
void SSETransport::handle_endpoint_event(const std::string& data) {
    if (data.empty()) {
        return;
    }

    // Could be absolute URL or relative path
    if (data[0] == '/') {
        message_endpoint_ = data;
    } else if (data.find("://") != std::string::npos) {
        // Extract path from absolute URL
        auto path_start = data.find('/', data.find("://") + 3);
        message_endpoint_ = (path_start != std::string::npos)
            ? data.substr(path_start) : "/message";
    } else {
        message_endpoint_ = "/" + data;
    }

    connected_ = true;
    logger->info("SSE message endpoint: {}", message_endpoint_);
}

} // namespace entropic
