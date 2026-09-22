// SPDX-License-Identifier: Apache-2.0
/**
 * @file expert_offload_buft.h
 * @brief EXPERIMENTAL (v2.13.0): the llama.cpp binding for expert offload —
 *        an owning, null-terminated `llama_model_tensor_buft_override` array.
 *
 * `llama_model_params::tensor_buft_overrides` is a raw pointer to a
 * null-terminated array whose elements hold `const char* pattern`. llama.cpp
 * copies neither: the loader stores the pointer and dereferences the strings
 * while it places tensors (`llama-model-loader.cpp`, `std::regex_search` per
 * tensor per pattern). **The array AND every string it points at must
 * therefore outlive `llama_model_load_from_file`.**
 *
 * Upstream solves this with a function-local `static std::list<std::string>`
 * that is never freed (`llm_add_n_cpu_ffn_overrides`). That is fine for a CLI
 * that loads one model and exits; it is not fine for a library that loads,
 * unloads and reloads models across many handles, so this owns its storage
 * instead and dies with the scope that built it. The invariant is the one
 * property worth asserting in a unit test: `patterns_` is filled ONCE and
 * never touched again, so the `c_str()` values stored in `overrides_` stay
 * valid for the object's whole life.
 *
 * Copy and move are deleted — not because a move would be unsafe (a
 * `std::vector` move transfers the buffer; the `std::string` elements
 * themselves do not move) but because "this object contains pointers into
 * itself" is a claim better enforced than reasoned about. Construct it in the
 * scope that performs the load.
 *
 * Separated from `expert_offload.h` so that the pure rule stays vendor-free
 * and `src/config/validate.cpp` — which links no llama.cpp — can include it.
 *
 * @version 2.13.0
 */

#pragma once

#include "expert_offload.h"

#include <llama.h>

#include <cstddef>
#include <string>
#include <vector>

namespace entropic {

/**
 * @brief Owning, null-terminated tensor-buffer override list for expert
 *        offload.
 *
 * Empty (and `data() == nullptr`) whenever `cpu_moe_layers <= 0`, which is
 * what makes an absent config key byte-identical to every release before
 * this one: `llama_model_default_params()` already leaves
 * `tensor_buft_overrides` null, and assigning null over null changes nothing.
 *
 * @req REQ-INFER-027
 * @version 2.13.0
 */
class ExpertOffloadOverrides {
public:
    /**
     * @brief Build the override list for the first N layers.
     * @param cpu_moe_layers Layers whose expert tensors go to the host;
     *                       <= 0 builds an empty list.
     * @param cpu_buft Destination buffer type, normally
     *                 `ggml_backend_cpu_buffer_type()`. Taken as a parameter
     *                 rather than called here so the construction is
     *                 assertable without a live ggml backend registry.
     * @version 2.13.0
     */
    ExpertOffloadOverrides(int cpu_moe_layers,
                           ggml_backend_buffer_type_t cpu_buft)
        : patterns_(expert_offload_patterns(cpu_moe_layers)) {
        if (patterns_.empty()) { return; }
        // +1 for the terminator. Reserved up front so no push_back can
        // reallocate mid-fill; `patterns_` is never modified after the
        // member initializer, so every c_str() below stays valid.
        overrides_.reserve(patterns_.size() + 1);
        for (const std::string& pattern : patterns_) {
            overrides_.push_back({pattern.c_str(), cpu_buft});
        }
        overrides_.push_back({nullptr, nullptr});
    }

    ExpertOffloadOverrides(const ExpertOffloadOverrides&) = delete;
    ExpertOffloadOverrides& operator=(const ExpertOffloadOverrides&) = delete;
    ExpertOffloadOverrides(ExpertOffloadOverrides&&) = delete;
    ExpertOffloadOverrides& operator=(ExpertOffloadOverrides&&) = delete;
    ~ExpertOffloadOverrides() = default;

    /**
     * @brief Pointer to hand to `llama_model_params::tensor_buft_overrides`.
     * @return Null-terminated array, or nullptr when the feature is off.
     * @req REQ-INFER-027
     * @version 2.13.0
     */
    const llama_model_tensor_buft_override* data() const {
        return overrides_.empty() ? nullptr : overrides_.data();
    }

    /**
     * @brief The owned pattern strings, in layer order.
     * @return Stable storage backing every `pattern` in `data()`.
     * @utility
     * @version 2.13.0
     */
    const std::vector<std::string>& patterns() const { return patterns_; }

    /**
     * @brief Number of override entries, excluding the terminator.
     * @return Layer count actually overridden; 0 when off.
     * @utility
     * @version 2.13.0
     */
    std::size_t size() const { return patterns_.size(); }

private:
    std::vector<std::string> patterns_;                      ///< Owned strings
    std::vector<llama_model_tensor_buft_override> overrides_; ///< + terminator
};

}  // namespace entropic
