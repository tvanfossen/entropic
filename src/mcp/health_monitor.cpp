// SPDX-License-Identifier: Apache-2.0
/**
 * @file health_monitor.cpp
 * @brief HealthMonitor implementation.
 * @version 1.8.7
 */

#include <entropic/mcp/health_monitor.h>
#include <entropic/types/logging.h>

static auto logger = entropic::log::get("mcp.health_monitor");

namespace entropic {

/**
 * @brief Construct with reconnection policy.
 * @param policy Backoff policy.
 * @param health_check_interval_ms Ping interval (0 = disabled).
 * @utility
 * @version 1.8.7
 */
HealthMonitor::HealthMonitor(
    ReconnectPolicy policy,
    uint32_t health_check_interval_ms)
    : policy_(std::move(policy)),
      health_check_interval_ms_(health_check_interval_ms) {}

/**
 * @brief Destructor — stops monitor if running.
 * @utility
 * @version 1.8.7
 */
HealthMonitor::~HealthMonitor() {
    stop();
}

/**
 * @brief Start monitoring a server.
 * @param name Server name.
 * @param client Non-owning pointer.
 * @req REQ-MCP-025
 * @version 1.8.7
 */
void HealthMonitor::watch(
    const std::string& name,
    ExternalMCPClient* client) {

    std::lock_guard<std::mutex> lock(watched_mutex_);
    WatchEntry entry;
    entry.client = client;
    entry.status = client->is_connected() ? "connected" : "disconnected";
    entry.reconnect_attempt = 0;
    entry.next_action = std::chrono::steady_clock::now();
    watched_[name] = entry;

    logger->info("Watching server '{}'", name);
    wake_cv_.notify_one();
}

/**
 * @brief Stop monitoring a server.
 * @param name Server name.
 * @utility
 * @version 1.8.7
 */
void HealthMonitor::unwatch(const std::string& name) {
    std::lock_guard<std::mutex> lock(watched_mutex_);
    watched_.erase(name);
    logger->info("Unwatched server '{}'", name);
}

/**
 * @brief Set callback for status change events.
 * @param cb Callback.
 * @utility
 * @version 1.8.7
 */
void HealthMonitor::set_status_callback(StatusCallback cb) {
    status_callback_ = std::move(cb);
}

/**
 * @brief Start the monitoring thread.
 * @req REQ-MCP-025
 * @version 1.8.7
 */
void HealthMonitor::start() {
    if (running_) {
        return;
    }
    running_ = true;
    monitor_thread_ = std::thread(
        &HealthMonitor::monitor_loop, this);
    logger->info("Health monitor started");
}

/**
 * @brief Stop monitoring and all reconnection attempts.
 * @utility
 * @version 1.8.7
 */
void HealthMonitor::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    wake_cv_.notify_all();
    if (monitor_thread_.joinable()) {
        monitor_thread_.join();
    }
    logger->info("Health monitor stopped");
}

/**
 * @brief Drain event queue, invoke callbacks on engine thread.
 *
 * The monitor thread only ever ENQUEUES; callbacks fire here, on the
 * engine thread, so engine state is never mutated cross-thread. The
 * queue is swapped out under the lock so a callback cannot deadlock
 * against a concurrent post_event.
 *
 * @req REQ-MCP-025
 * @version 1.8.7
 */
void HealthMonitor::process_events() {
    std::vector<HealthEvent> events;
    {
        std::lock_guard<std::mutex> lock(event_mutex_);
        events.swap(event_queue_);
    }

    for (const auto& event : events) {
        if (status_callback_) {
            status_callback_(event);
        }
    }
}

/**
 * @brief Background monitor loop.
 * @callback
 * @version 1.8.7
 */
void HealthMonitor::monitor_loop() {
    constexpr auto poll_interval = std::chrono::milliseconds(500);

    while (running_) {
        {
            std::lock_guard<std::mutex> lock(watched_mutex_);
            auto now = std::chrono::steady_clock::now();
            for (auto& [name, entry] : watched_) {
                if (now >= entry.next_action) {
                    check_server(name, entry);
                }
            }
        }

        std::unique_lock<std::mutex> lock(wake_mutex_);
        wake_cv_.wait_for(lock, poll_interval,
                          [this] { return !running_.load(); });
    }
}

