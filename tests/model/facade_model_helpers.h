// SPDX-License-Identifier: Apache-2.0
/**
 * @file facade_model_helpers.h
 * @brief gh#93 (v2.8.0) facade-driven model-test harness.
 *
 * Builds a temp project (config.local.yaml + identity_<tier>.md + GBNF files)
 * and configures a real handle through the public C-ABI
 * (entropic_create -> entropic_configure_dir -> grammar registration). Unlike
 * `model_test_context.h` (which builds the orchestrator DIRECTLY, bypassing
 * `configure_common`), this exercises the production configure path — the seam
 * that hid gh#88/90/94. RAII: destroys the handle + removes the dir on scope
 * exit.
 *
 * Also owns the run helpers (v2.13.0) — see the "transcript vs answer"
 * note below for why a model test must almost never assert positively on
 * what `entropic_run*` returns directly.
 *
 * @version 2.13.0
 */
#pragma once

#include <entropic/entropic.h>

// The ONE answer extractor. src/facade is on the model-test include path
// (tests/model/CMakeLists.txt) for exactly this, mirroring what
// tests/unit/api/final_text_test.cpp already does — a second extractor
// written here could drift from the rule the bridge actually applies.
#include "final_text.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace entropic::test::facade {

namespace fs = std::filesystem;

/// @brief Write a file, creating parent dirs. @utility @version 2.8.0
inline void write_file(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << content;
}

/// @brief Path to a bundled GGUF under ~/.entropic/models. @utility @version 2.8.0
inline fs::path model_gguf(const std::string& filename) {
    const char* home = std::getenv("HOME");
    return home ? fs::path(home) / ".entropic" / "models" / filename : fs::path{};
}

/**
 * @brief One tier in a facade test project.
 * @version 2.8.0
 */
struct TierSpec {
    std::string name;                 ///< Tier name (e.g. "npc")
    std::string gguf_key;             ///< bundled_models key (e.g. "gemma4_e2b")
    std::string adapter;              ///< Adapter (e.g. "gemma4")
    std::string identity_body =       ///< Identity prose (system prompt)
        "You are a helpful assistant.";
    std::string grammar_name;         ///< Grammar key ("" = none)
    std::string grammar_gbnf;         ///< GBNF content (when grammar_name set)
    int context_length = 8192;
    int gpu_layers = 99;
    int n_parallel = 1;
    bool enable_thinking = false;     ///< Identity enable_thinking frontmatter

    /// @brief Identity `allowed_tools` frontmatter (gh#121). Empty = all.
    ///
    /// A configured handle stages EVERY registered tool by default: 27 of
    /// them, 18.6 KB, ~5000 prompt tokens (see the gh#158 run log). A tier
    /// with a 2-8 K context therefore overflows on the tool block alone,
    /// before the model has seen the task — llama.cpp logs "Decode chunk
    /// failed" and the turn comes back empty. A scenario that needs two
    /// filesystem calls names them here and pays for two.
    /// @version 2.13.0
    std::vector<std::string> allowed_tools;

    /// @brief Identity `explicit_completion` frontmatter. nullopt = derive.
    ///
    /// `populate_tier_info` (src/facade/entropic.cpp) derives this as
    /// `frontmatter.explicit_completion.value_or(!tier.auto_chain
    /// .has_value())`, so a FacadeProject tier — which writes neither —
    /// came out TRUE, and every turn owed a closing tool call. For a
    /// scenario that is about conversation state rather than about tool
    /// use, that contract is pure interference: the engine answers a
    /// perfectly good reply with "[SYSTEM] Your previous response
    /// contained no tool call ... Retry.", three times, and the LAST
    /// assistant message — the one `final_answer` reads — is whatever the
    /// model said while arguing with the nudge, not its answer. See the
    /// v2.13.0 gh#165 and gh#158 gate logs.
    ///
    /// Set it to false when the scenario needs no completion contract.
    /// @version 2.13.0
    std::optional<bool> explicit_completion;

