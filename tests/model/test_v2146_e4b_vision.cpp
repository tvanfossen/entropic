// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_v2146_e4b_vision.cpp
 * @brief A REAL image through Gemma 4 E4B QAT and its projector (v2.14.6).
 *
 * The only multimodal model test before this one (test_multimodal_fallback)
 * proves the opposite path: a text-only tier strips an image it cannot see.
 * Nothing ran an image through a projector. This loads the pared set's
 * workhorse, `gemma4_e4b_qat`, with `gemma4_e4b_mmproj`
 * (gemma-4-E4B-it-mmproj-F16.gguf, projection dim 2560 = the trunk's n_embd)
 * and asserts three things, each from evidence the engine produced:
 *
 *   1. The projector initialized: init_mmproj_if_configured logs
 *      "mmproj loaded from <path> — vision=true".
 *   2. The image was consumed: generate_multimodal logs
 *      "Multimodal generate: 1 images", and mtmd_prefill's
 *      "Multimodal prefill complete: n_past=N" exceeds the token count the
 *      SAME text costs without the image ("Generate: N input tokens") — the
 *      difference is the image's own tokens.
 *   3. The answer is coherent: tests/data/test_image_100x100.png is a solid
 *      RGB(128, 64, 32) square (read from its IDAT), so a model that saw it
 *      names a brown/orange hue.
 *
 * Requires: GPU + gemma4_e4b_qat + gemma4_e4b_mmproj. Run: ctest -L model -R vision
 *
 * @version 2.14.6
 */

#include <catch2/catch_test_macros.hpp>

#include "model_test_context.h"

#include <entropic/inference/orchestrator.h>
#include <entropic/types/config.h>
#include <entropic/types/content.h>
#include <entropic/types/message.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr char kTrunk[] = "gemma-4-E4B-it-qat-UD-Q4_K_XL.gguf";
constexpr char kProjector[] = "gemma-4-E4B-it-mmproj-F16.gguf";
constexpr char kQuestion[] =
    "What colour is this image? Answer with one colour word.";

/**
 * @brief A bundled GGUF under ~/.entropic/models, SKIPping only when absent.
 * @param name GGUF filename.
 * @return Its path.
 * @internal
 * @version 2.14.6
 */
fs::path gguf_or_skip(const char* name) {
    const char* home = std::getenv("HOME");
    const fs::path p = home != nullptr
        ? fs::path(home) / ".entropic" / "models" / name : fs::path{};
    if (!fs::is_regular_file(p)) {
        SKIP(std::string(name) + " not present at " + p.string());
    }
    return p;
}

/**
 * @brief One vision tier: E4B QAT trunk + its own projector, fully offloaded.
 * @param trunk Trunk GGUF.
 * @param projector mmproj GGUF.
 * @return Parsed config for ModelOrchestrator::initialize.
 * @internal
 * @version 2.14.6
 */
entropic::ParsedConfig vision_config(const fs::path& trunk,
                                     const fs::path& projector) {
    entropic::ParsedConfig config;
    config.models.default_tier = "lead";
    entropic::TierConfig lead;
    lead.path = trunk;
    lead.adapter = "gemma4";
    lead.mmproj_path = projector;
    lead.capabilities = {"text", "vision"};
    lead.context_length = 4096;
    lead.gpu_layers = -1;
    lead.flash_attn = false;
    config.models.tiers["lead"] = lead;
    return config;
}

/**
 * @brief The user turn, with or without the image part.
 * @param image Image path, or empty for the text-only control.
 * @return System + user messages.
 * @internal
 * @version 2.14.6
 */
std::vector<entropic::Message> ask(const fs::path& image) {
    entropic::Message sys;
    sys.role = "system";
    sys.content = "You are a terse assistant.";
    entropic::Message usr;
    usr.role = "user";
    usr.content = kQuestion;
    if (!image.empty()) {
        entropic::ContentPart text;
        text.type = entropic::ContentPartType::TEXT;
        text.text = kQuestion;
        entropic::ContentPart picture;
        picture.type = entropic::ContentPartType::IMAGE;
        picture.image_path = image.string();
        usr.content_parts = {text, picture};
    }
    return {sys, usr};
}

/**
 * @brief Does the answer name the image's hue, RGB(128, 64, 32)?
 * @param text Model output.
 * @return true for a brown/orange family word.
 * @internal
 * @version 2.14.6
 */
bool names_brown_hue(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    bool found = false;
    for (const char* c : {"brown", "orange", "rust", "copper", "bronze",
                          "sienna", "amber", "tan", "chocolate"}) {
        found = found || text.find(c) != std::string::npos;
    }
    return found;
}

}  // namespace

TEST_CASE("v2.14.6: a real image runs through E4B QAT and its projector",
          "[model][vision][multimodal]") {
    const fs::path trunk = gguf_or_skip(kTrunk);
    const fs::path projector = gguf_or_skip(kProjector);
    const fs::path image =
        fs::path(MODEL_PATH) / "tests" / "data" / "test_image_100x100.png";
    REQUIRE(fs::is_regular_file(image));

    entropic::GenerationParams params;
    params.max_tokens = 48;
    params.temperature = 0.0f;
    params.enable_thinking = false;

    const std::string log = "v2146_e4b_vision";
    start_test_log(log);
    entropic::ModelOrchestrator orch;
    REQUIRE(orch.initialize(vision_config(trunk, projector)));

    // Control first, so the first "Generate: N input tokens" line is its own.
    const auto control = orch.generate(ask({}), params, "lead");
    const auto seen = orch.generate(ask(image), params, "lead");

    const bool projector_up = test_log_contains(log, "mmproj loaded from")
        && test_log_contains(log, "vision=true");
    const bool image_routed =
        test_log_contains(log, "Multimodal generate: 1 images");
    const long text_tokens = test_log_int_after(log, "Generate: ");
    const long n_past =
        test_log_int_after(log, "Multimodal prefill complete: n_past=");
    end_test_log();
    orch.shutdown();

    INFO("text-only prompt " << text_tokens << " tokens; with image n_past="
         << n_past << " (image tokens ~" << (n_past - text_tokens) << ")\n"
         << "control=[" << control.content << "]\nanswer=[" << seen.content
         << "] error=" << seen.error_code << " " << seen.error_message);
    CHECK(control.error_code == 0);
    CHECK(projector_up);
    CHECK(image_routed);
    REQUIRE(text_tokens > 0);
    // The image must add real tokens, not a stripped placeholder: well past
    // the few a marker costs.
    CHECK(n_past > text_tokens + 16);
    CHECK(seen.error_code == 0);
    CHECK_FALSE(seen.content.empty());
    CHECK(names_brown_hue(seen.content));
}