/**
 * @brief Check one server and handle state transitions.
 *
 * Detects connection loss (connected → disconnected), drives the
 * reconnect attempt while down, and otherwise schedules the next poll.
 *
 * @param name Server name.
 * @param entry Watch entry, mutated in place.
 * @req REQ-MCP-025
 * @version 1.8.7
 */
void HealthMonitor::check_server(
    const std::string& name,
    WatchEntry& entry) {

    bool alive = entry.client->is_connected();

    if (entry.status == "connected" && !alive) {
        // Connection lost — start reconnecting
        auto old = entry.status;
        entry.status = "disconnected";
        entry.reconnect_attempt = 0;
        post_event(name, old, entry.status);
        logger->warn("Server '{}' disconnected", name);
        attempt_reconnect(name, entry);
        return;
    }

    if (entry.status == "disconnected" ||
        entry.status == "reconnecting") {
        attempt_reconnect(name, entry);
        return;
    }

    // Connected + healthy: schedule next check
    if (health_check_interval_ms_ > 0) {
        entry.next_action = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(health_check_interval_ms_);
    } else {
        entry.next_action = std::chrono::steady_clock::now() +
            std::chrono::seconds(5);
    }
}

/**
 * @brief Attempt reconnection for one server.
 *
 * Each failed attempt schedules the next one at the policy's
 * exponential-backoff-plus-jitter delay; once the policy reports
 * exhaustion the server is marked "error" and no longer retried.
 *
 * @param name Server name.
 * @param entry Watch entry, mutated in place.
 * @req REQ-MCP-025
 * @version 2.3.7
 */
void HealthMonitor::attempt_reconnect(
    const std::string& name,
    WatchEntry& entry) {

    if (policy_.exhausted(entry.reconnect_attempt)) {
        if (entry.status != "error") {
            auto old = entry.status;
            entry.status = "error";
            post_event(name, old, "error");
            logger->error("Server '{}' reconnection exhausted "
                          "after {} attempts",
                          name, entry.reconnect_attempt);
        }
        entry.next_action = std::chrono::steady_clock::time_point::max();
        return;
    }

    if (entry.status != "reconnecting") {
        auto old = entry.status;
        entry.status = "reconnecting";
        post_event(name, old, "reconnecting");
    }

    logger->info("Reconnecting to '{}' (attempt {})",
                 name, entry.reconnect_attempt + 1);

    if (entry.client->connect()) {
        on_reconnect_success(name, entry);
        return;
    }

    // Schedule next attempt with backoff
    auto delay = policy_.delay_ms(entry.reconnect_attempt);
    entry.reconnect_attempt++;
    entry.next_action = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(delay);

    logger->info("Server '{}' reconnect failed, "
                 "retry in {}ms", name, delay);
}

/**
 * @brief Handle a successful reconnect: refresh, mark, enqueue event.
 *
 * The tool list is re-queried on every successful reconnect, and the
 * added/removed diff rides along on the status event so the engine
 * learns about a server whose surface changed while it was down.
 *
 * @param name Server name.
 * @param entry Watch entry (mutated to connected, attempt counter
 *              reset).
 * @req REQ-MCP-025
 * @version 2.3.7
 */
void HealthMonitor::on_reconnect_success(const std::string& name,
                                         WatchEntry& entry) {
    auto [added, removed] = entry.client->refresh_tools();
    entry.status = "connected";
    entry.reconnect_attempt = 0;

    HealthEvent evt;
    evt.server_name = name;
    evt.old_status = "reconnecting";
    evt.new_status = "connected";
    evt.added_tools = std::move(added);
    evt.removed_tools = std::move(removed);
    {
        std::lock_guard<std::mutex> lock(event_mutex_);
        event_queue_.push_back(std::move(evt));
    }

    logger->info("Server '{}' reconnected", name);
    entry.next_action = std::chrono::steady_clock::now();
}

/**
 * @brief Post a status change event to the queue.
 *
 * The monitor thread's only channel to the engine — enqueue here, drain
 * in process_events() on the engine thread.
 *
 * @param name Server name.
 * @param old_status Previous status.
 * @param new_status New status.
 * @req REQ-MCP-025
 * @version 1.8.7
 */
void HealthMonitor::post_event(
    const std::string& name,
    const std::string& old_status,
    const std::string& new_status) {

    HealthEvent evt;
    evt.server_name = name;
    evt.old_status = old_status;
    evt.new_status = new_status;
    {
        std::lock_guard<std::mutex> lock(event_mutex_);
        event_queue_.push_back(std::move(evt));
    }
}

} // namespace entropic
