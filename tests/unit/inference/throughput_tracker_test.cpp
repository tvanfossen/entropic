// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_throughput_tracker.cpp
 * @brief Tests for ThroughputTracker -- EWMA throughput measurement.
 *
 * Tests cover: first sample, EWMA smoothing, degenerate sample filtering,
 * predict_ms, recommend_tokens with headroom and floor, reset, sustained
 * throughput change adaptation, thread safety.
 *
 * @version 1.9.7
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <entropic/inference/throughput_tracker.h>

#include <thread>

using entropic::ThroughputTracker;
using Catch::Matchers::WithinRel;

// ── First sample ────────────────────────────────────────────

SCENARIO("First sample sets initial EWMA", "[throughput_tracker]") {
    ThroughputTracker tracker;

    GIVEN("an empty ThroughputTracker") {
        REQUIRE(tracker.sample_count() == 0);
        REQUIRE(tracker.tok_per_sec() == 0.0);

        WHEN("record(100, 2000) is called") {
            tracker.record(100, 2000);

            THEN("EWMA is set to the sample value") {
                REQUIRE(tracker.sample_count() == 1);
                REQUIRE_THAT(tracker.tok_per_sec(),
                             WithinRel(50.0, 0.001));
            }
        }
    }
}

// ── EWMA smoothing ─────────────────────────────────────────

SCENARIO("Subsequent samples smooth via EWMA", "[throughput_tracker]") {
    ThroughputTracker tracker;
    tracker.record(100, 2000);  // 50 tok/s

    WHEN("a second sample at 20 tok/s is recorded") {
        tracker.record(100, 5000);  // 20 tok/s

        THEN("EWMA blends old and new") {
            // 0.3 * 20 + 0.7 * 50 = 41.0
            REQUIRE(tracker.sample_count() == 2);
            REQUIRE_THAT(tracker.tok_per_sec(),
                         WithinRel(41.0, 0.001));
        }
    }
}

// ── Degenerate sample filtering ─────────────────────────────

SCENARIO("Degenerate samples are ignored", "[throughput_tracker]") {
    ThroughputTracker tracker;
    tracker.record(100, 2000);  // 50 tok/s

    WHEN("record with 0 tokens is called") {
        tracker.record(0, 1000);

        THEN("sample count unchanged") {
            REQUIRE(tracker.sample_count() == 1);
            REQUIRE_THAT(tracker.tok_per_sec(),
                         WithinRel(50.0, 0.001));
        }
    }

    WHEN("record with 3 tokens is called (below kMinTokens=4)") {
        tracker.record(3, 1000);

        THEN("sample count unchanged") {
            REQUIRE(tracker.sample_count() == 1);
        }
    }
}

SCENARIO("Zero-ms samples are ignored", "[throughput_tracker]") {
    ThroughputTracker tracker;
    tracker.record(100, 2000);  // 50 tok/s

    WHEN("record with 0 ms is called") {
        tracker.record(100, 0);

        THEN("sample count unchanged") {
            REQUIRE(tracker.sample_count() == 1);
        }
    }

    WHEN("record with negative ms is called") {
        tracker.record(100, -500);

        THEN("sample count unchanged") {
            REQUIRE(tracker.sample_count() == 1);
        }
    }
}

// ── predict_ms ──────────────────────────────────────────────

SCENARIO("predict_ms estimates generation time", "[throughput_tracker]") {
    ThroughputTracker tracker;
    tracker.record(80, 2000);  // 40 tok/s

    WHEN("predict_ms(200) is called") {
        int64_t ms = tracker.predict_ms(200);

        THEN("returns 200/40 * 1000 = 5000 ms") {
            REQUIRE(ms == 5000);
        }
    }
}

SCENARIO("predict_ms returns 0 with no data", "[throughput_tracker]") {
    ThroughputTracker tracker;

    WHEN("predict_ms is called on empty tracker") {
        int64_t ms = tracker.predict_ms(200);

        THEN("returns 0") {
            REQUIRE(ms == 0);
        }
    }
}

