// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_v2146_thinking_tool_call.cpp
 * @brief Model test — with thinking on, an agent-loop turn ends in a REAL call.
 *
 * v2.14.6, user decision. A free-form raw decode with thinking on can write a
 * Gemma 4 call with plain quotes, which the parser cannot read
 * (gh87-backend-common-chat; parse_response now keeps that text and logs
 * ERROR instead of dropping it). The guarantee that calls work with thinking
 * on lives here, in the loop. A turn that writes an unreadable call is prose,
 * so it is nudged, and the forced-grammar retry must produce a valid call that
 * actually executes. Two turns in one persistent conversation, so the second
 * runs on a history that already holds a tool call and its result.
 *
 * @version 2.14.6
 */

#include "engine_test_helpers.h"

#include <entropic/mcp/server_manager.h>
#include <entropic/mcp/tool_executor.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

/**
 * @brief The shared model with thinking pinned ON for the lead tier.
 * @version 2.14.6
 */
class ThinkingOnListener : public ModelTestListener {
public:
    using ModelTestListener::ModelTestListener;

protected:
    /**
     * @brief Pin enable_thinking on the default tier.
     * @param ctx Harness context.
     * @utility
     * @version 2.14.6
     */
    void adjust_config(ModelTestContext& ctx) override {
        auto& tier = ctx.config.models.tiers[ctx.config.models.default_tier];
        tier.enable_thinking = true;
        spdlog::info("thinking_tool_call: tier '{}' enable_thinking -> true",
                     ctx.config.models.default_tier);
    }
};

} // namespace

CATCH_REGISTER_LISTENER(ThinkingOnListener)

SCENARIO("v2.14.6: with thinking on, each loop turn ends in a real tool call",
         "[model][engine][thinking][explicit_completion][2.14.6]")
{
    namespace fs = std::filesystem;
    GIVEN("a thinking lead with explicit_completion, read_file and complete") {
        REQUIRE(g_ctx.initialized);
        const std::string log_name = "v2146_thinking_tool_call";
        start_test_log(log_name);

        auto project = fs::temp_directory_path() /
                       ("entropic_v2146_think_" + std::to_string(::getpid()));
        fs::remove_all(project);
        fs::create_directories(project);
        { std::ofstream(project / "hostname.txt") << "pinewood-depot-7\n"; }

        entropic::PermissionsConfig perms;
        perms.allow = {"*"};
        entropic::ServerManager servers(perms, project);
        entropic::MCPConfig mcp_cfg;
        servers.init_builtins(mcp_cfg, {"eng"},
                              (fs::path(MODEL_PATH) / "data").string());
        servers.initialize();
        std::string menu = tools_named(
            servers.list_tools(), {"filesystem.read_file", "entropic.complete"});
        REQUIRE(nlohmann::json::parse(menu).size() == 2);  // fixture self-check

        auto iface = make_real_interface();
        iface.get_tool_prompt = lead_tool_prompt;
        iface.tool_prompt_data = &menu;
        LoopConfig lc;
        lc.max_iterations = 8;
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
        tri.resolve_tier = mock_resolve_tier;
        tri.tier_exists = mock_tier_exists;
        tri.get_tier_param = [](const std::string& tier, const std::string& param,
                                void* /*ud*/) -> std::string {
            return (tier == g_ctx.default_tier && param == "explicit_completion")
                ? "true" : "";
        };
        engine.set_tier_resolution(tri);

        const std::unordered_map<std::string, std::vector<std::string>>
            tier_allowed{{g_ctx.default_tier,
                          {"filesystem.read_file", "entropic.complete"}}};
        entropic::ToolExecutor executor(
            servers, engine.loop_config(), engine.callbacks(),
            engine.build_directive_hooks());
        executor.set_tier_allowed_tools(&tier_allowed);
        ToolExecutionInterface tei;
        tei.process_tool_calls = [](LoopContext& ctx,
                                    const std::vector<ToolCall>& calls,
                                    void* ud) {
            return static_cast<entropic::ToolExecutor*>(ud)
                ->process_tool_calls(ctx, calls);
        };
        tei.user_data = &executor;
        engine.set_tool_executor(tei);

        WHEN("it is asked to read a file, then to read it again") {
            auto messages = make_messages(
                "You are a helpful engineer. Use filesystem.read_file to read "
                "files, then finish by calling entropic.complete with your "
                "answer as the summary.",
                "Use the read_file tool to read hostname.txt and tell me what "
                "it says.");
            auto after_turn1 = engine.run(std::move(messages));
            const int reads_turn1 =
                count_log_lines(log_name, "[EXECUTE] filesystem.read_file");

            Message again;
            again.role = "user";
            again.content = "Read hostname.txt once more and confirm it has not "
                            "changed.";
            after_turn1.push_back(again);
            auto after_turn2 = engine.run(std::move(after_turn1));

            THEN("thinking was on, and both turns executed a real read_file") {
                const int reads =
                    count_log_lines(log_name, "[EXECUTE] filesystem.read_file");
                INFO("reads turn1=" << reads_turn1 << " total=" << reads
                     << " dropped-span errors="
                     << count_log_lines(log_name, "could not be parsed and were "
                                                  "dropped")
                     << " last message=[" << after_turn2.back().content << "]");
                // Engagement first: thinking must actually be on.
                REQUIRE(test_log_contains(log_name, "thinking=true"));
                CHECK_FALSE(test_log_contains(log_name, "turn failed"));
                CHECK(reads_turn1 >= 1);
                CHECK(reads >= reads_turn1 + 1);
                CHECK_FALSE(after_turn2.back().content.empty());
                end_test_log();
            }
        }
        fs::remove_all(project);
    }
}
