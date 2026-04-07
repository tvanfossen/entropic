/**
 * @file test_hook_lifecycle.cpp
 * @brief Hook lifecycle: hooks fire in correct order during generation.
 *
 * Exercises the HookInterface through AgentEngine::run() with a live
 * model. Captures informational hook events via fire_info and validates
 * ordering: ON_LOOP_START before ON_LOOP_END, with ON_STATE_CHANGE
 * and ON_LOOP_ITERATION observed between them.
 *
 * Requires: GPU with >= 16GB VRAM, model on disk.
 * Run: ctest -L model
 *
 * @version 1.10.2
 */

#include "engine_test_helpers.h"
#include <entropic/interfaces/i_hook_handler.h>

CATCH_REGISTER_LISTENER(ModelTestListener)

// ── Hook event capture ─────────────────────────────────────

/// @brief Captured hook event: point + context JSON.
/// @internal
/// @version 1.10.2
struct HookEvent {
    entropic_hook_point_t point; ///< Hook point that fired
    std::string json;            ///< Context JSON (may be empty)
};

/// @brief Captured hook events for verification.
/// @internal
/// @version 1.10.2
static std::vector<HookEvent> g_hook_events;

/**
 * @brief Capture fire_info events into g_hook_events.
 * @param registry Unused (opaque pointer).
 * @param point Hook point that fired.
 * @param context_json Context JSON string.
 * @callback
 * @version 1.10.2
 */
static void capture_fire_info(void* /*registry*/,
                              entropic_hook_point_t point,
                              const char* context_json) {
    HookEvent evt;
    evt.point = point;
    evt.json = context_json ? context_json : "";
    g_hook_events.push_back(std::move(evt));
}

/**
 * @brief No-op fire_pre that never cancels.
 * @param registry Unused.
 * @param point Unused.
 * @param context_json Unused.
 * @param out_json Set to nullptr (no modification).
 * @return 0 (proceed).
 * @callback
 * @version 1.10.2
 */
static int noop_fire_pre(void* /*registry*/,
                         entropic_hook_point_t /*point*/,
                         const char* /*context_json*/,
                         char** out_json) {
    *out_json = nullptr;
    return 0;
}

/**
 * @brief No-op fire_post that does not transform.
 * @param registry Unused.
 * @param point Unused.
 * @param context_json Unused.
 * @param out_json Set to nullptr (no transformation).
 * @callback
 * @version 1.10.2
 */
static void noop_fire_post(void* /*registry*/,
                           entropic_hook_point_t /*point*/,
                           const char* /*context_json*/,
                           char** out_json) {
    *out_json = nullptr;
}

// ── Test scenario ──────────────────────────────────────────

SCENARIO("Hooks fire in correct order during a generation cycle",
         "[model][engine][hooks]")
{
    GIVEN("an engine with HookInterface wired to capture hook events") {
        REQUIRE(g_ctx.initialized);
        start_test_log("hook_lifecycle");
        g_hook_events.clear();

        auto iface = make_real_interface();
        LoopConfig lc;
        lc.max_iterations = 3;
        lc.stream_output = false;
        CompactionConfig cc;
        AgentEngine engine(iface, lc, cc);

        HookInterface hooks;
        hooks.fire_pre = noop_fire_pre;
        hooks.fire_post = noop_fire_post;
        hooks.fire_info = capture_fire_info;
        hooks.registry = nullptr;
        engine.set_hooks(hooks);

        CallbackState state;
        EngineCallbacks cbs{};
        wire_callbacks(cbs, state);
        engine.set_callbacks(cbs);

        WHEN("engine runs a simple prompt") {
            auto messages = make_messages(
                "You are a helpful assistant.", "Say hello.");
            engine.run(std::move(messages));

            THEN("hooks fired in correct order") {
                auto has_point = [](entropic_hook_point_t p) {
                    for (const auto& e : g_hook_events) {
                        if (e.point == p) { return true; }
                    }
                    return false;
                };
                CHECK(has_point(ENTROPIC_HOOK_ON_LOOP_START));
                CHECK(has_point(ENTROPIC_HOOK_ON_LOOP_END));
                CHECK(has_point(ENTROPIC_HOOK_ON_STATE_CHANGE));
                CHECK(has_point(ENTROPIC_HOOK_ON_LOOP_ITERATION));

                // ON_LOOP_START must appear before ON_LOOP_END
                size_t start_idx = g_hook_events.size();
                size_t end_idx = 0;
                for (size_t i = 0; i < g_hook_events.size(); ++i) {
                    if (g_hook_events[i].point
                        == ENTROPIC_HOOK_ON_LOOP_START) {
                        start_idx = i;
                        break;
                    }
                }
                for (size_t i = 0; i < g_hook_events.size(); ++i) {
                    if (g_hook_events[i].point
                        == ENTROPIC_HOOK_ON_LOOP_END) {
                        end_idx = i;
                    }
                }
                CHECK(start_idx < end_idx);
                end_test_log();
            }
        }
    }
}