    /// @brief Identity `max_iterations` frontmatter (E6). -1 = absent.
    ///
    /// The per-tier loop cap. It is the ONLY way a facade test can make a
    /// DELEGATION CHILD hit the iteration cap: `LoopConfig::max_iterations`
    /// is engine-wide, and a child loop reads its override through
    /// `AgentEngine::run_loop` -> `apply_identity_overrides` ->
    /// `get_tier_param(tier, "max_iterations")`, which the facade answers
    /// from this frontmatter field (`populate_tier_info`,
    /// src/facade/entropic.cpp). Note that `entropic.delegate`'s own
    /// `max_turns` argument does NOT cap the child — it only reaches the
    /// storage record — so a scenario that needs a capped child must set
    /// this.
    /// @version 2.13.0
    int max_iterations = -1;

    /// @brief Identity `max_consecutive_empty_turns` frontmatter (gh#123).
    ///
    /// -1 = absent, which leaves the engine default of 3. A tier that owes
    /// an explicit completion it cannot emit is nudged
    /// ("[SYSTEM] Your previous response contained no tool call ... Retry.")
    /// and, on the fourth such turn, FAILS the loop into `AgentState::ERROR`
    /// — a terminal state, so neither the iteration cap nor the
    /// thinking-budget cut fires after it. A scenario whose subject IS one
    /// of those terminals raises the allowance so the ladder cannot reach
    /// the run first.
    /// @version 2.13.0
    int max_consecutive_empty_turns = -1;
};

/**
 * @brief Human-readable reason a `setup()` returned nullptr.
 *
 * `REQUIRE(h != nullptr)` on its own prints `nullptr != nullptr`, which
 * says nothing about WHICH of the five configure failures happened. The
 * one that actually bites on the floor hardware (a 1080 Ti, 11 GB) is the
 * VRAM admission gate, so it is named explicitly.
 *
 * @param rc Error code from the failing C-ABI call.
 * @param step Which call failed.
 * @param tiers Tier specs, echoed so the model and its sizing are visible.
 * @return One line suitable for INFO() ahead of the REQUIRE.
 * @utility
 * @version 2.13.0
 */
inline std::string setup_failure_text(entropic_error_t rc,
                                      const std::string& step,
                                      const std::vector<TierSpec>& tiers) {
    std::string why = step + " failed, rc="
                    + std::to_string(static_cast<int>(rc));
    if (rc == ENTROPIC_ERROR_TIER_MODEL_TOO_LARGE) {
        why += " (TIER_MODEL_TOO_LARGE — the model did NOT FIT the VRAM "
               "budget; the [residency] log lines carry the footprint and "
               "the budget. Pick a smaller GGUF or wait for the previous "
               "model test's VRAM to return)";
    }
    for (const auto& t : tiers) {
        why += "; tier " + t.name + " model=" + t.gguf_key
             + " ctx=" + std::to_string(t.context_length)
             + " gpu_layers=" + std::to_string(t.gpu_layers);
    }
    return why;
}

/**
 * @brief RAII temp project configured through the real C-ABI (gh#93).
 * @version 2.8.0
 */
