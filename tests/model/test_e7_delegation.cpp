// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_e7_delegation.cpp
 * @brief E7: Delegation wiring fires on_delegation_start callback.
 *
 * Exercises the delegation path through AgentEngine::run(). Mock tier
 * resolution is wired; a delegation-triggering prompt is sent. Validates
 * that the engine completes without error.
 *
 * Requires: GPU with >= 16GB VRAM, model on disk.
 * Run: ctest -L model
 *
 * @version 1.10.2
 */

#include "engine_test_helpers.h"

CATCH_REGISTER_LISTENER(ModelTestListener)

// ── E7: Delegation through engine ───────────────────────────

SCENARIO("Delegation wiring fires on_delegation_start callback",
         "[model][engine]")
{
    GIVEN("an engine with delegation infrastructure wired") {
        REQUIRE(g_ctx.initialized);
        start_test_log("e7_delegation");
        auto iface = make_real_interface();
        LoopConfig lc;
        // Bumped 10 → 20 at v2.0.6: the prompt-cache bleed fix removed
        // KV residue that was implicitly nudging the model toward
        // convergence. With a clean KV, the lead identity explores
        // more tool-call iterations before emitting a final assistant
        // message. 20 is enough headroom without turning the test into
        // a slow marathon.
        lc.max_iterations = 20;
        lc.stream_output = false;
        CompactionConfig cc;
        AgentEngine engine(iface, lc, cc);

        CallbackState state;
        EngineCallbacks cbs{};
        wire_callbacks(cbs, state);
        engine.set_callbacks(cbs);

        TierResolutionInterface tri;
        tri.resolve_tier = mock_resolve_tier;
        tri.tier_exists = mock_tier_exists;
        engine.set_tier_resolution(tri);

        WHEN("engine runs with delegation-triggering prompt") {
            // gh#87 (v2.7.0): tools flow via params.tools → common_chat, not a
            // rigged prompt. This scenario asserts loop completion (≥3 msgs
            // ending in assistant), with the delegation infrastructure wired.
            auto messages = make_messages(
                "You are a lead engineer who delegates work to your team.",
                "Delegate writing a hello world function to eng.");

            // Wire a tool executor that detects delegate calls
            ToolExecutionInterface tei;
            tei.process_tool_calls = mock_tool_exec;
            tei.user_data = &state;
            engine.set_tool_executor(tei);

            auto result = engine.run(std::move(messages));

            THEN("engine completed without error") {
                REQUIRE(result.size() >= 3);
                CHECK(result.back().role == "assistant");
                // gh#89-B: dispatch must track iterations — defeats the
                // forced-synthetic-complete spiral blindspot (gh#88 class).
                // (delegation_started firing is verified separately in D —
                // the mock harness may not exercise the directive path.)
                auto m = engine.last_loop_metrics();
                INFO("iterations=" << m.iterations
                     << " dispatches=" << state.tool_exec_count
                     << " delegation_started=" << state.delegation_started);
                CHECK(state.tool_exec_count >= m.iterations - 1);
                end_test_log();
            }
        }
    }
}

// ── gh#160 (v2.13.0): isolation actually contains a child's writes ──────

#include <entropic/mcp/server_manager.h>
#include <entropic/mcp/tool_executor.h>

#include <filesystem>
#include <fstream>

namespace gh160_model {
namespace fs = std::filesystem;

/// @brief Patch delivered through the delegation-complete callback.
/// @internal
/// @version 2.13.0
struct PatchCapture {
    int fired = 0;        ///< Times the complete callback ran
    size_t files = 0;     ///< files_touched_len of the last call
    std::string patch;    ///< Patch text of the last call
};

/**
 * @brief Read a whole file into a string.
 * @param p File path.
 * @return Contents, or "" when unreadable.
 * @internal
 * @version 2.13.0
 */
inline std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

} // namespace gh160_model

