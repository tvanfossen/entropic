// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_gh95_identity_grammar.cpp
 * @brief gh#95: a GBNF grammar bound via an identity `grammar:` frontmatter
 *        field must actually constrain that tier's generation.
 *
 * FACADE-DRIVEN (the production path the model harness bypasses): build a temp
 * project, `entropic_configure_dir` + `entropic_run` through the real C-ABI.
 * The bug (gh#95): `thread_frontmatter_sampler` threads the sampler knobs but
 * omits `grammar`, so the identity `grammar:` key never reaches
 * `TierConfig.grammar` → `resolve_grammar_key` finds nothing → output is
 * unconstrained. Red on the current rev, green once the grammar is threaded.
 *
 * Deterministic: the grammar is `root ::= "HELLO"`, so a correctly-applied
 * constraint forces the literal output `HELLO` regardless of model quality.
 *
 * Requires: GPU + gemma4_e2b GGUF. Run: ctest -L model -R gh95
 *
 * @version 2.7.4
 */

#include <catch2/catch_test_macros.hpp>

#include <entropic/entropic.h>
#include <entropic/types/config.h>
#include <entropic/types/message.h>
#include "../../src/inference/llama_cpp_backend.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

/// @brief Write a file, creating parent dirs.
/// @utility
/// @version 2.7.4
void write_file(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << content;
}

}  // namespace

SCENARIO("gh#95: identity grammar: field constrains that tier's output",
         "[model][gh95]")
{
    GIVEN("a tier whose identity frontmatter binds grammar: forcehello") {
        const char* home = std::getenv("HOME");
        REQUIRE(home != nullptr);
        fs::path gguf = fs::path(home) / ".entropic" / "models"
                        / "gemma-4-E2B-it-Q8_0.gguf";
        if (!fs::is_regular_file(gguf)) {
            SKIP("gemma4_e2b GGUF not present at " + gguf.string());
        }

        fs::path dir = fs::temp_directory_path() / "entropic_gh95_identity_grammar";
        fs::remove_all(dir);
        fs::create_directories(dir);

        // root ::= "HELLO" — the only valid output under the grammar.
        write_file(dir / "forcehello.gbnf", "root ::= \"HELLO\"\n");
        // Identity binds the grammar via the frontmatter field under test.
        write_file(dir / "identity_lead.md",
                   "---\ntype: identity\nversion: 1\nname: lead\n"
                   "focus:\n  - answer tersely\n"
                   "enable_thinking: false\ngrammar: forcehello\n---\n"
                   "You are a terse assistant.\n");
        // Project config: REPLACE semantics — only this tier exists, so only
        // gemma4_e2b loads (no global/default tier bleed). Constitutional
        // validation off so its critique generation doesn't add noise.
        write_file(dir / "config.local.yaml",
                   "models:\n"
                   "  lead:\n"
                   "    path: gemma4_e2b\n"
                   "    adapter: gemma4\n"
                   "    context_length: 16384\n"  // fits the ~4900-token tool prompt
                   "    gpu_layers: 99\n"
                   "    identity: " + (dir / "identity_lead.md").string() + "\n"
                   "  default: lead\n"
                   "constitutional_validation:\n"
                   "  enabled: false\n");

        // Point discovery at the repo data dir for bundled_models + prompts.
        setenv("ENTROPIC_DATA_DIR",
               (fs::path(MODEL_PATH) / "data").string().c_str(), 1);

        entropic_handle_t h = nullptr;
        REQUIRE(entropic_create(&h) == ENTROPIC_OK);
        REQUIRE(entropic_configure_dir(h, dir.string().c_str()) == ENTROPIC_OK);
        // Registration works (the issue confirms); the gap is enforcement.
        REQUIRE(entropic_grammar_register_file(
                    h, "forcehello",
                    (dir / "forcehello.gbnf").string().c_str()) == ENTROPIC_OK);

        WHEN("a prompt is run on that tier") {
            char* out = nullptr;
            auto rc = entropic_run(
                h, "Write a short paragraph about cats.", &out);
            std::string result = (out != nullptr) ? out : "";
            if (out != nullptr) { entropic_free(out); }

            entropic_destroy(h);

            THEN("the identity grammar constrains the tier's output to HELLO") {
                INFO("rc=" << rc << "\nresult=[" << result << "]");
                REQUIRE(rc == ENTROPIC_OK);
                // gh#95 (deterministic behavioral proof): root ::= "HELLO"
                // forces the literal output. On the bug the identity grammar:
                // key was dropped in thread_frontmatter_sampler — registered
                // but never threaded to the tier, so resolve_grammar_key found
                // nothing and output was unconstrained (≠ HELLO). The fix
                // threads it; the sampler enforces it.
                // NB: context_length must fit the staged tool prompt — a
                // too-small ctx overflows and garbles output regardless.
                CHECK(result.find("HELLO") != std::string::npos);
            }
        }

        fs::remove_all(dir);
    }
}

SCENARIO("gh#95 isolation: GBNF grammar constrains backend output with NO tools",
         "[model][gh95]")
{
    GIVEN("a backend with grammar root ::= \"HELLO\" and no tools staged") {
        const char* home = std::getenv("HOME");
        REQUIRE(home != nullptr);
        std::filesystem::path gguf = std::filesystem::path(home) / ".entropic"
            / "models" / "gemma-4-E2B-it-Q8_0.gguf";
        if (!std::filesystem::is_regular_file(gguf)) {
            SKIP("gemma4_e2b GGUF not present");
        }

        entropic::LlamaCppBackend backend;
        entropic::ModelConfig cfg;
        cfg.path = gguf;
        cfg.adapter = "gemma4";
        cfg.context_length = 4096;
        cfg.gpu_layers = 99;
        cfg.flash_attn = false;
        REQUIRE(backend.load(cfg));
        REQUIRE(backend.activate());
        // NO set_active_tools — isolate the grammar sampler from common_chat.

        std::vector<entropic::Message> msgs;
        entropic::Message u;
        u.role = "user";
        u.content = "Write a short paragraph about cats.";
        msgs.push_back(u);

        entropic::GenerationParams params;
        params.max_tokens = 32;
        params.temperature = 0.0f;
        params.grammar = "root ::= \"HELLO\"\n";  // raw GBNF, no registry

        WHEN("generating with the grammar and no tools") {
            auto result = backend.generate(msgs, params);
            backend.deactivate();
            backend.unload();
            THEN("the grammar forces the output to HELLO") {
                INFO("content=[" << result.content << "]");
                // Backend-level floor: the grammar sampler enforces the
                // constraint independent of the facade/identity threading.
                // This always held — it pins the invariant that gh#95 was
                // purely a threading-layer drop (the identity grammar: key
                // never reaching params.grammar), not a sampler defect.
                CHECK(result.content.find("HELLO") != std::string::npos);
            }
        }
    }
}
