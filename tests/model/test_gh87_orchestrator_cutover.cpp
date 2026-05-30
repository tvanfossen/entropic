// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_gh87_orchestrator_cutover.cpp
 * @brief gh#87 Phase B: validate the live cutover at the orchestrator level.
 *
 * Phase A proved common_chat parses each family at the BACKEND level. This
 * proves the live wiring one layer up: a tool def supplied via
 * `params.tools` (NOT system-message injection) flows through
 * `ModelOrchestrator::generate` →  `stage_active_tools` →
 * `render_with_tools` → common_chat → `apply_adapter_parse`
 * (`has_common_chat_params()` → `parse_response`) and lands in
 * `result.tool_calls`. This is the exact path the agent loop drives in
 * production. SKIPs if the gemma4_a4b GGUF is absent.
 *
 * @version 2.7.0
 */

#include "v219_family_test_helpers.h"

#include <entropic/types/tool_call.h>

namespace { constexpr char K_GEMMA4_A4B[] = "gemma4_a4b"; }
CATCH_REGISTER_LISTENER(V219FamilyListener<K_GEMMA4_A4B>)

SCENARIO("gh#87 cutover: params.tools drives common_chat through the orchestrator",
         "[model][gh87][cutover]")
{
    if (!g_ctx.initialized) {
        SKIP("gemma4_a4b GGUF not present — run `entropic download gemma4_a4b`");
    }
    GIVEN("a read_file tool supplied via params.tools (no system-msg injection)") {
        start_test_log("gh87_orchestrator_cutover");
        auto params = test_gen_params();
        params.max_tokens = 512;
        params.tools =
            R"([{"name":"read_file","description":"Read a file from disk.",)"
            R"("inputSchema":{"type":"object","properties":{"path":)"
            R"({"type":"string"}},"required":["path"]}}])";

        auto messages = make_messages(
            "You are a helpful assistant.",
            "You must call the read_file tool to read the file /etc/hostname. "
            "Emit only the tool call.");

        WHEN("the orchestrator generates with tools staged") {
            auto result = g_ctx.orchestrator->generate(
                messages, params, g_ctx.default_tier);

            THEN("the orchestrator extracts the native tool call via common_chat") {
                INFO("raw=[" << result.raw_content << "] content=["
                     << result.content << "] calls=" << result.tool_calls.size());
                REQUIRE_FALSE(result.tool_calls.empty());
                CHECK(result.tool_calls[0].name.find("read_file")
                      != std::string::npos);
                end_test_log();
            }
        }
    }
}