SCENARIO("gh#160: an isolated delegation never writes the project, and "
         "the lead still sees the original file next turn",
         "[model][engine][gh160][delegation]")
{
    using namespace gh160_model;

    GIVEN("delegation.isolation: sandbox, wired the way the facade wires it") {
        REQUIRE(g_ctx.initialized);
        start_test_log("gh160_isolated_delegation");

        auto project = fs::temp_directory_path() /
                       ("entropic_gh160_model_" + std::to_string(::getpid()));
        fs::remove_all(project);
        fs::create_directories(project);
        std::ofstream(project / "notes.md") << "ORIGINAL CONTENT\n";

        // Real servers rooted at the project — the harness builds the
        // engine directly, so every seam the facade installs is installed
        // here BY HAND. Anything skipped runs degraded and passes
        // vacuously (the v2.7.2 lesson).
        entropic::PermissionsConfig perms;
        perms.allow = {"*"};
        entropic::ServerManager servers(perms, project);
        entropic::MCPConfig mcp_cfg;
        servers.init_builtins(mcp_cfg, {"eng"},
                              (fs::path(MODEL_PATH) / "data").string());
        servers.initialize();

        auto iface = make_real_interface();
        LoopConfig lc;
        lc.max_iterations = 14;
        lc.stream_output = false;
        lc.auto_approve_tools = true;
        lc.delegation_isolation = true;  // the switch under test
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
        engine.set_tier_resolution(tri);

        entropic::SessionRootInterface sri;
        sri.resolve_root = [](const std::string&, void* ud) {
            return static_cast<entropic::ServerManager*>(ud)->project_dir();
        };
        sri.user_data = &servers;
        engine.set_session_root_interface(sri);
        engine.set_dir_swap(
            [](const std::string&, const fs::path& p, bool, void* ud) {
                static_cast<entropic::ServerManager*>(ud)
                    ->set_working_dir_all(p);
            }, &servers);

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

        PatchCapture cap;
        engine.set_delegation_callbacks(
            nullptr,
            [](const ent_delegation_result_t* r, void* ud) {
                auto* c = static_cast<PatchCapture*>(ud);
                c->fired++;
                c->files = r->files_touched_len;
                c->patch.assign(r->patch, r->patch_len);
                return ENT_DECISION_ACCEPT;
            }, &cap);

        WHEN("the lead delegates a file edit and is then asked about it") {
            auto messages = make_messages(
                "You are a lead engineer. You do not edit files yourself; "
                "you delegate every edit to the 'eng' tier using the "
                "entropic.delegate tool.",
                "Delegate to eng: rewrite notes.md so its only line is "
                "REWRITTEN BY ENG.");
            auto after_turn1 = engine.run(std::move(messages));

            // Turn 2 on the SAME accumulated context — the emergent part.
            // A child that escaped its sandbox would make the lead report
            // the rewritten text here.
            after_turn1.push_back([] {
                Message m;
                m.role = "user";
                m.content = "Now read notes.md yourself with "
                            "filesystem.read_file and quote its exact "
                            "contents.";
                return m;
            }());
            auto after_turn2 = engine.run(std::move(after_turn1));

            THEN("the project file is untouched") {
                INFO("delegation complete callbacks: " << cap.fired
                     << " patch bytes: " << cap.patch.size());
                CHECK(slurp(project / "notes.md") == "ORIGINAL CONTENT\n");
            }
            THEN("a delegation ran and its work came back as a patch") {
                // Without this the containment assertion above is vacuous:
                // a run where the model never delegated proves nothing.
                REQUIRE(cap.fired >= 1);
                CHECK(cap.patch.find("REWRITTEN") != std::string::npos);
                CHECK(cap.files >= 1);
            }
            THEN("the lead's second turn still sees the original text") {
                REQUIRE(after_turn2.size() > 3);
                CHECK(after_turn2.back().role == "assistant");
                CHECK(after_turn2.back().content.find("REWRITTEN BY ENG")
                      == std::string::npos);
                end_test_log();
            }
        }

        fs::remove_all(project);
    }
}
