// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file health_monitor.h
 * @brief Monitors external MCP server health and triggers reconnection.
 *
 * Runs a background thread that detects connection loss, schedules
 * reconnection with exponential backoff, optionally pings for health,
 * and refreshes tool lists after successful reconnection.
 *
 * Thread model: status changes are posted to a thread-safe event queue.
 * The engine's main thread drains via process_events(). No cross-thread
 * mutation of engine state.
 *
 * @version 1.8.7
 */

#pragma once

#include <entropic/mcp/external_client.h>
#include <entropic/mcp/reconnect_policy.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace entropic {

/**
 * @brief Status change event posted by the monitor thread.
 * @version 1.8.7
 */
struct HealthEvent {
    std::string server_name;   ///< Which server changed
    std::string old_status;    ///< Previous status
    std::string new_status;    ///< New status
    std::vector<std::string> added_tools;   ///< Tools added on refresh
    std::vector<std::string> removed_tools; ///< Tools removed on refresh
};

/**
 * @brief Monitors external server connections and handles reconnection.
 *
 * The monitor thread checks connection state periodically and attempts
 * reconnection using ReconnectPolicy. Events are queued for the engine
 * thread to process (no direct cross-thread mutation).
 *
 * @version 1.8.7
 */
class HealthMonitor {
public:
    /**
     * @brief Callback invoked on engine thread when processing events.
     * @version 1.8.7
     */
    using StatusCallback = std::function<void(const HealthEvent&)>;

    /**
     * @brief Construct with reconnection policy.
     * @param policy Backoff policy for reconnection attempts.
     * @param health_check_interval_ms Ping interval (0 = disabled).
     * @version 1.8.7
     */
    HealthMonitor(ReconnectPolicy policy,
                  uint32_t health_check_interval_ms = 0);

    ~HealthMonitor();

    /**
     * @brief Start monitoring a server.
     * @param name Server name.
     * @param client Non-owning pointer to ExternalMCPClient.
     * @version 1.8.7
     */
    void watch(const std::string& name, ExternalMCPClient* client);

    /**
     * @brief Stop monitoring a server.
     * @param name Server name.
     * @version 1.8.7
     */
    void unwatch(const std::string& name);

    /**
     * @brief Set callback for status change events.
     * @param cb Callback invoked by process_events().
     * @version 1.8.7
     */
    void set_status_callback(StatusCallback cb);

    /**
     * @brief Start the monitoring thread.
     * @version 1.8.7
     */
    void start();

    /**
     * @brief Stop monitoring and all reconnection attempts.
     * @version 1.8.7
     */
    void stop();

    /**
     * @brief Drain event queue, invoke callbacks (call on engine thread).
     * @version 1.8.7
     */
    void process_events();

private:
    ReconnectPolicy policy_;                     ///< Backoff policy
    uint32_t health_check_interval_ms_;          ///< Ping interval
    StatusCallback status_callback_;             ///< Engine-thread callback

    /**
     * @brief Per-server monitoring state.
     * @version 1.8.7
     */
    struct WatchEntry {
        ExternalMCPClient* client;               ///< Non-owning
        std::string status;                      ///< Current status
        uint32_t reconnect_attempt;              ///< Attempt counter
        std::chrono::steady_clock::time_point next_action; ///< Next check/reconnect time
    };

    std::map<std::string, WatchEntry> watched_;  ///< Monitored servers
    std::mutex watched_mutex_;                   ///< Guards watched_

    std::vector<HealthEvent> event_queue_;        ///< Pending events
    std::mutex event_mutex_;                     ///< Guards event_queue_

    std::thread monitor_thread_;                 ///< Background thread
    std::atomic<bool> running_{false};           ///< Thread control
    std::condition_variable wake_cv_;            ///< Wake for immediate check
    std::mutex wake_mutex_;                      ///< Guards wake_cv_

    /**
     * @brief Monitor loop (background thread).
     * @callback
     * @version 1.8.7
     */
    void monitor_loop();

    /**
     * @brief Check one server and handle state transitions.
     * @param name Server name.
     * @param entry Watch entry (mutated).
     * @utility
     * @version 1.8.7
     */
    void check_server(const std::string& name, WatchEntry& entry);

    /**
     * @brief Attempt reconnection for a single server.
     * @param name Server name.
     * @param entry Watch entry (mutated on success/failure).
     * @utility
     * @version 1.8.7
     */
    void attempt_reconnect(const std::string& name,
                           WatchEntry& entry);

    /**
     * @brief Post a status change event to the queue.
     * @param name Server name.
     * @param old_status Previous status.
     * @param new_status New status.
     * @utility
     * @version 1.8.7
     */
    void post_event(const std::string& name,
                    const std::string& old_status,
                    const std::string& new_status);
};

} // namespace entropic
