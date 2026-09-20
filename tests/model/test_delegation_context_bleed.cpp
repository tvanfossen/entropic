// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_delegation_context_bleed.cpp
 * @brief Regression test for v2.0.6 — delegated-agent context bleed.
 *
 * Reproduces the failure mode described in
 * `.claude/proposals/ACTIVE/v2.0.6-delegated-context-bleed.md`:
 *
 * Before the fix, `save_prefix_to_cache()` captured the full prompt's
 * KV state but labeled it as "prefix only". On the next same-tier
 * generation (same system prompt → same cache key), the restore
 * leaked residual KV entries from the prior generation, which the
 * model's attention could read as context.
 *
 * This test drives two consecutive generations through the same
 * orchestrator/tier with identical system prompts but distinct user
 * content. If the KV truncation after state restore is missing or
 * wrong, the second generation's output will echo the first's
 * distinctive content.
 *
 * Requires: GPU, model on disk, prompt cache enabled (default).
 * Run: ctest -L model -R context-bleed
 *
 * @version 2.0.6
 */

#include "model_test_context.h"

#include <algorithm>
#include <cctype>
#include <string>

CATCH_REGISTER_LISTENER(ModelTestListener)

namespace {

/**
 * @brief Lowercase a copy of the input string.
 * @param s Input string.
 * @return Lowercased copy.
 * @utility
 * @version 2.0.6
 */
std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

/**
 * @brief Check if haystack contains needle (case-insensitive substring).
 * @param haystack Text to search.
 * @param needle Substring to find.
 * @return true if needle appears in haystack (case-insensitive).
 * @utility
 * @version 2.0.6
 */
bool contains_ci(const std::string& haystack, const std::string& needle) {
    return to_lower(haystack).find(to_lower(needle)) != std::string::npos;
}

} // anonymous namespace

SCENARIO("Consecutive same-tier generations do not leak content across "
         "KV cache restores",
         "[model][regression][v2.0.6][context-bleed]")
{
    GIVEN("an orchestrator with prompt cache enabled (default)") {
        REQUIRE(g_ctx.initialized);
        start_test_log("v2.0.6_delegation_context_bleed");

        auto params = test_gen_params();
        params.max_tokens = 160;
        params.enable_thinking = false;

        // Long system prompt to ensure the cache captures meaningful
        // prefix state (trivial prompts may token-count below the
        // cache store threshold).
        const std::string sys_prompt =
            "You are a concise research assistant. Answer every "
            "question using ONLY the information provided by the "
            "user in the CURRENT message. Do not reference prior "
            "messages. Keep responses short and factual.";

        WHEN("gen A defines a distinctive fictional term, then gen B "
             "asks an unrelated question with the same system prompt")
        {
            // Gen A: distinctive content that would pollute KV if the
            // fix is missing. The fictional term is deliberately
            // unusual so that any echo in gen B indicates bleed rather
            // than general model knowledge.
            const std::string marker_a = "QUIXOTIC_FLAMINGO_47";
            auto msgs_a = make_messages(
                sys_prompt,
                "Invent a one-sentence definition for the fictional "
                "term " + marker_a + ". Use the term verbatim in "
                "your answer.");
            auto r_a = g_ctx.orchestrator->generate(
                msgs_a, params, g_ctx.default_tier);

            // Sanity check: gen A should have used the marker. If it
            // didn't, the test setup is broken and subsequent
            // assertions on gen B are meaningless.
            REQUIRE(contains_ci(r_a.content, "QUIXOTIC"));

            // Gen B: same system prompt (→ same cache key → triggers
            // restore path), deliberately unrelated topic.
            auto msgs_b = make_messages(
                sys_prompt,
                "List the first three prime numbers in ascending "
                "order. Reply with just the numbers.");
            auto r_b = g_ctx.orchestrator->generate(
                msgs_b, params, g_ctx.default_tier);

            THEN("gen B does not echo the marker token from gen A") {
                CHECK_FALSE(contains_ci(r_b.content, "QUIXOTIC"));
                CHECK_FALSE(contains_ci(r_b.content, "FLAMINGO"));
            }

            THEN("gen B produces a plausible response to its own "
                 "question (contains at least one digit)")
            {
                bool has_digit = std::any_of(
                    r_b.content.begin(), r_b.content.end(),
                    [](unsigned char c) { return std::isdigit(c); });
                CHECK(has_digit);
            }

            end_test_log();
        }
    }
}

