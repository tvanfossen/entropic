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
 * @version 2.8.0
 */
#pragma once

#include <entropic/entropic.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
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
        write_file(dir_ / "config.local.yaml", cfg);
    }

    void write_tier_identity(const TierSpec& t) {
        // `focus:` is required by the identity loader (>= one entry).
        std::string fm = "---\ntype: identity\nversion: 1\nname: " + t.name
                       + "\nfocus:\n  - act in character\n"
                       + "enable_thinking: "
                       + (t.enable_thinking ? "true" : "false") + "\n";
        if (!t.grammar_name.empty()) { fm += "grammar: " + t.grammar_name + "\n"; }
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

}  // namespace entropic::test::facade
