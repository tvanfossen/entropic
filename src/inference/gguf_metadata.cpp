// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file gguf_metadata.cpp
 * @brief Metadata-only GGUF reads for the residency estimator (v2.13.1).
 * @version 2.13.1
 */
#include "gguf_metadata.h"

#include "expert_offload.h"

#include <gguf.h>

#include <regex>
#include <string>

namespace entropic {

namespace {

/**
 * @brief Read a u32 metadata key, or a fallback when it is absent.
 * @param ctx Open gguf context (metadata only).
 * @param key Fully qualified key name.
 * @param fallback Value to return when the key is missing.
 * @return The key's value, or fallback.
 * @dg_internal
 * @version 2.13.1
 */
int u32_or(const gguf_context* ctx, const std::string& key, int fallback) {
    const int64_t id = gguf_find_key(ctx, key.c_str());
    if (id < 0) { return fallback; }
    return static_cast<int>(gguf_get_val_u32(ctx, id));
}

/**
 * @brief Zero-based block index a tensor belongs to, or -1 for none.
 *
 * Reads the `blk.<n>.` prefix llama.cpp gives every per-layer tensor.
 * Anything without it — token embeddings, the output head, final norms — is
 * resident no matter how many layers are offloaded, and is counted
 * separately so per-layer cost is not inflated by it.
 *
 * @param name Tensor name.
 * @return Block index, or -1 when the tensor belongs to no block.
 * @dg_internal
 * @version 2.13.1
 */
int block_index_of(const std::string& name) {
    static const std::regex blk("^blk\\.([0-9]+)\\.");
    std::smatch m;
    if (!std::regex_search(name, m, blk)) { return -1; }
    return std::stoi(m[1].str());
}

}  // namespace

/**
 * @brief Read a GGUF's shape from its metadata, without loading tensor data.
 * @param path Filesystem path to the `.gguf`.
 * @return The shape; `known == false` when unreadable or architecture-less.
 * @req REQ-INFER-019
 * @version 2.13.1
 */
GgufShape read_gguf_shape(const std::string& path) {
    GgufShape shape;

    // no_alloc: map the header, the KV metadata and the tensor index; read
    // none of the tensor data. A 10 GB model costs the same as a 1 GB one.
    gguf_init_params params{};
    params.no_alloc = true;
    params.ctx = nullptr;

    gguf_context* ctx = gguf_init_from_file(path.c_str(), params);
    if (ctx == nullptr) {
        return shape;  // known stays false — caller must fall back.
    }

    const int64_t arch_id = gguf_find_key(ctx, "general.architecture");
    if (arch_id < 0) {
        gguf_free(ctx);
        return shape;  // No architecture means no way to name the other keys.
    }
    shape.architecture = gguf_get_val_str(ctx, arch_id);

    shape.block_count =
        u32_or(ctx, shape.architecture + ".block_count", 0);
    shape.expert_count =
        u32_or(ctx, shape.architecture + ".expert_count", 0);

    // Sum the tensor index. Expert bytes are summed by MATCHING llama.cpp's
    // own expert pattern rather than by assuming experts are some fraction
    // of a layer — the difference between a measurement and a heuristic.
    const std::regex experts(kExpertTensorRegex);
    const int64_t n_tensors = gguf_get_n_tensors(ctx);
    for (int64_t i = 0; i < n_tensors; ++i) {
        const std::string name = gguf_get_tensor_name(ctx, i);
        const auto bytes = static_cast<uint64_t>(gguf_get_tensor_size(ctx, i));
        if (block_index_of(name) < 0) {
            shape.non_block_bytes += bytes;
            continue;
        }
        shape.block_bytes += bytes;
        if (std::regex_search(name, experts)) {
            shape.expert_bytes += bytes;
        }
    }

    gguf_free(ctx);

    // A file whose tensors carry no block prefix tells us nothing about
    // per-layer cost, so it is not a usable shape even though it parsed.
    shape.known = shape.block_count > 0 && shape.block_bytes > 0;
    return shape;
}

}  // namespace entropic