SCENARIO("Recall-style prompts do not surface prior-generation content",
         "[model][regression][v2.0.6][context-bleed]")
{
    GIVEN("an orchestrator with prompt cache enabled") {
        REQUIRE(g_ctx.initialized);
        start_test_log("v2.0.6_context_bleed_recall");

        auto params = test_gen_params();
        params.max_tokens = 160;
        params.enable_thinking = false;

        const std::string sys_prompt =
            "You are a stateless assistant. Each request is "
            "independent. You have no memory of prior interactions.";

        WHEN("gen A mentions a distinctive topic, then gen B asks "
             "whether any prior topic exists")
        {
            auto msgs_a = make_messages(
                sys_prompt,
                "Describe the TUI terminal interface in one sentence.");
            auto r_a = g_ctx.orchestrator->generate(
                msgs_a, params, g_ctx.default_tier);
            REQUIRE(!r_a.content.empty());

            auto msgs_b = make_messages(
                sys_prompt,
                "Is there anything I have asked you previously in "
                "this message? Reply yes or no and explain briefly.");
            auto r_b = g_ctx.orchestrator->generate(
                msgs_b, params, g_ctx.default_tier);

            THEN("gen B's response does not reference TUI "
                 "(the prior-generation topic)")
            {
                CHECK_FALSE(contains_ci(r_b.content, "TUI"));
                CHECK_FALSE(contains_ci(r_b.content, "terminal "
                                                    "interface"));
            }

            end_test_log();
        }
    }
}

// ── gh#162 (v2.13.0): seeded context reaches a child that cannot search ─

#include "engine_test_helpers.h"

#include <entropic/mcp/server_manager.h>
#include <entropic/mcp/tool_executor.h>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace gh162_model {
namespace fs = std::filesystem;

/// @brief Tools the delegate tier may see — read_file and nothing else.
/// @internal
/// @version 2.13.0
struct ReaderTier {
    std::vector<std::string> tools;  ///< Tool JSON definitions for the child
};

/**
 * @brief Resolve the "reader" tier with a search-free tool set.
 *
 * The gh#162 shape: a tier that can OPEN a path but cannot FIND one. With
 * no seeded context such a child is structurally unable to succeed, so a
 * pass here cannot come from the child rediscovering the file.
 *
 * @param tier_name Requested tier.
 * @param ud ReaderTier pointer.
 * @return ChildContextInfo carrying only the read tool.
 * @callback
 * @version 2.13.0
 */
inline ChildContextInfo reader_resolve_tier(
    const std::string& tier_name, void* ud) {
    auto* r = static_cast<ReaderTier*>(ud);
    ChildContextInfo info;
    info.valid = true;
    info.system_prompt =
        "You are " + tier_name + ". You can read files with "
        "filesystem.read_file and you have NO search tools. Read the files "
        "you are given and answer from their contents.";
    info.tools = r->tools;
    return info;
}

/**
 * @brief Keep only `filesystem.read_file` from a server tool list.
 * @param tools_json Full tool list JSON array.
 * @return Single-element vector of tool JSON, or empty if absent.
 * @utility
 * @version 2.13.0
 */
