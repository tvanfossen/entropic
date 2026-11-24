// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_gh87_verify_qwen35.cpp
 * @brief gh#87 Phase A: verify the qwen35 family parses via common_chat.
 * @version 2.8.3
 */

#include "gh87_verify_helpers.h"

TEST_CASE("gh#87 Phase A: qwen35 tool call round-trips through common_chat",
          "[gh87][model][verify]") {
    // v2.14.6: was test_gh87_verify_qwen36 on Qwen3.6-35B-A3B (~13 GB,
    // gpu_layers=15 partial offload), which left the pared model set.
    // Qwen3.5-0.8B is the on-disk member of the same qwen35 family and
    // carries the same qwen3_coder XML tool-call template (<tool_call>
    // <function=...>, read from its GGUF chat template). At 0.8 GB it fits
    // any card whole, so gpu_layers=99 (full offload) like the other small
    // families — the 15-layer clamp existed only for the 13 GB model.
    gh87verify::verify_family_common_chat(
        "Qwen3.5-0.8B-Q8_0.gguf", "qwen35", /*gpu_layers=*/99);
}
