// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file gguf_metadata.h
 * @brief Read a GGUF's shape without loading its tensors (v2.13.1).
 *
 * `gpu_layers: auto` has to answer "how many layers fit" before anything is
 * loaded, and until v2.13.1 it answered without knowing how many layers the
 * model had. `partial_offload.h` deliberately reads no GGUF metadata — a
 * pure header with no llama.cpp dependency — so it defaults to 30 layers and
 * documents that as a low-end guess. That guess is exactly right for the
 * 26B-A4B and 40% wrong for a 42-layer E4B, which is priced as if each layer
 * cost `file_bytes / 30` and therefore derives a count that leaves the card
 * underused (gh#192).
 *
 * The information was always available. `gguf_init_from_file` with
 * `no_alloc = true` maps the header, the key/value metadata and the tensor
 * index, and reads none of the tensor data — so a 10 GB model costs a header
 * read to answer:
 *
 *   - `general.architecture`, which names the other keys
 *   - `<arch>.block_count`, the real layer count
 *   - `<arch>.expert_count`, non-zero exactly for a MoE
 *   - every tensor's name and size, which is what makes expert placement
 *     EXACT rather than a guessed fraction of a layer
 *
 * That last point is the one that matters for a MoE. Deriving an expert
 * split from "experts are most of a layer" would be a heuristic deciding
 * residency; summing the bytes of the tensors that actually match the expert
 * pattern is a measurement.
 *
 * @version 2.13.1
 */
#pragma once

#include <cstdint>
#include <string>

namespace entropic {

/**
 * @brief A GGUF's shape, as far as its metadata states it.
 *
 * Every field is "what the file says", never an inference. `known == false`
 * means the file could not be read or did not declare its architecture, and
 * callers must fall back rather than treat the zeros as measurements — the
 * same contract `FootprintEstimate` uses.
 * @version 2.13.1
 */
struct GgufShape {
    bool known = false;          ///< False = read failed; fields are meaningless.
    std::string architecture;    ///< `general.architecture`, "" when absent.
    int block_count = 0;         ///< `<arch>.block_count` — the real layer count.
    int expert_count = 0;        ///< `<arch>.expert_count`; 0 = dense, >0 = MoE.

    /// @brief Total bytes of tensors that belong to a numbered block.
    ///
    /// Excludes token embeddings, the output head and final norms, which are
    /// resident regardless of how many layers are offloaded. Pricing a layer
    /// as `file_bytes / block_count` folds those in and overstates per-layer
    /// cost — the error that makes `auto` conservative even when it knows
    /// the layer count.
    uint64_t block_bytes = 0;

    /// @brief Bytes of expert (MoE FFN) tensors inside `block_bytes`.
    ///
    /// Summed over tensors matching llama.cpp's own expert pattern. 0 for a
    /// dense model. This is what a `cpu_moe_layers` split actually moves.
    uint64_t expert_bytes = 0;

    /// @brief Bytes not attributable to any block — always resident.
    uint64_t non_block_bytes = 0;

    /**
     * @brief Average bytes per transformer block.
     * @return `block_bytes / block_count`, or 0 when unknown.
     * @utility
     * @version 2.13.1
     */
    uint64_t bytes_per_block() const {
        if (!known || block_count <= 0) { return 0; }
        return block_bytes / static_cast<uint64_t>(block_count);
    }

    /**
     * @brief Average expert bytes per block — what one `cpu_moe_layers` buys.
     * @return `expert_bytes / block_count`, or 0 when dense or unknown.
     * @utility
     * @version 2.13.1
     */
    uint64_t expert_bytes_per_block() const {
        if (!known || block_count <= 0) { return 0; }
        return expert_bytes / static_cast<uint64_t>(block_count);
    }
};

/**
 * @brief Read a GGUF's shape from its metadata, without loading tensor data.
 *
 * Opens with `no_alloc = true`, so the cost is the header plus the tensor
 * index regardless of the file's size.
 *
 * @param path Filesystem path to the `.gguf`.
 * @return The shape; `known == false` when the file cannot be read or
 *         declares no architecture. Never throws — a metadata read failing
 *         must degrade `auto` to its previous behaviour, not fail a load.
 * @req REQ-INFER-019
 * @version 2.13.1
 */
GgufShape read_gguf_shape(const std::string& path);

}  // namespace entropic