inline std::vector<std::string> only_read_file(
    const std::string& tools_json) {
    std::vector<std::string> out;
    auto tools = nlohmann::json::parse(tools_json, nullptr, false);
    if (!tools.is_array()) { return out; }
    for (const auto& t : tools) {
        if (t.value("name", std::string{}) == "filesystem.read_file") {
            out.push_back(t.dump());
        }
    }
    return out;
}

} // namespace gh162_model

SCENARIO("gh#162: a delegate seeded with context answers from the named "
         "file it could never have found", "[model][engine][gh162]")
{
    using namespace gh162_model;

    GIVEN("a project full of decoys and one tier that cannot search") {
        REQUIRE(g_ctx.initialized);
        start_test_log("gh162_context_seed");

        auto project = fs::temp_directory_path() /
                       ("entropic_gh162_" + std::to_string(::getpid()));
        fs::remove_all(project);
        fs::create_directories(project / "src");
        for (int i = 0; i < 24; ++i) {
            std::ofstream(project / "src" /
                          ("decoy_" + std::to_string(i) + ".cpp"))
                << "// nothing of interest here\n";
        }
        // The answer exists in exactly one file, and its name appears
        // ONLY in the seeded context — never in the task text.
        std::ofstream(project / "src" / "FusedPoseEstimator.cpp")
            << "// latency compensation constant\n"
               "static const double kLatencyAlpha = 0.37;\n";

        entropic::PermissionsConfig perms;
        perms.allow = {"*"};
        entropic::ServerManager servers(perms, project);
        entropic::MCPConfig mcp_cfg;
        servers.init_builtins(mcp_cfg, {"reader"},
                              (fs::path(MODEL_PATH) / "data").string());
        servers.initialize();

        ReaderTier reader{only_read_file(servers.list_tools())};
        REQUIRE(reader.tools.size() == 1);

        auto iface = make_real_interface();
        LoopConfig lc;
        lc.max_iterations = 12;
        lc.stream_output = false;
        lc.auto_approve_tools = true;
        CompactionConfig cc;
        AgentEngine engine(iface, lc, cc);
        engine.set_project_dir(project);

        CallbackState state;
        EngineCallbacks cbs{};
        wire_callbacks(cbs, state);
        engine.set_callbacks(cbs);

        TierResolutionInterface tri;
        tri.resolve_tier = reader_resolve_tier;
        tri.tier_exists = mock_tier_exists;
        tri.user_data = &reader;
        engine.set_tier_resolution(tri);

        entropic::ToolExecutor executor(
            servers, engine.loop_config(), engine.callbacks(),
            engine.build_directive_hooks());
        ToolExecutionInterface tei;
        tei.process_tool_calls = [](LoopContext& ctx,
                                    const std::vector<ToolCall>& calls,
                                    void* ud) {
            return static_cast<entropic::ToolExecutor*>(ud)
                ->process_tool_calls(ctx, calls);
        };
        tei.user_data = &executor;
        engine.set_tool_executor(tei);

        WHEN("the lead delegates with a context reference, then follows up") {
            auto messages = make_messages(
                "You are a lead. Delegate analysis to the 'reader' tier "
                "with entropic.delegate, and ALWAYS pass a `context` array "
                "naming the exact file paths the reader should open — it "
                "has no search tools.",
                "Ask reader what the latency compensation constant is. The "
                "relevant file is src/FusedPoseEstimator.cpp — pass it as "
                "context.");
            auto turn1 = engine.run(std::move(messages));

            // Emergent second turn on the accumulated context: the lead
            // must still be able to answer without re-delegating blind.
            turn1.push_back([] {
                Message m;
                m.role = "user";
                m.content = "State that constant's numeric value.";
                return m;
            }());
            auto turn2 = engine.run(std::move(turn1));

            THEN("the answer carries the value only that file holds") {
                REQUIRE_FALSE(turn2.empty());
                INFO("final: " << turn2.back().content);
                CHECK(turn2.back().content.find("0.37")
                      != std::string::npos);
            }
        }

        fs::remove_all(project);
    }
}
