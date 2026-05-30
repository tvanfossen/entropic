// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_gh90_string_typing.cpp
 * @brief gh#90 regression — gemma `<|"|>` quote-escape must preserve string
 *        typing for numeric-looking string args.
 *
 * On 2.7.1 a string param emitted via the gemma escape (e.g.
 * `grade_level:<|"|>3<|"|>`, intending the string "3") lost its typing through
 * PEG_GEMMA4 and arrived as a bare int (3) — a string-typed tool schema then
 * rejected it and the delegation circuit-broke. v2.7.2 coerces scalars back to
 * strings for string-declared params (LlamaCppBackend::parse_response →
 * coerce_string_typed_args).
 *
 * Deterministic full-backend-path test: warm a gemma render with a tool whose
 * `grade_level` is declared `string` (so the backend captures common_chat
 * params + the staged schema), then drive `parse_response` on the issue's exact
 * raw emission and assert the delivered arg is the STRING "3", not int 3.
 * (FAILS on pre-2.7.2 code — the bare int arrives.)
 *
 * Requires: GPU + `entropic download gemma4_e4b`. Run: ctest -L model.
 *
 * @version 2.7.2
 */

#include "v219_family_test_helpers.h"

namespace { constexpr char K_GEMMA4_E4B[] = "gemma4_e4b"; }
CATCH_REGISTER_LISTENER(V219FamilyListener<K_GEMMA4_E4B>)

SCENARIO("gh#90: gemma <|\"|> escape on a numeric value keeps string typing",
         "[model][gh90][gemma4]")
{
    if (!g_ctx.initialized) {
        SKIP("gemma4_e4b GGUF not present — run `entropic download gemma4_e4b`");
    }
    GIVEN("the gemma backend warmed with a string-typed param via tools") {
        start_test_log("gh90_string_typing");
        // Stage a tool whose grade_level is declared `string`, so the backend
        // captures common_chat params (PEG_GEMMA4) + retains the schema.
        auto params = test_gen_params();
        params.max_tokens = 8;
        params.tools =
            R"([{"name":"create_student","description":"Create a student.",)"
            R"("inputSchema":{"type":"object","properties":{"grade_level":)"
            R"({"type":"string"},"name":{"type":"string"}},)"
            R"("required":["grade_level","name"]}}])";
        auto warm = make_messages("You are a registrar.", "Create a student.");
        (void)g_ctx.orchestrator->generate(warm, params, g_ctx.default_tier);

        WHEN("a call emits the numeric value through the <|\"|> escape") {
            auto* llama = dynamic_cast<LlamaCppBackend*>(
                g_ctx.orchestrator->get_backend(g_ctx.default_tier));
            REQUIRE(llama != nullptr);
            REQUIRE(llama->common_chat_parse_reliable());  // PEG_GEMMA4 path
            auto parsed = llama->parse_response(
                R"(<|tool_call>call:create_student{grade_level:<|"|>3<|"|>,)"
                R"(name:<|"|>Tamsin<|"|>}<tool_call|>)");

            THEN("grade_level arrives as the STRING \"3\", not int 3 (gh#90)") {
                REQUIRE(parsed.tool_calls.size() == 1);
                const std::string& aj = parsed.tool_calls[0].arguments_json;
                INFO("arguments_json: " << aj);
                CHECK(aj.find(R"("grade_level":"3")") != std::string::npos);
                CHECK(aj.find(R"("grade_level":3)") == std::string::npos);
                CHECK(aj.find("Tamsin") != std::string::npos);
                end_test_log();
            }
        }
    }
}