class FacadeProject {
public:
    explicit FacadeProject(const std::string& tag)
        : dir_(fs::temp_directory_path() / ("entropic_" + tag)) {
        fs::remove_all(dir_);
        fs::create_directories(dir_);
    }
    ~FacadeProject() {
        if (handle_ != nullptr) { entropic_destroy(handle_); }
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    FacadeProject(const FacadeProject&) = delete;
    FacadeProject& operator=(const FacadeProject&) = delete;

    const fs::path& dir() const { return dir_; }
    entropic_handle_t handle() const { return handle_; }

    /// @brief `permissions.auto_approve` for the generated config.
    ///
    /// A FacadeProject wrote no `permissions:` block at all, so
    /// `PermissionsConfig::auto_approve` stayed at its `false` default and
    /// `lc.auto_approve_tools` with it. `ToolExecutor::check_approval` then
    /// needs an explicit allow-pattern or an `on_tool_call` callback, and a
    /// facade test wires neither — so EVERY tool call from EVERY facade
    /// model test was denied at dispatch:
    ///
    ///     [mcp.tool_executor] No approval callback — denying:
    ///       entropic.complete
    ///
    /// That is not a production configuration; it is a project nobody
    /// ships. `examples/explorer` and `examples/pychess` both set
    /// `auto_approve: true`, and the direct-engine model harness sets
    /// `lc.auto_approve_tools = true` by hand. Defaulting it here puts the
    /// facade harness on the same footing — a tool a test stages is a tool
    /// the test can actually call. Set it false to exercise denial.
    /// @version 2.13.0
    bool auto_approve_tools = true;

    /// @brief Extra top-level YAML appended verbatim to config.local.yaml.
    ///
    /// For a scenario that needs a section the generator does not write —
    /// e.g. `mcp:` with a fixed `working_dir` and an outside-root policy
    /// (v2.13.0). Must be complete top-level keys; "" writes nothing.
    /// @version 2.13.0
    std::string extra_config;

    /// @brief Why the last `setup()` returned nullptr ("" when it did not).
    /// @version 2.13.0
    const std::string& setup_failure() const { return setup_failure_; }

    /**
     * @brief Build the project files, create + configure a handle, register
     *        per-tier grammars.
     * @param tiers Tier specs (first is the default unless overridden).
     * @param default_tier Default tier name ("" = first).
     * @return Configured handle, or nullptr on failure (see setup_failure()).
     */
    entropic_handle_t setup(const std::vector<TierSpec>& tiers,
                            const std::string& default_tier = "") {
        write_project_files(tiers, default_tier);
        setenv("ENTROPIC_DATA_DIR",
               (fs::path(MODEL_PATH) / "data").string().c_str(), 1);
        setup_failure_.clear();
        auto rc = entropic_create(&handle_);
        if (rc != ENTROPIC_OK) {
            setup_failure_ = setup_failure_text(rc, "entropic_create", tiers);
            return nullptr;
        }
        rc = entropic_configure_dir(handle_, dir_.string().c_str());
        if (rc != ENTROPIC_OK) {
            setup_failure_ =
                setup_failure_text(rc, "entropic_configure_dir", tiers);
            return nullptr;
        }
        for (const auto& t : tiers) {
            if (!t.grammar_name.empty()) {
                entropic_grammar_register_file(
                    handle_, t.grammar_name.c_str(),
                    (dir_ / (t.grammar_name + ".gbnf")).string().c_str());
            }
        }
        return handle_;
    }

private:
    void write_project_files(const std::vector<TierSpec>& tiers,
                             const std::string& default_tier) {
        std::string cfg = "models:\n";
        for (const auto& t : tiers) {
            write_tier_identity(t);
            if (!t.grammar_name.empty()) {
                write_file(dir_ / (t.grammar_name + ".gbnf"), t.grammar_gbnf);
            }
            cfg += tier_config_block(t);
        }
        cfg += "  default: "
             + (default_tier.empty() ? tiers.front().name : default_tier) + "\n";
        cfg += "constitutional_validation:\n  enabled: false\n";
        cfg += std::string("permissions:\n  auto_approve: ")
             + (auto_approve_tools ? "true" : "false") + "\n";
        cfg += extra_config;
        write_file(dir_ / "config.local.yaml", cfg);
    }

    void write_tier_identity(const TierSpec& t) {
        // `focus:` is required by the identity loader (>= one entry).
        std::string fm = "---\ntype: identity\nversion: 1\nname: " + t.name
                       + "\nfocus:\n  - act in character\n"
                       + "enable_thinking: "
                       + (t.enable_thinking ? "true" : "false") + "\n";
        if (!t.grammar_name.empty()) { fm += "grammar: " + t.grammar_name + "\n"; }
        if (t.explicit_completion.has_value()) {
            fm += std::string("explicit_completion: ")
                + (*t.explicit_completion ? "true" : "false") + "\n";
        }
        if (t.max_iterations > 0) {
            fm += "max_iterations: "
                + std::to_string(t.max_iterations) + "\n";
        }
        if (t.max_consecutive_empty_turns > 0) {
            fm += "max_consecutive_empty_turns: "
                + std::to_string(t.max_consecutive_empty_turns) + "\n";
        }
        if (!t.allowed_tools.empty()) {
            fm += "allowed_tools:\n";
            for (const auto& tool : t.allowed_tools) {
                fm += "  - " + tool + "\n";
            }
        }
        fm += "---\n" + t.identity_body + "\n";
        write_file(dir_ / ("identity_" + t.name + ".md"), fm);
    }

    std::string tier_config_block(const TierSpec& t) const {
        return "  " + t.name + ":\n"
             + "    path: " + t.gguf_key + "\n"
             + "    adapter: " + t.adapter + "\n"
             + "    context_length: " + std::to_string(t.context_length) + "\n"
             + "    gpu_layers: " + std::to_string(t.gpu_layers) + "\n"
             + "    n_parallel: " + std::to_string(t.n_parallel) + "\n"
             + "    identity: " + (dir_ / ("identity_" + t.name + ".md")).string()
             + "\n";
    }

    fs::path dir_;
    entropic_handle_t handle_ = nullptr;
    std::string setup_failure_;  ///< Reason the last setup() failed
};

// ── Running a turn: transcript vs answer (v2.13.0) ──────────
//
// `entropic_run`, `entropic_run_as` and `entropic_run_session` all return
// `serialize_messages(engine->run_turn(...))` — the WHOLE conversation,
// system prompt and every user turn included, not the new assistant reply.
//
// A model test that treats that string as "the answer" cannot fail a
// positive content assertion it seeded itself. The gh#165 shape:
//
//     run_turn(h, k, "Remember this word ...: cinnamon");
//     answer = run_turn(h, k, "Repeat that exact word now.");
//     CHECK(contains_ci(answer, "cinnamon"));     // ← ALWAYS true
//
// `answer` contains the seed message the test wrote two lines earlier, so
// the CHECK passes whatever the model does — including decoding zero
// characters, which is exactly what the v2.13.0 G4 gate log shows under
// 28 green assertions. Only the negative form (`CHECK_FALSE(...)`) carried
// any signal.
//
// So a run gives a call site BOTH, under two names, and the call site has
// to pick per assertion:
//
//   run_*_transcript(...)  — the whole serialized conversation. Correct
//       when the assertion is ABOUT the conversation: a round trip, a
//       message count, or a NEGATIVE over everything the turn produced
//       ("no <think> block anywhere", "this session's history never saw
//       the other session's secret"). A negative narrowed to the answer
//       would be WEAKER, so those deliberately stay here.
//   final_answer(transcript) — the last non-empty assistant message, via
//       the bridge's own `facade_text::extract_final_text`. Correct for
//       every positive claim about what the MODEL said.
//
// Call sites keep the transcript around even when they assert on the
// answer, so a failure prints why — untruncated.

/// @brief Take ownership of a facade out-parameter string. @utility
/// @version 2.13.0
inline std::string take_owned(char* out) {
    if (out == nullptr) { return {}; }
    std::string s = out;
    entropic_free(out);
    return s;
}

/// @brief Final assistant message of a serialized conversation.
///
/// Empty when the turn produced no assistant text at all — which is a
/// FAILING answer, and the whole point: the transcript form could not
/// express it.
/// @param transcript Serialized conversation JSON from a facade run.
/// @return The last non-empty assistant content, or "".
/// @utility @version 2.13.0
inline std::string final_answer(const std::string& transcript) {
    return facade_text::extract_final_text(transcript.c_str());
}

/// @brief Run a turn on the default session; whole conversation.
/// @utility @version 2.13.0
inline std::string run_transcript(entropic_handle_t h, const char* input) {
    char* out = nullptr;
    const auto rc = entropic_run(h, input, &out);
    auto text = take_owned(out);
    return (rc == ENTROPIC_OK) ? text : std::string{};
}

/// @brief Run a turn on a named session; whole conversation.
///
/// Empty on a non-OK return — which is how a caller distinguishes "the run
/// was refused" (e.g. ENTROPIC_ERROR_EVAL_CONTEXT_FULL since 26884f6) from
/// "the model said nothing".
/// @utility @version 2.13.0
inline std::string run_session_transcript(entropic_handle_t h,
                                          const char* key,
                                          const char* input) {
    char* out = nullptr;
    const auto rc = entropic_run_session(h, key, input, &out);
    auto text = take_owned(out);
    return (rc == ENTROPIC_OK) ? text : std::string{};
}

}  // namespace entropic::test::facade
