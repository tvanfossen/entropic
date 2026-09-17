// SPDX-License-Identifier: Apache-2.0
/**
 * @file model_skip_reason_test.cpp
 * @brief gh#149 (v2.13.0): a model test's SKIP must name the rule that fired.
 *
 * The defect this pins: `init_orchestrator_for_v219_family` returns a BOOL for
 * five different causes, and every SCENARIO then emitted one hardcoded literal
 * — "<key> GGUF not present — run `entropic download <key>`". When the cause
 * was the v2.12.0 operator waiver or a host-RAM shortfall the model WAS on
 * disk, so the release record stated something nothing had verified and
 * pointed the reader at an action that changes nothing.
 *
 * Lives beside partial_offload_test.cpp because it asserts the REPORTING side
 * of the same v2.12.0 policy that file asserts the DECISION side of
 * (large_model_tests_waived / host_can_hold_warm_load). Pure string mapping,
 * so it needs no GGUF, no GPU and no orchestrator — the reason texts are
 * exactly the part of gh#149 that is CPU-testable.
 *
 * @version 2.13.0
 */

#include "../../model/model_skip_reason.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using entropic::test::SkipCause;
using entropic::test::SkipFacts;
using entropic::test::skip_reason_text;

namespace {

/// @brief Case-sensitive substring test, spelled once.
bool has(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

/// @brief The 12.6 GB Qwen3.6-35B-A3B that every one of these causes fired on.
constexpr uint64_t kQwen36Bytes = 13211155424ull;

/// @brief Facts for a model that IS on disk.
SkipFacts present_facts() {
    SkipFacts f;
    f.key = "qwen3_6_a3b";
    f.path = "/home/dev/.entropic/models/Qwen3.6-35B-A3B-UD-IQ3_XXS.gguf";
    f.file_bytes = kQwen36Bytes;
    f.available_bytes = 8ull * 1024 * 1024 * 1024;
    f.needed_bytes = kQwen36Bytes + (2ull * 1024 * 1024 * 1024);
    return f;
}

}  // namespace

SCENARIO("gh#149: a skip whose model is present never tells you to download it",
         "[model_skip_reason][gh149][2.13.0]") {
    GIVEN("the GGUF that motivated gh#149, present on disk") {
        const auto f = present_facts();

        WHEN("the operator waived large-model tests") {
            const auto msg =
                skip_reason_text(SkipCause::kLargeModelWaived, f);

            THEN("it names the allowance, not a missing file") {
                INFO("reason: " << msg);
                CHECK(has(msg, "ENTROPIC_SKIP_LARGE_MODEL_TESTS"));
                CHECK_FALSE(has(msg, "download"));
                CHECK_FALSE(has(msg, "not present"));
            }

            THEN("it says the model is on disk, so the reader stops looking") {
                INFO("reason: " << msg);
                CHECK(has(msg, f.path));
            }
        }

        WHEN("the host cannot hold the WARM load") {
            const auto msg =
                skip_reason_text(SkipCause::kHostRamInsufficient, f);

            THEN("it is framed as a hardware limit, not a defect") {
                INFO("reason: " << msg);
                CHECK(has(msg, "not a defect"));
                CHECK_FALSE(has(msg, "download"));
            }

            THEN("it carries the measured numbers that decided it") {
                INFO("reason: " << msg);
                // 13211155424 / 1MiB = 12599; available 8 GiB = 8192 MiB.
                CHECK(has(msg, "12599"));
                CHECK(has(msg, "8192"));
            }
        }

        WHEN("the orchestrator failed to load a GGUF that is present") {
            const auto msg =
                skip_reason_text(SkipCause::kOrchestratorInitFailed, f);

            THEN("it is reported as a load failure, not a missing download") {
                INFO("reason: " << msg);
                CHECK(has(msg, "load"));
                CHECK_FALSE(has(msg, "download"));
            }
        }
    }
}

SCENARIO("gh#149: causes that are NOT about the file say so",
         "[model_skip_reason][gh149][2.13.0]") {
    GIVEN("a registry key that resolves to nothing") {
        SkipFacts f;
        f.key = "typo_key";

        WHEN("the reason is built") {
            const auto msg =
                skip_reason_text(SkipCause::kRegistryKeyMissing, f);

            THEN("it points at the registry, not at a download") {
                INFO("reason: " << msg);
                CHECK(has(msg, "bundled_models.yaml"));
                CHECK(has(msg, "typo_key"));
                CHECK_FALSE(has(msg, "download"));
            }
        }
    }

    GIVEN("harness setup that failed before any model was reached") {
        SkipFacts f;
        f.key = "gemma4_e4b";
        f.detail = "Config load failed: unknown tier 'lead'";

        WHEN("the reason is built") {
            const auto msg =
                skip_reason_text(SkipCause::kHarnessSetupFailed, f);

            THEN("it names the setup failure and carries the loader's error") {
                INFO("reason: " << msg);
                CHECK(has(msg, "unknown tier 'lead'"));
                CHECK_FALSE(has(msg, "download"));
            }
        }
    }
}

SCENARIO("gh#149: the genuinely-absent case keeps its actionable text",
         "[model_skip_reason][gh149][2.13.0]") {
    GIVEN("a key whose GGUF is not on disk") {
        SkipFacts f;
        f.key = "gemma4_e4b";
        f.path = "/home/dev/.entropic/models/gemma-4-E4B-it.gguf";

        WHEN("the reason is built") {
            const auto msg = skip_reason_text(SkipCause::kGgufMissing, f);

            THEN("it still tells the developer exactly what to run") {
                INFO("reason: " << msg);
                CHECK(has(msg, "entropic download gemma4_e4b"));
                CHECK(has(msg, f.path));
            }
        }
    }
}

SCENARIO("gh#149: no cause yields no reason, so the caller's default stands",
         "[model_skip_reason][gh149][2.13.0]") {
    GIVEN("a context whose setup succeeded") {
        WHEN("a reason is requested anyway") {
            const auto msg = skip_reason_text(SkipCause::kNone, SkipFacts{});

            THEN("it is empty — the accessor falls back, nothing regresses") {
                CHECK(msg.empty());
            }
        }
    }
}
