// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_gh105_validator_clobbers.cpp
 * @brief gh#105 — the consumer's KILLER, RED-first. With constitutional
 *        validation ON, the validator's toolless critique render clobbers the
 *        backend's GLOBAL captured PEG parser (render_prompt sets
 *        have_chat_params_=false) BEFORE the engine RE-parses the MAIN
 *        generation's output (engine.cpp:529 generate → :530 dispatch_post_generate
 *        [validator] → :531 process_generation_result → :543 parse_tool_calls).
 *        The re-parse routes through common_chat_parse_reliable()/parse_response
 *        (interface_factory.cpp:329 ; harness mirror real_parse_tool_calls:630),
 *        which now reads cleared state → a syntactically-perfect gemma tool call
 *        extracts as ZERO calls → the engine injects "no tool call, retry" → the
 *        turn spirals. Reproduced by the consumer 3/3 with validation on.
 *
 * Why no existing test caught it: the gh#103 gemma test calls
 * orchestrator->generate() directly (no validator interleave); the
 * constitutional test calls validator.validate() directly (no engine re-parse).
 * Only the full AgentEngine::run loop with validation ON exercises the
 * gen → POST_GENERATE(clobber) → re-parse sequence.
 *
 * RED on v2.8.2 (tool_exec_count == 0). GREEN once render params are captured
 * PER-CALL (parse_captured_, written only by tooled renders, survives the
 * validator's interleaved toolless render).
 *
 * gemma4_e4b specifically: common_chat_parse_reliable() is true ONLY for
 * PEG_GEMMA4, so the clobber drops the reliable-parse route that gemma depends
 * on (qwen/nemotron use the adapter path, which the clobber does not touch).
 *
 * Requires: GPU + gemma4_e4b GGUF. Run: ctest -L model -R gh105
 * @version 2.8.3
 */

#include "engine_test_helpers.h"
#include "v219_family_test_helpers.h"

#include <entropic/core/constitutional_validator.h>
#include <entropic/core/hook_registry.h>

namespace { constexpr char K_GEMMA4_E4B[] = "gemma4_e4b"; }
CATCH_REGISTER_LISTENER(V219FamilyListener<K_GEMMA4_E4B>)

SCENARIO("gh#105: constitutional validator's toolless render must NOT clobber "
         "the main generation's tool-call extraction (engine loop, gemma4)",
         "[model][gh105]")
{
    if (!g_ctx.initialized) {
        SKIP("gemma4_e4b GGUF not present — run `entropic download gemma4_e4b`");
    }
    GIVEN("an engine with tools staged + constitutional validation ENABLED") {
        start_test_log("gh105_validator_clobbers");
        auto iface = make_real_interface();
        LoopConfig lc;
        lc.max_iterations = 4;
        lc.auto_approve_tools = true;
        lc.stream_output = false;   // batch path
        CompactionConfig cc;
        AgentEngine engine(iface, lc, cc);

        CallbackState state;
        EngineCallbacks cbs{};
        wire_callbacks(cbs, state);
        engine.set_callbacks(cbs);

        ToolExecutionInterface tei;
        tei.process_tool_calls = mock_tool_exec;   // increments tool_exec_count
        tei.user_data = &state;
        engine.set_tool_executor(tei);

        // The interleave that clobbers: a real validator on the engine's hooks.
        auto constitution = load_constitution_prompt();
        REQUIRE_FALSE(constitution.empty());
        ConstitutionalValidationConfig vcfg;
        vcfg.enabled = true;
        vcfg.max_revisions = 1;
        vcfg.skip_tiers.clear();   // gh#89-D: default {"lead"} bypasses the hook
        ConstitutionalValidator validator(vcfg, constitution);
        // Wire a real HookRegistry + dispatch lambdas so the validator's
        // POST_GENERATE hook actually fires in the engine loop (mirrors the
        // facade, src/facade/entropic.cpp:1103-1118). An empty HookInterface
        // has registry==nullptr → attach() fails → validator never fires.
        HookRegistry hook_registry;
        HookInterface hooks{};
        hooks.registry = &hook_registry;
        hooks.fire_pre = [](void* reg, entropic_hook_point_t pt,
                            const char* json, char** out) -> int {
            return static_cast<HookRegistry*>(reg)->fire_pre(pt, json, out);
        };
        hooks.fire_post = [](void* reg, entropic_hook_point_t pt,
                             const char* json, char** out) {
            static_cast<HookRegistry*>(reg)->fire_post(pt, json, out);
        };
        hooks.fire_info = [](void* reg, entropic_hook_point_t pt,
                             const char* json) {
            static_cast<HookRegistry*>(reg)->fire_info(pt, json);
        };
        REQUIRE(validator.attach(&hooks, &iface) == ENTROPIC_OK);
        engine.set_hooks(hooks);

        WHEN("the engine runs a tool-demanding request (validator interleaves)") {
            // Tools flow via make_real_interface's get_tool_prompt → params.tools
            // → common_chat (gemma native format). No prompt rigging.
            auto messages = make_messages(
                "You are a helpful assistant with filesystem tools.",
                "Read the file test.txt with the read_file tool and tell me "
                "what it says.");
            auto result = engine.run(std::move(messages));
            auto m = engine.last_loop_metrics();

            THEN("the main tool call still extracts + dispatches despite the "
                 "validator's interleaved toolless render") {
                INFO("tool_exec_count=" << state.tool_exec_count
                     << " iterations=" << m.iterations
                     << " msgs=" << result.size());
                // THE KILLER — RED on v2.8.2 (== 0): the validator's toolless
                // critique render clears the captured PEG parser before the
                // engine re-parses the main output, so the gemma call extracts
                // as zero. Per-call capture keeps the main call's parser intact.
                CHECK(state.tool_exec_count >= 1);
                REQUIRE(result.size() >= 3);
                CHECK(result.back().role == "assistant");
                // gh#105 NON-VACUITY GUARD — prove the validator's toolless
                // critique render actually FIRED, so the clobber path engaged.
                // validate() short-circuits to "Validation skipped" (no render,
                // no clobber) when is_pure_tool_call() recognizes the output. It
                // does NOT today (gemma's <|tool_call>/<tool_call|> markup isn't
                // the <tool_call>...</tool_call> wrapper the stripper matches),
                // so critique fires and the test is RED on v2.8.2. But that is
                // load-bearing on an external heuristic: if gemma's format ever
                // shifts, validate() would skip, no clobber, and tool_exec_count
                // >= 1 would pass on BOTH broken and fixed code. "Validation
                // start" (constitutional_validator.cpp) logs iff validate() did
                // NOT short-circuit -> with inference wired, the toolless render
                // ran -> the clobber path was exercised.
                CHECK(test_log_contains("gh105_validator_clobbers",
                                        "Validation start"));
                validator.detach(&hooks);
                end_test_log();
            }
        }
    }
}
