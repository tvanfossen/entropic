// SPDX-License-Identifier: Apache-2.0
/**
 * @file vocab_only_smoke_test.cpp
 * @brief CPU-only smoke covering LlamaCppTokenizer + LlamaCppBackend
 *        load/unload via vocab-only GGUF fixtures.
 *
 * Loads a `ggml-vocab-*.gguf` fixture (sub-100MB) two ways:
 *   1. Directly via llama.cpp C API with `mparams.vocab_only=true`
 *      to construct a real `LlamaCppTokenizer` and exercise
 *      tokenize/detokenize/count_tokens against actual vocab data.
 *   2. Through `LlamaCppBackend::load()` (which uses vocab_only=false)
 *      to exercise the do_load / do_unload code paths. Outcome is
 *      not pinned — both success and failure exercise the branches.
 *
 * Pre-v2.3.10 these files were 0% / 4% covered in the CPU-only gate.
 *
 * @version 2.3.10
 */

#include <catch2/catch_test_macros.hpp>

#include <entropic/inference/tokenizer.h>
#include <entropic/types/config.h>
#include "../../../src/inference/llama_cpp_backend.h"
#include "../../../src/inference/llama_cpp_tokenizer.h"

#include <llama.h>

#include <filesystem>
#include <string>
#include <vector>

namespace {

/// Resolve a vocab-only GGUF relative to the source tree.
/// `ENTROPIC_SOURCE_DIR` is injected via target_compile_definitions.
std::filesystem::path find_vocab_gguf(const std::string& name) {
#ifdef ENTROPIC_SOURCE_DIR
    return std::filesystem::path(ENTROPIC_SOURCE_DIR)
        / "extern" / "llama.cpp" / "models" / name;
#else
    return std::filesystem::path("extern/llama.cpp/models") / name;
#endif
}

/// RAII wrapper around a vocab-only llama_model load. Model freed in
/// destructor; vocab pointer is borrowed (owned by the model).
struct VocabOnlyModel {
    llama_model* model = nullptr;
    const llama_vocab* vocab = nullptr;

    explicit VocabOnlyModel(const std::filesystem::path& path) {
        llama_model_params mparams = llama_model_default_params();
        mparams.n_gpu_layers = 0;
        mparams.vocab_only = true;   // ← the crux — skip weight tensors
        mparams.use_mmap = true;
        mparams.use_mlock = false;
        model = llama_model_load_from_file(path.c_str(), mparams);
        if (model != nullptr) {
            vocab = llama_model_get_vocab(model);
        }
    }

    ~VocabOnlyModel() {
        if (model != nullptr) {
            llama_model_free(model);
        }
    }

    VocabOnlyModel(const VocabOnlyModel&) = delete;
    VocabOnlyModel& operator=(const VocabOnlyModel&) = delete;

    bool valid() const { return model != nullptr && vocab != nullptr; }
};

} // anonymous namespace

// ── LlamaCppTokenizer: real-vocab smoke ─────────────────────

SCENARIO("LlamaCppTokenizer round-trips ASCII against a real vocab",
         "[v2.3.10][inference][vocab_only_smoke]")
{
    auto path = find_vocab_gguf("ggml-vocab-llama-bpe.gguf");
    if (!std::filesystem::exists(path)) {
        WARN("vocab GGUF missing at "
             + path.string()
             + " — skipping tokenizer real-vocab smoke");
        return;
    }

    VocabOnlyModel om(path);
    REQUIRE(om.valid());

    GIVEN("a LlamaCppTokenizer wired to the loaded vocab") {
        entropic::LlamaCppTokenizer tok(om.vocab);

        WHEN("tokenize('hello world') is invoked") {
            auto ids = tok.tokenize("hello world", false);
            THEN("at least one token is produced") {
                REQUIRE(!ids.empty());
                // Sanity: token IDs are non-negative.
                for (auto t : ids) {
                    REQUIRE(t >= 0);
                }
            }

            AND_THEN("count_tokens matches tokenize().size()") {
                int n = tok.count_tokens("hello world");
                REQUIRE(n == static_cast<int>(ids.size()));
            }

            AND_THEN("detokenize over the full id stream produces "
                     "a non-empty surface string") {
                std::string surface;
                for (auto t : ids) {
                    surface += tok.detokenize(t);
                }
                // Tokenizers vary in how aggressively they strip
                // whitespace, so we don't assert exact equality with
                // the input — only that detokenize produced text.
                REQUIRE(!surface.empty());
            }
        }
    }
}

