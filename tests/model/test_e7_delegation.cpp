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
#include <string>
#include <unordered_map>
#include <vector>

namespace gh160_model {
namespace fs = std::filesystem;

/// @brief Patch delivered through the delegation-complete callback.
/// @internal
/// @version 2.13.0
struct PatchCapture {
    int fired = 0;        ///< Times the complete callback ran
    size_t files = 0;     ///< files_touched_len summed over every call (v2.14.6)
    std::string patch;    ///< Patch text of every call, appended (v2.14.6)
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

/**
 * @brief The last `filesystem.read_file` RESULT in a conversation.
 *
 * A tool result is EVIDENCE: the bytes the filesystem handed back, in
 * the message the executor appended. The assistant's prose around it is
 * narration, and narration is not what containment is a claim about.
 *
 * @param msgs Conversation after the turn.
 * @return The result message's content, or "" when the turn never read.
 * @utility
 * @version 2.13.0
 */
inline std::string last_read_result(const std::vector<Message>& msgs) {
    for (auto it = msgs.rbegin(); it != msgs.rend(); ++it) {
        auto name = it->metadata.find("tool_name");
        if (name != it->metadata.end()
            && name->second == "filesystem.read_file") {
            return it->content;
        }
    }
    return {};
}

/// @brief One JSON tool-definition array per tier in this scenario.
/// @internal
/// @version 2.13.0
struct TierToolsets {
    std::string lead;  ///< Lead's menu: delegate + a reader, no writer
    std::string eng;   ///< Child's menu: the writer lives here only
};

/**
 * @brief Pick named tools out of a `ServerManager::list_tools()` array.
 * @param all_json list_tools() output.
 * @param names Fully-qualified tool names to keep, in the order wanted.
 * @return A JSON ARRAY string holding exactly the named tools.
 * @utility
 * @version 2.13.0
 */
inline std::string tools_named(const std::string& all_json,
                               const std::vector<std::string>& names) {
    auto all = nlohmann::json::parse(all_json, nullptr, false);
    auto picked = nlohmann::json::array();
    if (!all.is_array()) { return picked.dump(); }
    for (const auto& want : names) {
        for (const auto& tool : all) {
            if (tool.value("name", std::string{}) == want) {
                picked.push_back(tool);
                break;
            }
        }
    }
    return picked.dump();
}

/**
 * @brief Per-tier tool staging — the seam `facade_get_tool_prompt` fills
 *        in production, and the reason gh#160 was never exercised.
 *
 * The shared harness provider (`real_get_tool_prompt`) serves ONE fixed
 * pair, filesystem.read_file + filesystem.write_file, to every tier.
 * `entropic.delegate` was therefore never on the lead's menu: the v2.13.0
 * run has the model reasoning "The tool should be entropic.delegate, but
 * ... I only have filesystem tools", then writing notes.md itself. The
 * containment CHECK failed on the lead's own write and read as an
 * isolation breach. Here the lead gets delegate plus a reader and NO
 * writer, so delegation is the only way to change the file.
 *
 * @param tier Tier being generated for ("eng" is the delegation child).
 * @param result Output: heap JSON array. Caller frees via free_fn.
 * @param ud TierToolsets pointer.
 * @return 0 — both tiers always have tools.
 * @callback
 * @version 2.13.0
 */
inline int gh160_tool_prompt(const char* tier, char** result, void* ud) {
    const auto* sets = static_cast<const TierToolsets*>(ud);
    const bool child = (tier != nullptr) && std::string(tier) == "eng";
    *result = alloc_cstr(child ? sets->eng : sets->lead);
    return 0;
}

/**
 * @brief Tier resolution for the 'eng' child: a writer, told to write.
 *
 * `mock_resolve_tier` gives every tier the bare prompt "You are <tier>.",
 * which leaves the child to guess that it is meant to edit a file. This
 * names the two calls it has and the order the filesystem server requires
 * (read before write).
 *
 * @param tier_name Target tier.
 * @param user_data Unused.
 * @return Child context info for the delegation.
 * @callback
 * @version 2.13.0
 */
inline ChildContextInfo eng_resolve_tier(const std::string& tier_name,
                                         void* /*user_data*/) {
    ChildContextInfo info;
    info.valid = true;
    info.system_prompt =
        "You are " + tier_name + ", an engineer with filesystem tools. "
        "Carry out the file change you are given: read the file with "
        "filesystem.read_file first, then write the new contents with "
        "filesystem.write_file. Paths are relative to your working "
        "directory. Then state in one sentence what you changed.";
    info.allowed_tools = {"filesystem.read_file", "filesystem.write_file"};
    return info;
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

        // Who holds the writer decides whether gh#160 is reachable at all.
        // Give the lead one and it edits notes.md itself; the sandbox is
        // then never entered and the containment CHECK fails on a write
        // that had nothing to do with isolation.
        const std::string registry = servers.list_tools();
        // v2.14.6: the lead is also STAGED entropic.complete, as the
        // production lead is (its allowlist below already held it). Without
        // it a lead that owes a completion call could never make one, and the
        // forced retry after a prose answer had nothing to conclude with.
        TierToolsets toolsets{
            tools_named(registry,
                        {"entropic.delegate", "filesystem.read_file",
                         "entropic.complete"}),
            tools_named(registry,
                        {"filesystem.read_file", "filesystem.write_file"})};
        // Fixture self-check: a renamed tool would otherwise silently stage
        // an empty menu and look like a model that declined to delegate.
        REQUIRE(nlohmann::json::parse(toolsets.lead).size() == 3);
        REQUIRE(nlohmann::json::parse(toolsets.eng).size() == 2);

        auto iface = make_real_interface();
        iface.get_tool_prompt = gh160_tool_prompt;
        iface.tool_prompt_data = &toolsets;
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
        tri.resolve_tier = eng_resolve_tier;
        tri.tier_exists = mock_tier_exists;
        // v2.14.6: the lead's PRODUCTION contract. The bundled lead identity
        // sets explicit_completion: true and the facade enforces it; this
        // engine is built directly, so without this hook the lead ran with
        // no contract and a prose answer ("I'll have eng do that") was
        // accepted as complete — the scenario then never delegated (about a
        // third of E4B runs). With it, a turn that delegates nothing is
        // nudged toward a tool call, as it is for a real consumer.
        tri.get_tier_param = [](const std::string& tier, const std::string& param,
                                void* /*ud*/) -> std::string {
            return (tier == g_ctx.default_tier && param == "explicit_completion")
                ? "true" : "";
        };
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

        // The facade's dispatch-time allowlist (gh#83), wired BY HAND for
        // the same reason every other seam above is: skip it and the lead
        // can still reach a writer it hallucinates, and the fixture's tier
        // design holds only as long as the model cooperates. Keyed on the
        // routed default tier, whose name comes from the dev box config.
        const std::unordered_map<std::string, std::vector<std::string>>
            tier_allowed{
                // entropic.complete: the production lead's own (identity_lead
                // allowed_tools) — a lead that owes a completion call must be
                // able to make one (gh#213).
                {g_ctx.default_tier,
                 {"entropic.delegate", "filesystem.read_file",
                  "entropic.complete"}},
                {"eng", {"filesystem.read_file", "filesystem.write_file"}}};

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

        PatchCapture cap;
        engine.set_delegation_callbacks(
            nullptr,
            [](const ent_delegation_result_t* r, void* ud) {
                auto* c = static_cast<PatchCapture*>(ud);
                c->fired++;
                // v2.14.6: ACCUMULATE. This kept only the LAST delegation's
                // patch, so a later no-op delegation (the lead acknowledging
                // on turn 2) erased the evidence of the edit that did run.
                c->files += r->files_touched_len;
                c->patch.append(r->patch, r->patch_len);
                return ENT_DECISION_ACCEPT;
            }, &cap);

        WHEN("the lead delegates a file edit and is then asked about it") {
            auto messages = make_messages(
                "You are a lead engineer. You have no tool that writes "
                "files; you delegate every edit to the 'eng' tier by "
                "calling entropic.delegate with target 'eng' and a task "
                "describing the edit.",
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

            // Ordered deliberately: the delegation assertion runs FIRST so
            // a run where the model declined to delegate reports THAT,
            // instead of surfacing as a containment failure it never
            // reached. v2.13.0 lost a G4 cycle to exactly that mis-read.
            THEN("a delegation ran and its work came back as a patch") {
                // Without this the containment assertion below is vacuous:
                // a run where the model never delegated proves nothing.
                REQUIRE(cap.fired >= 1);
                CHECK(cap.patch.find("REWRITTEN") != std::string::npos);
                CHECK(cap.files >= 1);
            }
            THEN("the project file is untouched") {
                INFO("delegation complete callbacks: " << cap.fired
                     << " patch bytes: " << cap.patch.size());
                REQUIRE(cap.fired >= 1);
                CHECK(slurp(project / "notes.md") == "ORIGINAL CONTENT\n");
            }
            THEN("the lead's second turn still sees the original text") {
                REQUIRE(after_turn2.size() > 3);
                CHECK(after_turn2.back().role == "assistant");
                // Asserted on the TOOL RESULT, not on the final prose.
                //
                // The prose form (`after_turn2.back().content` must not
                // contain "REWRITTEN BY ENG") was the v2.13.0 gate's one
                // failure, and it was the assertion that was wrong. The
                // lead LEGITIMATELY knows that phrase in turn 2: the
                // delegation-complete summary folds the child's report
                // into the parent's context, by design. On the failing
                // run the lead's <think> block said 'the file contains
                // "ORIGINAL CONTENT" instead of what I expected
                // ("REWRITTEN BY ENG")' and its ANSWER quoted the
                // original bytes — containment held perfectly and the
                // assertion still failed. It was constraining what the
                // model chose to say, and (with thinking on) its
                // reasoning trace as well.
                //
                // This form is STRICTLY STRONGER, on three counts:
                //   - It reads the bytes the filesystem returned, which
                //     is what "the lead still sees the original file"
                //     actually claims. A sandbox escape changes those;
                //     it cannot be talked around.
                //   - REQUIRE makes it non-vacuous. The prose assertion
                //     PASSED whenever the model declined to read at all
                //     — the exact run where it proves nothing.
                //   - It asserts the positive (ORIGINAL CONTENT present)
                //     as well as the negative, so a read that errored or
                //     came back empty fails instead of passing on an
                //     absent needle.
                const auto evidence = last_read_result(after_turn2);
                INFO("turn-2 read result: " << evidence
                     << "\nturn-2 answer: " << after_turn2.back().content);
                REQUIRE_FALSE(evidence.empty());
                CHECK(evidence.find("ORIGINAL CONTENT")
                      != std::string::npos);
                CHECK(evidence.find("REWRITTEN BY ENG")
                      == std::string::npos);
                end_test_log();
            }
        }

        fs::remove_all(project);
    }
}

// ── v2.14.6: a prose answer's forced retry may only conclude ────────────

namespace complete_only_model {

/**
 * @brief How many lines of the active test log contain @p needle?
 *
 * A completion call ends the loop and need not leave a tool-result message
 * behind, so the execution log line is the evidence that it ran.
 *
 * @param test_name The name passed to start_test_log.
 * @param needle Substring to count.
 * @return Matching line count.
 * @utility
 * @version 2.14.6
 */
inline int count_log_lines(const std::string& test_name,
                           const std::string& needle) {
    if (g_file_sink) { g_file_sink->flush(); }
    std::ifstream in((LOG_DIR / (test_name + ".log")).string());
    int n = 0;
    for (std::string line; std::getline(in, line);) {
        if (line.find(needle) != std::string::npos) { ++n; }
    }
    return n;
}

/**
 * @brief Lead tool menu for this scenario: delegate + complete.
 * @param tier Tier being generated for (unused: one menu).
 * @param result Output: heap JSON array. Caller frees via free_fn.
 * @param ud std::string pointer holding the menu.
 * @return 0.
 * @callback
 * @version 2.14.6
 */
inline int lead_tool_prompt(const char* /*tier*/, char** result, void* ud) {
    *result = alloc_cstr(*static_cast<const std::string*>(ud));
    return 0;
}

} // namespace complete_only_model

SCENARIO("v2.14.6: a lead that answers in prose is nudged, and its forced "
         "retry concludes with entropic.complete instead of delegating",
         "[model][engine][delegation][explicit_completion][2.14.6]")
{
    using namespace complete_only_model;
    using gh160_model::tools_named;
    namespace fs = std::filesystem;

    GIVEN("the production lead contract: explicit_completion, delegate + complete") {
        REQUIRE(g_ctx.initialized);
        const std::string log_name = "v2146_complete_only";
        start_test_log(log_name);

        auto project = fs::temp_directory_path() /
                       ("entropic_v2146_model_" + std::to_string(::getpid()));
        fs::remove_all(project);
        fs::create_directories(project);

        entropic::PermissionsConfig perms;
        perms.allow = {"*"};
        entropic::ServerManager servers(perms, project);
        entropic::MCPConfig mcp_cfg;
        servers.init_builtins(mcp_cfg, {"eng"},
                              (fs::path(MODEL_PATH) / "data").string());
        servers.initialize();
        std::string menu = tools_named(servers.list_tools(),
                                       {"entropic.delegate", "entropic.complete"});
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
                          {"entropic.delegate", "entropic.complete"}}};
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

