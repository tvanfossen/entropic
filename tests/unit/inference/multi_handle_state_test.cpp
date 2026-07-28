// SPDX-License-Identifier: Apache-2.0
/**
 * @file multi_handle_state_test.cpp
 * @brief gh#134/gh#58: v2.10.4's new per-turn backend state must be
 *        per-instance, not shared across concurrent handles.
 *
 * @par Why this exists
 * v2.10.4 adds three pieces of mutable state that the orchestrator stages on
 * the backend once per turn:
 *
 *   require_tool_call_   — whether this render asks for tool_choice=REQUIRED
 *   tool_grammar_        — the tool-call GBNF the render derived
 *   tool_grammar_lazy_   — whether that grammar arms on a trigger
 *
 * sassafras-class runs multiple concurrent engine handles in one process, and
 * this codebase has FIVE recorded instances of state assumed per-instance
 * turning out to be process-global under exactly that usage (the gh#58
 * series). Reading the declarations and seeing plain members is not proof —
 * the same reading said the render's grammar reached the sampler, and it had
 * been discarded for the entire life of tool staging.
 *
 * So: assert isolation, cheaply, on CPU, with no model required.
 *
 * @par What this does NOT cover
 * `tool_grammar_` is written only inside `render_with_tools`, which needs a
 * loaded model, so its isolation is not exercised here. It is a sibling member
 * of the same object with no static storage, so it shares this one's fate —
 * but that is an inference, and a GPU multi-handle test would be needed to
 * make it a measurement.
 *
 * @version 2.10.4
 */

#include "llama_cpp_backend.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <vector>

SCENARIO("gh#134 staged tool-choice state does not leak between backends",
         "[inference][multi-handle][gh134][gh58][cpu]")
{
    GIVEN("two independent backends, as two engine handles would own") {
        entropic::LlamaCppBackend a;
        entropic::LlamaCppBackend b;

        THEN("both start unset — the default every existing consumer rides") {
            CHECK_FALSE(a.require_tool_call());
            CHECK_FALSE(b.require_tool_call());
        }

        WHEN("one handle stages REQUIRED and the other does not") {
            a.set_require_tool_call(true);
            b.set_require_tool_call(false);

            THEN("neither observes the other's staging") {
                CHECK(a.require_tool_call());
                CHECK_FALSE(b.require_tool_call());
            }
        }

        WHEN("the staging order is reversed mid-flight") {
            // Two handles interleaving turns: each re-stages before its own
            // render, so the last writer must not win globally.
            a.set_require_tool_call(true);
            b.set_require_tool_call(true);
            b.set_require_tool_call(false);

            THEN("the first handle keeps its own value") {
                CHECK(a.require_tool_call());
                CHECK_FALSE(b.require_tool_call());
            }
        }
    }

    GIVEN("many backends, as a tier pool would hold") {
        constexpr int kN = 8;
        std::vector<std::unique_ptr<entropic::LlamaCppBackend>> pool;
        for (int i = 0; i < kN; ++i) {
            pool.push_back(std::make_unique<entropic::LlamaCppBackend>());
        }

        WHEN("alternating members stage REQUIRED") {
            for (int i = 0; i < kN; ++i) {
                pool[i]->set_require_tool_call(i % 2 == 0);
            }

            THEN("each retains exactly what it was given") {
                for (int i = 0; i < kN; ++i) {
                    INFO("backend index " << i);
                    CHECK(pool[i]->require_tool_call() == (i % 2 == 0));
                }
            }
        }
    }
}
