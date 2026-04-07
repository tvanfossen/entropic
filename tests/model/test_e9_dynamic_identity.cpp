/**
 * @file test_e9_dynamic_identity.cpp
 * @brief E9: Dynamic identity creation and generation through engine loop.
 *
 * Creates a dynamic identity via IdentityManager with allow_dynamic=true,
 * builds a system prompt from it, and runs the engine loop. Validates
 * that the model responds and the engine reaches PLANNING and COMPLETE.
 *
 * Requires: GPU with >= 16GB VRAM, model on disk.
 * Run: ctest -L model
 *
 * @version 1.10.2
 */

#include "engine_test_helpers.h"
#include <entropic/core/identity_manager.h>

CATCH_REGISTER_LISTENER(ModelTestListener)

// ── E9: Dynamic identity through engine loop ───────────────

SCENARIO("Dynamic identity creation and generation through engine loop",
         "[model][engine][dynamic_identity]")
{
    GIVEN("an engine with a dynamically created identity") {
        REQUIRE(g_ctx.initialized);
        start_test_log("e9_dynamic_identity");

        IdentityManagerConfig mgr_cfg;
        mgr_cfg.allow_dynamic = true;
        mgr_cfg.max_identities = 64;
        IdentityManager mgr(mgr_cfg);

        IdentityConfig dyn;
        dyn.name = "test-assistant";
        dyn.system_prompt = "You are a test assistant named TestBot. "
            "Always introduce yourself as TestBot when asked about "
            "your role.";
        dyn.focus = {"testing"};
        dyn.origin = IdentityOrigin::DYNAMIC;
        auto err = mgr.create(dyn);
        REQUIRE(err == ENTROPIC_OK);

        const auto* identity = mgr.get("test-assistant");
        REQUIRE(identity != nullptr);

        auto iface = make_real_interface();
        LoopConfig lc;
        lc.max_iterations = 3;
        lc.stream_output = false;
        CompactionConfig cc;
        AgentEngine engine(iface, lc, cc);

        CallbackState state;
        EngineCallbacks cbs{};
        wire_callbacks(cbs, state);
        engine.set_callbacks(cbs);

        WHEN("engine runs with the dynamic identity system prompt") {
            auto messages = make_messages(
                identity->system_prompt, "What is your role?");
            auto result = engine.run(std::move(messages));

            THEN("model responds using the dynamic identity") {
                REQUIRE(result.size() >= 3);
                CHECK_FALSE(result.back().content.empty());
                CHECK(result.back().role == "assistant");

                auto has = [&](AgentState s) {
                    int v = static_cast<int>(s);
                    return std::find(state.states.begin(),
                        state.states.end(), v) != state.states.end();
                };
                CHECK(has(AgentState::PLANNING));
                CHECK(has(AgentState::COMPLETE));
                end_test_log();
            }
        }
    }
}