        int delegations = 0;
        engine.set_delegation_callbacks(
            nullptr,
            [](const ent_delegation_result_t*, void* ud) {
                ++*static_cast<int*>(ud);
                return ENT_DECISION_ACCEPT;
            }, &delegations);

        WHEN("the lead is asked something it answers directly, twice") {
            // A greeting invites prose, which the explicit_completion contract
            // nudges; the forced retry is then offered entropic.complete only.
            auto messages = make_messages(
                "You are a friendly lead engineer.",
                "Say hello to me in one short sentence.");
            auto after_turn1 = engine.run(std::move(messages));
            const bool engaged_turn1 = test_log_contains(
                log_name, "forced retry offers only 'entropic.complete'");
            // The restriction is only real if the grammar reached the
            // sampler; plain decode dropped it before v2.14.6.
            const bool grammar_turn1 = test_log_contains(
                log_name, "Tool-call grammar applied");

            after_turn1.push_back([] {
                Message m;
                m.role = "user";
                m.content = "Thanks. Now say goodbye in one short sentence.";
                return m;
            }());
            auto after_turn2 = engine.run(std::move(after_turn1));

            THEN("the narrowed retry engaged, and both turns concluded") {
                INFO("delegations=" << delegations
                     << " turn-2 answer: " << after_turn2.back().content);
                // Engagement first: a run where the model called complete on
                // its first try never reached the code under test.
                REQUIRE(engaged_turn1);
                REQUIRE(grammar_turn1);
                CHECK_FALSE(test_log_contains(log_name, "turn failed"));
                CHECK_FALSE(test_log_contains(log_name,
                                              "but the tier is not offered it"));
                // One completion per turn, each a real tool execution.
                CHECK(count_log_lines(log_name, "[EXECUTE] entropic.complete")
                      == 2);
                CHECK(delegations == 0);
                CHECK(after_turn2.back().role == "assistant");
                end_test_log();
            }
        }

        fs::remove_all(project);
    }
}
