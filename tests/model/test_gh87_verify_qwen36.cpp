// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_gh87_verify_qwen36.cpp
 * @brief gh#87 Phase A: verify qwen36 family parses via common_chat.
 * @version 2.7.0
 */

#include "gh87_verify_helpers.h"

TEST_CASE("gh#87 Phase A: qwen36 tool call round-trips through common_chat",
          "[gh87][model][verify]") {
    // Qwen3.6-35B-A3B at IQ3_XXS is ~13GB — partial CPU offload on 11GB GPU.
    gh87verify::verify_family_common_chat(
        "Qwen3.6-35B-A3B-UD-IQ3_XXS.gguf", "qwen36", /*gpu_layers=*/20);
}
