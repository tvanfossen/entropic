// SPDX-License-Identifier: Apache-2.0
/**
 * @file load_count_test.cpp
 * @brief gh#148 (v2.13.0) — a cold activation must read the file ONCE.
 *
 * The issue reported "closer to 1.5x the file size" for a partially
 * offloaded model and blamed an overlap between the WARM and ACTIVE
 * residencies. That overlap does not exist: `load_gpu_model` has freed the
 * WARM model BEFORE the GPU reload since v2.7.0, so the two are never
 * simultaneously resident, and what the reporter measured was page cache.
 *
 * The real waste is next to it and is not a memory figure at all:
 * `load_and_activate` ran `do_load` — the WHOLE file, `n_gpu_layers = 0` —
 * and then immediately threw that model away and read the whole file again
 * with the configured `gpu_layers`. One cold activation, two whole-file
 * loads.
 *
 * So this asserts WORK DONE rather than RSS: a deterministic counter of
 * `llama_model_load_from_file` calls, the same instrumentation shape as
 * gh#161's regex-evaluation counter. RSS and wall-clock both depend on page
 * cache state, which is precisely why the original report could not be
 * turned into a test.
 *
 * CPU-only (`gpu_layers = 0`): the doubled read is not GPU-specific — it is
 * in the lifecycle, and the counter reads the same on either.
 *
 * SEPARATE binary: its own process, so the process-wide counter is not
 * perturbed by another test's model load.
 *
 * @version 2.13.0
 */

#include <catch2/catch_test_macros.hpp>

#include <entropic/types/config.h>
#include "../../../src/inference/llama_cpp_backend.h"

#include <cstdlib>
#include <filesystem>
#include <string>

namespace {

std::filesystem::path small_model() {
    const char* home = std::getenv("HOME");
    if (home == nullptr) { return {}; }
    return std::filesystem::path(home) /
           ".entropic" / "models" / "Qwen3.5-0.8B-Q8_0.gguf";
}

entropic::ModelConfig cpu_config(const std::filesystem::path& path) {
    entropic::ModelConfig cfg;
    cfg.path = path;
    cfg.adapter = "qwen35";
    cfg.gpu_layers = 0;
    cfg.context_length = 512;
    cfg.n_threads = 2;
    cfg.use_mlock = false;
    cfg.flash_attn = false;
    return cfg;
}

}  // namespace

SCENARIO("gh#148: one cold activation reads the model file once",
         "[inference][gh148][realmodel][2.13.0]") {
    auto path = small_model();
    if (path.empty() || !std::filesystem::exists(path)) {
        SUCCEED("Qwen3.5-0.8B-Q8_0.gguf absent — skipping load-count smoke");
        return;
    }

    GIVEN("a COLD backend") {
        entropic::LlamaCppBackend backend;
        entropic::LlamaCppBackend::reset_model_file_loads();

        WHEN("it is loaded and activated in one step") {
            REQUIRE(backend.load_and_activate(cpu_config(path)));

            THEN("the file was read exactly once") {
                // RED on unfixed code: 2. do_load reads the whole file with
                // n_gpu_layers=0, then load_gpu_model frees it and reads the
                // whole file again with the configured split.
                CHECK(entropic::LlamaCppBackend::model_file_loads() == 1u);
                CHECK(backend.is_active());
            }

            AND_THEN("the backend is usable — a load count is not enough") {
                // A single load that produced an unusable backend would
                // satisfy the counter and nothing else.
                CHECK(backend.count_tokens("hello world") > 0);
            }

            backend.unload();
        }
    }
}

SCENARIO("gh#148: the WARM path deliberately keeps its reload",
         "[inference][gh148][realmodel][2.13.0]") {
    auto path = small_model();
    if (path.empty() || !std::filesystem::exists(path)) {
        SUCCEED("Qwen3.5-0.8B-Q8_0.gguf absent — skipping WARM-path smoke");
        return;
    }

    GIVEN("a backend loaded to WARM first, as a keep_warm swap-out leaves it") {
        entropic::LlamaCppBackend backend;
        entropic::LlamaCppBackend::reset_model_file_loads();

        REQUIRE(backend.load(cpu_config(path)));
        CHECK(entropic::LlamaCppBackend::model_file_loads() == 1u);

        WHEN("it is then activated") {
            REQUIRE(backend.activate());

            THEN("that IS a reload, and remains one") {
                // Design decision #19: llama.cpp ties offloading to the model
                // load, so WARM → ACTIVE cannot re-place layers without
                // reloading. gh#148 removes the pointless FIRST load on the
                // cold path; it does not pretend this one is avoidable.
                CHECK(entropic::LlamaCppBackend::model_file_loads() == 2u);
                CHECK(backend.is_active());
            }
        }

        backend.unload();
    }
}