SCENARIO("LlamaCppTokenizer is graceful on empty input",
         "[v2.3.10][inference][vocab_only_smoke][failure-mode]")
{
    auto path = find_vocab_gguf("ggml-vocab-llama-bpe.gguf");
    if (!std::filesystem::exists(path)) {
        WARN("vocab GGUF missing — skipping empty-input smoke");
        return;
    }
    VocabOnlyModel om(path);
    REQUIRE(om.valid());
    entropic::LlamaCppTokenizer tok(om.vocab);

    WHEN("tokenize / count_tokens are called with an empty string") {
        auto ids = tok.tokenize("", false);
        int n = tok.count_tokens("");
        THEN("count_tokens matches tokenize().size() (both safe)") {
            REQUIRE(n == static_cast<int>(ids.size()));
        }
    }
}

SCENARIO("LlamaCppTokenizer handles unicode + multi-byte UTF-8",
         "[v2.3.10][inference][vocab_only_smoke]")
{
    auto path = find_vocab_gguf("ggml-vocab-llama-bpe.gguf");
    if (!std::filesystem::exists(path)) {
        WARN("vocab GGUF missing — skipping unicode smoke");
        return;
    }
    VocabOnlyModel om(path);
    REQUIRE(om.valid());
    entropic::LlamaCppTokenizer tok(om.vocab);

    GIVEN("plain ASCII") {
        REQUIRE(!tok.tokenize("hello", false).empty());
    }
    GIVEN("multi-byte UTF-8 (accented + emoji)") {
        auto ids = tok.tokenize("héllo 🌍", false);
        THEN("tokenize succeeds and count_tokens agrees") {
            REQUIRE(!ids.empty());
            REQUIRE(tok.count_tokens("héllo 🌍")
                    == static_cast<int>(ids.size()));
        }
    }
}

SCENARIO("LlamaCppTokenizer with add_special=true differs from false",
         "[v2.3.10][inference][vocab_only_smoke]")
{
    auto path = find_vocab_gguf("ggml-vocab-llama-bpe.gguf");
    if (!std::filesystem::exists(path)) {
        WARN("vocab GGUF missing — skipping add_special smoke");
        return;
    }
    VocabOnlyModel om(path);
    REQUIRE(om.valid());
    entropic::LlamaCppTokenizer tok(om.vocab);

    WHEN("tokenize is invoked with add_special=true vs false") {
        auto with_special = tok.tokenize("hello world", true);
        auto without = tok.tokenize("hello world", false);
        THEN("both produce tokens; specials path is >= plain path") {
            REQUIRE(!with_special.empty());
            REQUIRE(!without.empty());
            REQUIRE(with_special.size() >= without.size());
        }
    }
}

SCENARIO("LlamaCppTokenizer handles a small SPM vocab as well",
         "[v2.3.10][inference][vocab_only_smoke]")
{
    auto path = find_vocab_gguf("ggml-vocab-llama-spm.gguf");
    if (!std::filesystem::exists(path)) {
        WARN("SPM vocab GGUF missing — skipping cross-tokenizer smoke");
        return;
    }
    VocabOnlyModel om(path);
    REQUIRE(om.valid());
    entropic::LlamaCppTokenizer tok(om.vocab);
    REQUIRE(!tok.tokenize("hello", false).empty());
}

// ── LlamaCppBackend: do_load / do_unload coverage ───────────

SCENARIO("LlamaCppBackend::load against a vocab-only GGUF exercises do_load",
         "[v2.3.10][inference][vocab_only_smoke][backend]")
{
    auto path = find_vocab_gguf("ggml-vocab-llama-bpe.gguf");
    if (!std::filesystem::exists(path)) {
        WARN("vocab GGUF missing — skipping backend do_load smoke");
        return;
    }
    entropic::LlamaCppBackend backend;
    REQUIRE(backend.state() == entropic::ModelState::COLD);
    REQUIRE_FALSE(backend.is_loaded());

    GIVEN("a ModelConfig pointing at the vocab-only GGUF") {
        entropic::ModelConfig cfg;
        cfg.path = path;
        cfg.gpu_layers = 0;        // CPU-only
        cfg.use_mlock = false;     // mlock needs CAP_IPC_LOCK
        cfg.context_length = 512;
        cfg.n_batch = 64;
        cfg.flash_attn = false;

        WHEN("load() is invoked") {
            bool ok = backend.load(cfg);
            THEN("state moves to WARM on success or stays COLD on failure") {
                if (ok) {
                    REQUIRE(backend.state() == entropic::ModelState::WARM);
                    REQUIRE(backend.is_loaded());
                } else {
                    REQUIRE(backend.state() == entropic::ModelState::COLD);
                    REQUIRE_FALSE(backend.is_loaded());
                }
            }
            AND_WHEN("unload() is invoked afterwards") {
                backend.unload();
                THEN("state returns to COLD") {
                    REQUIRE(backend.state() == entropic::ModelState::COLD);
                    REQUIRE_FALSE(backend.is_loaded());
                }
            }
        }
    }
}

