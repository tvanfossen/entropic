// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_gh87_verify_qwen36.cpp
 * @brief gh#87 Phase A: verify qwen36 family parses via common_chat.
 * @version 2.8.3
 */

#include "gh87_verify_helpers.h"

TEST_CASE("gh#87 Phase A: qwen36 tool call round-trips through common_chat",
          "[gh87][model][verify]") {
    // Qwen3.6-35B-A3B at IQ3_XXS is ~13GB — partial CPU offload on 11GB GPU.
    // gpu_layers=15 (== PARTIAL_GPU_LAYERS, matching test_gh97_hybrid_cache's
    // "OOM-safe on 11GB" offload for this same model). v2.8.2 shipped this at
    // 20, which over-commits the 11GB GTX 1080 Ti and OOMs at activate() time
    // (compute-buffer alloc) — the in-test retry loop only retries
    // generate+parse, not load/activate, so a load-time OOM fails every attempt.
    gh87verify::verify_family_common_chat(
        "Qwen3.6-35B-A3B-UD-IQ3_XXS.gguf", "qwen36", /*gpu_layers=*/15);
}
