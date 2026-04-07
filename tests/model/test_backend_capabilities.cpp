/**
 * @file test_backend_capabilities.cpp
 * @brief Backend capabilities: BackendInfo correctly populated for loaded model.
 *
 * Exercises the BackendInfo and capability query API through the live
 * orchestrator. Validates that the loaded Qwen3.5 model reports correct
 * metadata: backend name, compute device, model format, architecture,
 * parameter count, context length, and supported capabilities.
 *
 * Requires: GPU with >= 16GB VRAM, model on disk.
 * Run: ctest -L model
 *
 * @version 1.10.2
 */

#include "model_test_context.h"
#include <entropic/types/backend_capability.h>

CATCH_REGISTER_LISTENER(ModelTestListener)

// ── Backend capability test ────────────────────────────────

SCENARIO("BackendInfo is correctly populated for the loaded model",
         "[model][backend_capabilities]")
{
    GIVEN("a model loaded via the orchestrator") {
        REQUIRE(g_ctx.initialized);
        start_test_log("backend_capabilities");
        auto* backend = g_ctx.orchestrator->get_backend(
            g_ctx.default_tier);
        REQUIRE(backend != nullptr);
        auto bi = backend->info();

        WHEN("querying backend capabilities") {
            auto caps = backend->capabilities();

            THEN("backend reports correct information") {
                CHECK(bi.name == "llama.cpp");
                CHECK_FALSE(bi.compute_device.empty());
                CHECK(bi.model_format == "gguf");
                CHECK_FALSE(bi.architecture.empty());
                CHECK(bi.parameter_count > 0);
                CHECK(bi.max_context_length > 0);
                CHECK(backend->supports(BackendCapability::STREAMING));
                CHECK(backend->supports(BackendCapability::GRAMMAR));
                end_test_log();
            }
        }
    }
}