SCENARIO("LlamaCppBackend::load with a nonexistent path drives the failure branch",
         "[v2.3.10][inference][vocab_only_smoke][backend][failure-mode]")
{
    entropic::LlamaCppBackend backend;
    REQUIRE(backend.state() == entropic::ModelState::COLD);

    GIVEN("a ModelConfig pointing at a path that doesn't exist") {
        entropic::ModelConfig cfg;
        cfg.path = "/this/path/does/not/exist/nope.gguf";
        cfg.gpu_layers = 0;
        cfg.use_mlock = false;
        cfg.context_length = 512;

        WHEN("load() is invoked") {
            bool ok = backend.load(cfg);

            THEN("load returns false and state stays COLD") {
                REQUIRE_FALSE(ok);
                REQUIRE(backend.state()
                        == entropic::ModelState::COLD);
                REQUIRE_FALSE(backend.is_loaded());
            }
        }
    }
}

SCENARIO("LlamaCppBackend unload after failed load is a safe no-op",
         "[v2.3.10][inference][vocab_only_smoke][backend][failure-mode]")
{
    entropic::LlamaCppBackend backend;
    entropic::ModelConfig cfg;
    cfg.path = "/definitely/not/a/file.gguf";
    cfg.gpu_layers = 0;
    cfg.use_mlock = false;
    (void)backend.load(cfg);

    WHEN("unload is invoked twice on a never-loaded backend") {
        backend.unload();
        backend.unload();
        THEN("state is COLD and no crash occurred") {
            REQUIRE(backend.state() == entropic::ModelState::COLD);
        }
    }
}

// ── v2.3.10 top-up (gh#23): tokenizer fallback branches ────
//
// llama_cpp_tokenizer.cpp lines 65-73 (detokenize "buffer too small"
// retry branch) fire when a token's surface text exceeds 256 bytes.
// Iterate every token in the vocab so any long-piece token routes
// through the larger-buffer retry path.

SCENARIO("LlamaCppTokenizer detokenize sweeps every token in the vocab",
         "[v2.3.10][inference][topup][tokenizer]")
{
    auto path = find_vocab_gguf("ggml-vocab-llama-bpe.gguf");
    if (!std::filesystem::exists(path)) {
        WARN("vocab GGUF missing — skipping detokenize sweep");
        return;
    }
    VocabOnlyModel om(path);
    REQUIRE(om.valid());
    entropic::LlamaCppTokenizer tok(om.vocab);
    int n_vocab = llama_vocab_n_tokens(om.vocab);
    REQUIRE(n_vocab > 0);
    int non_empty = 0;
    for (int id = 0; id < n_vocab; ++id) {
        if (!tok.detokenize(static_cast<int32_t>(id)).empty()) {
            ++non_empty;
        }
    }
    REQUIRE(non_empty > 0);  // Sweep completed; at least one real token.
}

SCENARIO("LlamaCppBackend re-load after unload reuses do_load",
         "[v2.3.10][inference][vocab_only_smoke][backend]")
{
    auto path = find_vocab_gguf("ggml-vocab-llama-bpe.gguf");
    if (!std::filesystem::exists(path)) {
        WARN("vocab GGUF missing — skipping reload smoke");
        return;
    }

    entropic::LlamaCppBackend backend;
    entropic::ModelConfig cfg;
    cfg.path = path;
    cfg.gpu_layers = 0;
    cfg.use_mlock = false;
    cfg.context_length = 512;
    cfg.n_batch = 64;
    cfg.flash_attn = false;

    bool ok1 = backend.load(cfg);
    backend.unload();
    REQUIRE(backend.state() == entropic::ModelState::COLD);

    WHEN("load is invoked a second time on the same backend") {
        bool ok2 = backend.load(cfg);
        THEN("the second load mirrors the first outcome") {
            // Don't pin success — llama.cpp may reject vocab-only
            // files under vocab_only=false defaults. What matters:
            // do_load ran twice, state machine stays consistent.
            REQUIRE(ok1 == ok2);
            if (ok2) {
                REQUIRE(backend.state() == entropic::ModelState::WARM);
            } else {
                REQUIRE(backend.state() == entropic::ModelState::COLD);
            }
            backend.unload();
            REQUIRE(backend.state() == entropic::ModelState::COLD);
        }
    }
}
