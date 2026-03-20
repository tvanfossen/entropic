/**
 * @file reconnect_policy.cpp
 * @brief ReconnectPolicy implementation.
 * @version 1.8.7
 */

#include <entropic/mcp/reconnect_policy.h>

#include <algorithm>
#include <cmath>
#include <random>

namespace entropic {

/**
 * @brief Construct with explicit parameters.
 * @param base_delay_ms Base delay.
 * @param max_delay_ms Maximum delay cap.
 * @param max_retries Maximum attempts (0 = infinite).
 * @param backoff_factor Exponential multiplier.
 * @internal
 * @version 1.8.7
 */
ReconnectPolicy::ReconnectPolicy(
    uint32_t base_delay_ms,
    uint32_t max_delay_ms,
    uint32_t max_retries,
    double backoff_factor)
    : base_delay_ms_(base_delay_ms),
      max_delay_ms_(max_delay_ms),
      max_retries_(max_retries),
      backoff_factor_(backoff_factor) {}

/**
 * @brief Construct from config struct.
 * @param config ReconnectConfig.
 * @internal
 * @version 1.8.7
 */
ReconnectPolicy::ReconnectPolicy(const ReconnectConfig& config)
    : base_delay_ms_(config.base_delay_ms),
      max_delay_ms_(config.max_delay_ms),
      max_retries_(config.max_retries),
      backoff_factor_(config.backoff_factor) {}

/**
 * @brief Compute delay with exponential backoff and jitter.
 * @param attempt Zero-based attempt number.
 * @return Delay in milliseconds.
 * @internal
 * @version 1.8.7
 */
uint32_t ReconnectPolicy::delay_ms(uint32_t attempt) const {
    double raw = static_cast<double>(base_delay_ms_) *
                 std::pow(backoff_factor_, static_cast<double>(attempt));
    double capped = std::min(raw, static_cast<double>(max_delay_ms_));

    // Add jitter: uniform random in [0, 10% of delay]
    thread_local std::mt19937 rng(std::random_device{}());
    double jitter_range = capped * 0.1;
    std::uniform_real_distribution<double> dist(0.0, jitter_range);

    return static_cast<uint32_t>(capped + dist(rng));
}

/**
 * @brief Check if retries are exhausted.
 * @param attempt Zero-based attempt number.
 * @return true if retries exceeded.
 * @internal
 * @version 1.8.7
 */
bool ReconnectPolicy::exhausted(uint32_t attempt) const {
    if (max_retries_ == 0) {
        return false;
    }
    return attempt >= max_retries_;
}

} // namespace entropic
