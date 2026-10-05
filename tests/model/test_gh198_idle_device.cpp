// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_gh198_idle_device.cpp
 * @brief gh#198 (v2.14.0): a `defer_load` handle owns no GPU memory until it
 *        is asked to run.
 *
 * A consumer measured 138 MiB per idle host on a GTX 1080 Ti — four hosts,
 * ~570 MiB, no model resident in any of them — because configure probed the
 * device for its free VRAM, and that probe is what creates a CUDA context.
 * The CPU half of the fix is pinned by deferred_load_test.cpp (a call
 * counter). This is the half only a real card can answer: does the PROCESS
 * show up in nvidia-smi before it runs.
 *
 * It asserts both directions on purpose. "This process is absent from the
 * card" proves nothing unless the same measurement, a moment later, SEES the
 * process once it has run — otherwise an nvidia-smi that cannot see us at
 * all would pass the first half forever.
 *
 * No harness model: FacadeTestListener, never ModelTestListener — that one
 * loads a model in-process before the run starts, which would put a context
 * on the card before the first assertion and make this test meaningless.
 *
 * @version 2.14.0
 */
#include <catch2/catch_test_macros.hpp>

#include "facade_model_helpers.h"
#include "model_test_context.h"

#include <cstdio>
#include <string>
#include <unistd.h>

CATCH_REGISTER_LISTENER(FacadeTestListener)

namespace {

/**
 * @brief MiB this process holds on the GPU, per nvidia-smi.
 * @return MiB (0 when this PID has no CUDA context), or -1 when nvidia-smi
 *         could not be run.
 * @utility
 * @version 2.14.0
 */
long this_process_gpu_mib() {
    FILE* f = popen("nvidia-smi --query-compute-apps=pid,used_memory "
                    "--format=csv,noheader,nounits 2>/dev/null", "r");
    if (f == nullptr) { return -1; }
    const long me = static_cast<long>(::getpid());
    long found = 0;
    long pid = 0;
    long mib = 0;
    while (std::fscanf(f, " %ld , %ld", &pid, &mib) == 2) {
        if (pid == me) { found = mib; }
    }
    const int rc = pclose(f);
    return rc == 0 ? found : -1;
}

}  // namespace

SCENARIO("gh#198: a deferred handle holds no GPU memory until it runs",
         "[model][gh198][2.14.0]") {
    GIVEN("a configured defer_load handle on a small model") {
        auto gguf = entropic::test::facade::model_gguf(
            "gemma-4-E2B-it-qat-UD-Q4_K_XL.gguf");
        if (gguf.empty() || !fs::is_regular_file(gguf)) {
            SKIP("gemma-4-E2B QAT GGUF not present at " + gguf.string());
        }
        if (this_process_gpu_mib() < 0) {
            SKIP("nvidia-smi unavailable — cannot observe per-process VRAM");
        }

        entropic::test::facade::FacadeProject project("gh198_idle_device");
        project.defer_load = true;
        entropic::test::facade::TierSpec lead;
        lead.name = "lead";
        lead.gguf_key = "gemma4_e2b_qat";
        lead.adapter = "gemma4";
        lead.identity_body = "Answer in one short sentence.";
        lead.context_length = 2048;
        lead.allowed_tools = {"entropic.complete"};
        lead.explicit_completion = false;
        auto* h = project.setup({lead});
        INFO("setup: " << project.setup_failure());
        REQUIRE(h != nullptr);

        THEN("this process has no CUDA context on the card") {
            CHECK(this_process_gpu_mib() == 0);
        }

        WHEN("it is asked to run once") {
            char* out = nullptr;
            REQUIRE(entropic_run(h, "Say hello.", &out) == ENTROPIC_OK);
            entropic_free(out);

            THEN("the same measurement now sees it — so the absence was real") {
                // Without this, "absent" could mean nvidia-smi cannot see
                // this process at all, and the first THEN would be vacuous.
                CHECK(this_process_gpu_mib() > 0);
            }
        }
    }
}
