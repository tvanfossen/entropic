// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_gh88_envelope_priming.cpp
 * @brief gh#88 EMERGENT multi-turn regression — a real gemma session must
 *        not degrade as meta-tool `{"action":...}` result envelopes
 *        accumulate.
 *
 * The gh#87 single-turn parse tests all passed but missed gh#88: the bug
 * only surfaces over a MULTI-TURN loop. entropic.delegate/complete return
 * their result as `{"action":...}` JSON; pushed verbatim, the model gets
 * primed to parrot that call-shaped envelope, which PEG_GEMMA4 (native-only
 * after gh#87) cannot parse → the call no-ops and the loop spirals. This is
 * the class of failure that single-turn coverage structurally cannot catch,
 * so it is asserted here at the model level, over a real gemma loop.
 *
 * Faithful to production: the engine is served the real entropic.delegate /
 * entropic.complete tool defs (so gemma actually emits those calls), and the
 * executor echoes each meta tool's `{"action":...}` result envelope tagged
 * with the called tool name — exactly as ToolExecutor builds it.
 *
 * Asserts:
 *   1. Fix 1 (deterministic): NO raw `{"action":...}` envelope persists in
 *      the conversation — process_tool_results de-fanged every meta result
 *      before it entered context. FAILS on pre-2.7.1 code (pushed verbatim).
 *   2. Behavioral: dispatch TRACKS the iteration count. gh#88's parse-stall
 *      leaves dispatch ~1 (only the fresh first turn) while iterations climb,
 *      so tool_exec_count << iterations under the bug. NOTE: the mock sends no
 *      real completion signal, so the loop runs to its iteration budget — that
 *      is expected and is NOT the bug; the guard is that dispatch never stalls.
 *   3. Fix 2 (gemma reliable path): a parroted `{"action":...}` emission
 *      still dispatches via the common_chat-reliable parse + recovery.
 *
 * Requires: GPU + `entropic download gemma4_e4b`. Run: ctest -L model.
 *
 * @version 2.7.1
 */

#include "engine_test_helpers.h"
#include "v219_family_test_helpers.h"

#include <entropic/inference/interface_factory.h>  // gh#88: production iface

namespace { constexpr char K_GEMMA4_E4B[] = "gemma4_e4b"; }
CATCH_REGISTER_LISTENER(V219FamilyListener<K_GEMMA4_E4B>)

namespace {

/// @brief MCP tool defs for the real entropic meta-tools (delegate +
///        complete) so the model emits those calls, not filesystem ones.
/// @internal
/// @version 2.7.1
inline int gh88_meta_tool_prompt(const char* /*tier*/, char** result,
                                 void* /*user_data*/) {
    static const char* kTools =
        R"([{"name":"entropic.delegate",)"
        R"("description":"Delegate a task to another tier.",)"
        R"("inputSchema":{"type":"object","properties":{"target":)"
        R"({"type":"string"},"task":{"type":"string"}},)"
        R"("required":["target","task"]}},)"
        R"({"name":"entropic.complete",)"
        R"("description":"Signal that the task is complete.",)"
        R"("inputSchema":{"type":"object","properties":{"summary":)"
        R"({"type":"string"}},"required":["summary"]}}])";
    *result = alloc_cstr(kTools);
    return 0;
}

/// @brief Executor that echoes each meta tool's `{"action":...}` result
///        envelope tagged with the called tool name (the gh#88 priming
///        shape, exactly as ToolExecutor builds the result message).
/// @internal
/// @version 2.7.1
inline std::vector<Message> meta_envelope_exec(
    LoopContext& /*ctx*/,
    const std::vector<ToolCall>& tool_calls,
    void* user_data) {
    auto* state = static_cast<CallbackState*>(user_data);
    std::vector<Message> results;
    for (const auto& tc : tool_calls) {
        state->tool_exec_count++;
        Message msg;
        msg.role = "user";
        if (tc.name.find("complete") != std::string::npos) {
            msg.content =
                R"({"action":"complete","summary":"done","coverage_gap":false})";
        } else {
            msg.content =
                R"({"action":"delegate","target":"eng","task":"write hello"})";
        }
        msg.metadata["tool_name"] = tc.name;   // production: call.name
        msg.metadata["result_kind"] = "ok";
        results.push_back(std::move(msg));
    }
    return results;
}

/// @brief True if `content` is a raw call-shaped `{"action":"..."}` object
///        — the shape the model parrots. De-fanged results are prose.
/// @internal
/// @version 2.7.1
inline bool is_action_envelope(const std::string& content) {
    auto j = nlohmann::json::parse(content, nullptr, false);
    return j.is_object() && j.contains("action") && j["action"].is_string();
}

}  // namespace

// ── gh#88: emergent multi-turn de-fang (no envelope accumulation) ────────

SCENARIO("gh#88: a gemma multi-turn session never accumulates a parrotable "
         "{action:...} envelope",
         "[model][gh88][gemma4][emergent]")
{
    if (!g_ctx.initialized) {
        SKIP("gemma4_e4b GGUF not present — run `entropic download gemma4_e4b`");
    }
    GIVEN("a gemma engine loop whose tool results are meta {action} envelopes") {
        start_test_log("gh88_envelope_priming");
        auto iface = make_real_interface();
        iface.get_tool_prompt = gh88_meta_tool_prompt;  // serve entropic.* tools
        LoopConfig lc;
        lc.max_iterations = 8;
        lc.stream_output = false;
        CompactionConfig cc;
        AgentEngine engine(iface, lc, cc);

        CallbackState state;
        EngineCallbacks cbs{};
        wire_callbacks(cbs, state);
        engine.set_callbacks(cbs);

        TierResolutionInterface tri;
        tri.resolve_tier = mock_resolve_tier;
        tri.tier_exists = mock_tier_exists;
        engine.set_tier_resolution(tri);

        ToolExecutionInterface tei;
        tei.process_tool_calls = meta_envelope_exec;
        tei.user_data = &state;
        engine.set_tool_executor(tei);

        WHEN("the model runs a multi-turn tool-calling session") {
            auto messages = make_messages(
                "You are a lead engineer. Use the tools to delegate work to a "
                "tier, then complete when done.",
                "Delegate writing a hello-world function to eng, then finish.");
            auto result = engine.run(std::move(messages));

            THEN("no raw {action:...} envelope persists in the conversation "
                 "(Fix 1: process_tool_results de-fanged every meta result)") {
                int envelopes = 0;
                for (const auto& m : result) {
                    if (is_action_envelope(m.content)) { ++envelopes; }
                }
                INFO("conversation messages: " << result.size()
                     << ", tool dispatches: " << state.tool_exec_count);
                REQUIRE(envelopes == 0);
            }

            THEN("dispatch tracks the iteration count — gh#88's stall can't hide") {
                // gh#88 STALLS dispatch: parroted {action} envelopes fail to
                // parse, so the loop spins WITHOUT executing — tool_exec_count
                // stays ~1 (only the fresh first turn) while iterations climb to
                // the cap. With the fix every iteration's call parses + runs, so
                // dispatch keeps pace with the iteration count.
                //
                // The mock sends no completion signal, so the model keeps
                // delegating until the iteration budget — hitting the cap here
                // is EXPECTED and is NOT the bug. The guard: a regression that
                // re-introduces the parse-no-op leaves count far below iters,
                // which this assertion catches (the prior back().role=="assistant"
                // + count>=2 checks did NOT — they passed on a full spiral).
                REQUIRE_FALSE(result.empty());
                int iters = static_cast<int>(engine.last_loop_metrics().iterations);
                INFO("iterations=" << iters
                     << " dispatches=" << state.tool_exec_count);
                CHECK(iters > 0);
                CHECK(state.tool_exec_count >= iters - 1);
            }
            end_test_log();
        }
    }
}

// ── gh#88: parroted emission still dispatches (Fix 2, gemma path) ────────

SCENARIO("gh#88: a parroted {action:...} emission dispatches via the gemma "
         "reliable parse path",
         "[model][gh88][gemma4][recovery]")
{
    if (!g_ctx.initialized) {
        SKIP("gemma4_e4b GGUF not present — run `entropic download gemma4_e4b`");
    }
    GIVEN("the gemma backend warmed onto its common_chat-reliable parse path") {
        start_test_log("gh88_recovery");
        // A real generate with tools staged so the backend captures the
        // PEG_GEMMA4 params → common_chat_parse_reliable() is true.
        auto params = test_gen_params();
        params.max_tokens = 8;
        params.tools =
            R"([{"name":"entropic.delegate","description":"Delegate work.",)"
            R"("inputSchema":{"type":"object","properties":{"target":)"
            R"({"type":"string"},"task":{"type":"string"}},)"
            R"("required":["target","task"]}}])";
        auto warm = make_messages("You are a lead.", "Delegate to eng.");
        (void)g_ctx.orchestrator->generate(warm, params, g_ctx.default_tier);

        WHEN("a parroted bare-JSON {action:delegate} goes through the REAL "
             "production iface_parse_tool_calls (not the harness copy)") {
            // gh#88 audit: exercise the actual wire-site — build the production
            // orchestrator interface and call iface.parse_tool_calls, so the
            // real recovery call AND production serialize_tool_calls are
            // covered, not the harness's hand-rolled mirror.
            entropic::InterfaceContext* ictx = nullptr;
            auto iface = entropic::build_orchestrator_interface(
                g_ctx.orchestrator.get(), g_ctx.default_tier, &ictx);
            char* cleaned = nullptr;
            char* calls = nullptr;
            std::string parroted =
                R"({"action":"delegate","target":"registrar","task":"x"})";
            int rc = iface.parse_tool_calls(
                parroted.c_str(), &cleaned, &calls, iface.backend_data);

            THEN("the recovery dispatches it as an entropic.delegate call") {
                REQUIRE(rc == 0);
                std::string calls_str = calls ? calls : "[]";
                INFO("recovered tool_calls JSON: " << calls_str);
                CHECK(calls_str.find("entropic.delegate") != std::string::npos);
                iface.free_fn(cleaned);
                iface.free_fn(calls);
            }
            entropic::destroy_orchestrator_interface(ictx);
            end_test_log();
        }
    }
}