// ── recommend_tokens ────────────────────────────────────────

SCENARIO("recommend_tokens with headroom", "[throughput_tracker]") {
    ThroughputTracker tracker;
    tracker.record(40, 2000);  // 20 tok/s

    WHEN("recommend_tokens(10000, 0.9) is called") {
        int tokens = tracker.recommend_tokens(10000, 0.9f);

        THEN("returns approximately 180 (float truncation may yield 179)") {
            REQUIRE(tokens >= 179);
            REQUIRE(tokens <= 180);
        }
    }
}

SCENARIO("recommend_tokens respects floor", "[throughput_tracker]") {
    ThroughputTracker tracker;
    // 1 tok/s: record 4 tokens in 4000ms
    tracker.record(4, 4000);

    WHEN("recommend_tokens(1000, 0.9, floor=64) is called") {
        int tokens = tracker.recommend_tokens(1000, 0.9f, 64);

        THEN("returns floor because computed value < floor") {
            // 1 * 1 * 0.9 = 0.9 -> int(0) < 64
            REQUIRE(tokens == 64);
        }
    }
}

SCENARIO("recommend_tokens returns floor with no data",
         "[throughput_tracker]") {
    ThroughputTracker tracker;

    WHEN("recommend_tokens is called on empty tracker") {
        int tokens = tracker.recommend_tokens(10000, 0.9f, 64);

        THEN("returns floor") {
            REQUIRE(tokens == 64);
        }
    }
}

// ── reset ───────────────────────────────────────────────────

SCENARIO("reset clears all data", "[throughput_tracker]") {
    ThroughputTracker tracker;
    tracker.record(100, 2000);
    tracker.record(100, 2000);
    tracker.record(100, 2000);
    REQUIRE(tracker.sample_count() == 3);

    WHEN("reset() is called") {
        tracker.reset();

        THEN("all data is cleared") {
            REQUIRE(tracker.sample_count() == 0);
            REQUIRE(tracker.tok_per_sec() == 0.0);
        }
    }
}

// ── Sustained throughput change ─────────────────────────────

SCENARIO("EWMA adapts to sustained throughput change",
         "[throughput_tracker]") {
    ThroughputTracker tracker;

    GIVEN("10 samples at 30 tok/s") {
        for (int i = 0; i < 10; ++i) {
            tracker.record(60, 2000);  // 30 tok/s
        }
        REQUIRE_THAT(tracker.tok_per_sec(), WithinRel(30.0, 0.05));

        WHEN("5 samples at 15 tok/s are recorded") {
            for (int i = 0; i < 5; ++i) {
                tracker.record(30, 2000);  // 15 tok/s
            }

            THEN("EWMA has shifted toward 15") {
                // After 5 samples at new rate with alpha=0.3:
                // Convergence: each sample moves 30% toward 15
                double tps = tracker.tok_per_sec();
                REQUIRE(tps < 25.0);  // shifted significantly
                REQUIRE(tps > 15.0);  // not fully converged
            }
        }
    }
}

// ── Thread safety ───────────────────────────────────────────

SCENARIO("Concurrent record and read are thread-safe",
         "[throughput_tracker]") {
    ThroughputTracker tracker;
    constexpr int kIterations = 500;

    WHEN("threads record and read concurrently") {
        std::thread writer([&] {
            for (int i = 0; i < kIterations; ++i) {
                tracker.record(100, 2000);
            }
        });

        std::thread reader([&] {
            for (int i = 0; i < kIterations; ++i) {
                double tps = tracker.tok_per_sec();
                (void)tps;
                int count = tracker.sample_count();
                (void)count;
            }
        });

        writer.join();
        reader.join();

        THEN("no crash and sample count is correct") {
            REQUIRE(tracker.sample_count() == kIterations);
        }
    }
}
