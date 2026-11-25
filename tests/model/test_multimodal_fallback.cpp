// SPDX-License-Identifier: Apache-2.0
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

namespace {

/**
 * @brief Loads the shared model WITHOUT its vision projector.
 *
 * v2.14.6: the dev config pairs the E4B lead with its mmproj (gh#240), which
 * made this "text-only" test take the vision path, where an unopenable image
 * is correctly a loud IMAGE_LOAD_FAILED. The premise is a model with no
 * projector, so it is stated here rather than inherited from the config.
 * @version 2.14.6
 */
class TextOnlyListener : public ModelTestListener {
public:
    using ModelTestListener::ModelTestListener;

protected:
    /**
     * @brief Clear every tier's mmproj so the model loads text-only.
     * @param ctx Harness context.
     * @utility
     * @version 2.14.6
     */
    void adjust_config(ModelTestContext& ctx) override {
        for (auto& [name, tier] : ctx.config.models.tiers) {
            if (!tier.mmproj_path.empty()) {
                spdlog::info("multimodal_fallback: tier '{}' mmproj '{}' "
                             "cleared (text-only premise)",
                             name, tier.mmproj_path.string());
            }
            tier.mmproj_path.clear();
        }
    }
};

} // namespace

CATCH_REGISTER_LISTENER(TextOnlyListener)

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
