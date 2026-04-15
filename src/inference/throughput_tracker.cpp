// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file throughput_tracker.cpp
 * @brief ThroughputTracker implementation -- EWMA throughput measurement.
 *
 * @version 1.9.7
 */

#include <entropic/inference/throughput_tracker.h>
#include <entropic/types/logging.h>

#include <algorithm>

namespace entropic {

namespace {
auto logger = entropic::log::get("inference.throughput");
} // anonymous namespace

/**
 * @brief Record a completed generation sample.
 * @param tokens_generated Number of tokens produced.
 * @param elapsed_ms Wall-clock generation time in milliseconds.
 * @internal
 * @version 2.0.0
 */
void ThroughputTracker::record(int tokens_generated, int64_t elapsed_ms) {
    if (tokens_generated < kMinTokens || elapsed_ms <= 0) {
        return;
    }

    double sample_tok_s =
        static_cast<double>(tokens_generated) / (static_cast<double>(elapsed_ms) / 1000.0);

    std::lock_guard<std::mutex> lock(mutex_);
    if (samples_.load(std::memory_order_relaxed) == 0) {
        ewma_tok_s_.store(sample_tok_s, std::memory_order_relaxed);
    } else {
        double prev = ewma_tok_s_.load(std::memory_order_relaxed);
        ewma_tok_s_.store(
            kAlpha * sample_tok_s + (1.0 - kAlpha) * prev,
            std::memory_order_relaxed);
    }
    samples_.fetch_add(1, std::memory_order_relaxed);
    logger->info("Throughput sample: {:.1f} tok/s, EWMA={:.1f} tok/s, "
                 "{} samples",
                 sample_tok_s,
                 ewma_tok_s_.load(std::memory_order_relaxed),
                 samples_.load(std::memory_order_relaxed));
}

/**
 * @brief Current smoothed throughput estimate.
 * @return Tokens per second (EWMA). 0.0 if no samples recorded.
 * @internal
 * @version 1.9.7
 */
double ThroughputTracker::tok_per_sec() const {
    return ewma_tok_s_.load(std::memory_order_relaxed);
}

/**
 * @brief Predict wall-clock time for generating N tokens.
 * @param token_count Desired token count.
 * @return Predicted milliseconds. 0 if no throughput data.
 * @internal
 * @version 1.9.7
 */
int64_t ThroughputTracker::predict_ms(int token_count) const {
    double tps = tok_per_sec();
    if (tps <= 0.0) {
        return 0;
    }
    return static_cast<int64_t>(
        (static_cast<double>(token_count) / tps) * 1000.0);
}

/**
 * @brief Recommend max_tokens to fit within a time budget.
 * @param time_budget_ms Available wall-clock time.
 * @param headroom Fraction of budget to target.
 * @param floor Minimum token count to return.
 * @return Recommended max_tokens.
 * @internal
 * @version 1.9.7
 */
int ThroughputTracker::recommend_tokens(
    int64_t time_budget_ms, float headroom, int floor) const
{
    double tps = tok_per_sec();
    if (tps <= 0.0) {
        return floor;
    }

    double budget_sec = static_cast<double>(time_budget_ms) / 1000.0;
    int recommended = static_cast<int>(tps * budget_sec * headroom);
    return std::max(recommended, floor);
}

/**
 * @brief Number of recorded samples.
 * @return Sample count.
 * @internal
 * @version 1.9.7
 */
int ThroughputTracker::sample_count() const {
    return samples_.load(std::memory_order_relaxed);
}

/**
 * @brief Reset all throughput data.
 * @internal
 * @version 1.9.7
 */
void ThroughputTracker::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    ewma_tok_s_.store(0.0, std::memory_order_relaxed);
    samples_.store(0, std::memory_order_relaxed);
}

} // namespace entropic
