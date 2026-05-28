// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_gh87_verify_qwen35.cpp
 * @brief gh#87 Phase A: verify qwen35 family parses via common_chat.
 * @version 2.7.0
 */

#include "gh87_verify_helpers.h"

TEST_CASE("gh#87 Phase A: qwen35 tool call round-trips through common_chat",
          "[gh87][model][verify]") {
    // Qwen3.5-4B fits fully on an 11GB GPU.
    gh87verify::verify_family_common_chat(
        "Qwen3.5-4B-UD-Q4_K_XL.gguf", "qwen35", /*gpu_layers=*/99);
}
