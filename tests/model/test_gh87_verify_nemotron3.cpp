// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_gh87_verify_nemotron3.cpp
 * @brief gh#87 Phase A: verify nemotron3 family parses via common_chat.
 * @version 2.7.0
 */

#include "gh87_verify_helpers.h"

TEST_CASE("gh#87 Phase A: nemotron3 tool call round-trips through common_chat",
          "[gh87][model][verify]") {
    // Nemotron-3-Nano-4B is ~3GB — fits fully on an 11GB GPU.
    gh87verify::verify_family_common_chat(
        "NVIDIA-Nemotron-3-Nano-4B-UD-Q4_K_XL.gguf", "nemotron3",
        /*gpu_layers=*/99);
}
