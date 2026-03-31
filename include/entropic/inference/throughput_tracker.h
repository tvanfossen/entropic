/**
 * @file throughput_tracker.h
 * @brief ThroughputTracker -- real-time throughput measurement and prediction.
 *
 * @par Responsibilities:
 * - Record per-generation throughput samples (tokens, wall-clock time)
 * - Maintain exponentially weighted moving average (EWMA) of tok/s
 * - Predict time required for N tokens based on recent throughput
 * - Recommend max_tokens given a time budget
 *
 * @par Thread safety:
 * - record() acquires mutex_ (called from generation thread)
 * - tok_per_sec() and sample_count() are lock-free (read from atomics)
 * - predict_ms() and recommend_tokens() derive from lock-free tok_per_sec()
 * - One tracker per model (not per tier) -- multiple tiers sharing a model
 *   share throughput data, which is correct (same hardware, same model)
 *
 * @par Ownership:
 * Owned by ModelOrchestrator. One ThroughputTracker per loaded model
 * (keyed by model path, same dedup as model pool).
 *
 * @version 1.9.7
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

namespace entropic {

/**
 * @brief EWMA-based throughput tracker for generation budgeting.
 *
 * Single concrete class (no three-layer hierarchy). Records tok/s
 * samples from completed generations and provides smoothed estimates
 * for auto-adaptation of max_tokens.
 *
 * @version 1.9.7
 */
class ThroughputTracker {
public:
    /**
     * @brief Record a completed generation sample.
     * @param tokens_generated Number of tokens produced.
     * @param elapsed_ms Wall-clock generation time in milliseconds.
     *
     * Updates the EWMA. Ignores samples with fewer than kMinTokens
     * tokens or elapsed_ms <= 0 (degenerate generations).
     *
     * @version 1.9.7
     */
    void record(int tokens_generated, int64_t elapsed_ms);

    /**
     * @brief Current smoothed throughput estimate.
     * @return Tokens per second (EWMA). 0.0 if no samples recorded.
     * @version 1.9.7
     */
    double tok_per_sec() const;

    /**
     * @brief Predict wall-clock time for generating N tokens.
     * @param token_count Desired token count.
     * @return Predicted milliseconds. 0 if no throughput data.
     * @version 1.9.7
     */
    int64_t predict_ms(int token_count) const;

    /**
     * @brief Recommend max_tokens to fit within a time budget.
     * @param time_budget_ms Available wall-clock time.
     * @param headroom Fraction of budget to target (e.g., 0.9 = 90%).
     * @param floor Minimum token count to return (never recommend fewer).
     * @return Recommended max_tokens. Returns floor if no throughput data.
     * @version 1.9.7
     */
    int recommend_tokens(int64_t time_budget_ms,
                         float headroom = 0.9f,
                         int floor = 64) const;

    /**
     * @brief Number of recorded samples.
     * @return Sample count (lock-free read).
     * @version 1.9.7
     */
    int sample_count() const;

    /**
     * @brief Reset all throughput data.
     * @version 1.9.7
     */
    void reset();

private:
    /// @brief EWMA smoothing factor. 0.3 = 30% weight on newest sample.
    static constexpr double kAlpha = 0.3;

    /// @brief Minimum tokens for a sample to count (filters degenerate runs).
    static constexpr int kMinTokens = 4;

    std::atomic<double> ewma_tok_s_{0.0};  ///< Smoothed tok/s estimate
    std::atomic<int> samples_{0};          ///< Number of recorded samples
    std::mutex mutex_;                     ///< Guards EWMA update (write path only)
};

} // namespace entropic
