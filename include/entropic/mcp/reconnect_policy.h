/**
 * @file reconnect_policy.h
 * @brief Exponential backoff reconnection policy.
 *
 * Computes delay between reconnection attempts for external MCP
 * servers. Thread-safe. Used by HealthMonitor to schedule reconnection.
 *
 * @version 1.8.7
 */

#pragma once

#include <entropic/types/config.h>

#include <cstdint>

namespace entropic {

/**
 * @brief Exponential backoff with jitter for reconnection attempts.
 *
 * delay(n) = min(base * factor^n, max) + jitter(0, delay*0.1)
 *
 * @version 1.8.7
 */
class ReconnectPolicy {
public:
    /**
     * @brief Construct with explicit parameters.
     * @param base_delay_ms Base delay in milliseconds.
     * @param max_delay_ms Maximum delay cap.
     * @param max_retries Maximum attempts (0 = infinite).
     * @param backoff_factor Multiplicative factor per attempt.
     * @version 1.8.7
     */
    ReconnectPolicy(uint32_t base_delay_ms = 1000,
                    uint32_t max_delay_ms = 60000,
                    uint32_t max_retries = 5,
                    double backoff_factor = 2.0);

    /**
     * @brief Construct from config struct.
     * @param config ReconnectConfig with all parameters.
     * @version 1.8.7
     */
    explicit ReconnectPolicy(const ReconnectConfig& config);

    /**
     * @brief Compute delay for the given attempt number.
     * @param attempt Zero-based attempt number.
     * @return Delay in milliseconds (with jitter).
     * @version 1.8.7
     */
    uint32_t delay_ms(uint32_t attempt) const;

    /**
     * @brief Check if retries are exhausted.
     * @param attempt Zero-based attempt number.
     * @return true if max_retries > 0 and attempt >= max_retries.
     * @version 1.8.7
     */
    bool exhausted(uint32_t attempt) const;

private:
    uint32_t base_delay_ms_;    ///< Initial delay
    uint32_t max_delay_ms_;     ///< Delay ceiling
    uint32_t max_retries_;      ///< Max attempts (0 = infinite)
    double backoff_factor_;     ///< Exponential multiplier
};

} // namespace entropic
