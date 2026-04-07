/**
 * @file test_multimodal_fallback.cpp
 * @brief BDD subsystem test — text-only model handles multimodal input.
 *
 * Validates that sending a message with both TEXT and IMAGE content
 * parts to a text-only model processes the text content without
 * crashing. The image part should be gracefully ignored.
 *
 * Requires: GPU with >= 16GB VRAM, model on disk.
 * Run: ctest -L model
 *
 * @version 1.10.2
 */

#include "model_test_context.h"
#include <entropic/types/content.h>

CATCH_REGISTER_LISTENER(ModelTestListener)

// ── Test: Multimodal fallback on text-only model ──────────

SCENARIO("Text-only model handles multimodal input gracefully",
         "[model][multimodal_fallback]")
{
    GIVEN("a text-only model loaded") {
        REQUIRE(g_ctx.initialized);
        start_test_log("multimodal_fallback");

        Message sys;
        sys.role = "system";
        sys.content = "You are a helpful assistant.";

        Message usr;
        usr.role = "user";
        usr.content = "What is 2+2?";

        ContentPart text_part;
        text_part.type = ContentPartType::TEXT;
        text_part.text = "What is 2+2?";

        ContentPart image_part;
        image_part.type = ContentPartType::IMAGE;
        image_part.image_path = "/nonexistent/image.png";

        usr.content_parts = {text_part, image_part};

        std::vector<Message> messages = {sys, usr};
        auto params = test_gen_params();

        WHEN("generating with multimodal input") {
            auto result = g_ctx.orchestrator->generate(
                messages, params, g_ctx.default_tier);

            THEN("model processes the text content without crashing") {
                REQUIRE_FALSE(result.content.empty());
                CHECK(result.content.find("4")
                      != std::string::npos);
                end_test_log();
            }
        }
    }
}
