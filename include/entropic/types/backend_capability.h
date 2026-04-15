// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file backend_capability.h
 * @brief Backend capability flags and metadata for architecture-agnostic queries.
 *
 * Backends declare what they support; the engine queries before using
 * optional features. A backend that does not support a capability returns
 * graceful errors (not crashes) if the feature is invoked anyway.
 *
 * @version 1.9.13
 */

#pragma once

#include <cstddef>
#include <string>

namespace entropic {

/**
 * @brief Capabilities that an inference backend may or may not support.
 *
 * The engine queries capabilities before using optional features.
 * A backend that does not support a capability returns graceful errors
 * (not crashes) if the feature is invoked anyway.
 *
 * @par Design rationale:
 * Enum rather than string tags for compile-time exhaustiveness checking.
 * New capabilities are appended (never reordered) for ABI stability.
 *
 * @version 1.9.13
 */
enum class BackendCapability : int {
    KV_CACHE = 0,             ///< KV cache state management (save/load/clear)
    HIDDEN_STATE = 1,         ///< Recurrent hidden state management (save/load/reset)
    STREAMING = 2,            ///< Streaming token-by-token generation
    RAW_COMPLETION = 3,       ///< Raw text completion without chat template
    GRAMMAR = 4,              ///< GBNF grammar-constrained generation
    LORA_ADAPTERS = 5,        ///< LoRA adapter hot-swapping (v1.9.2)
    MULTI_SEQUENCE = 6,       ///< Multiple concurrent sequences on one model instance
    TOKENIZER = 7,            ///< Token counting / tokenizer access
    LOG_PROBS = 8,            ///< Log-probability retrieval (v1.9.10)
    VISION = 9,               ///< Vision / multimodal input (v1.9.11)
    SPECULATIVE_DECODING = 10, ///< Speculative decoding compatibility
    PROMPT_CACHING = 11,      ///< Prompt cache prefix save/load (v1.8.3)
    _COUNT                    ///< Sentinel — must be last. Used for iteration/array sizing.
};

/**
 * @brief Backend metadata for introspection.
 *
 * Returned by InferenceBackend::info(). The engine uses this for
 * logging, diagnostics (v1.9.12), and routing decisions.
 *
 * @version 1.9.13
 */
struct BackendInfo {
    std::string name;              ///< Backend identifier (e.g. "llama.cpp", "axcl")
    std::string version;           ///< Backend version string
    std::string compute_device;    ///< "cuda", "vulkan", "cpu", "npu"
    std::string model_format;      ///< "gguf", "axmodel", "onnx", etc.

    /// @brief Architecture family of the loaded model.
    /// "transformer", "gdn", "mamba", "rwkv", "hybrid", "unknown"
    /// Populated after load(). Empty when COLD.
    /// @version 1.9.13
    std::string architecture;

    /// @brief Maximum context length.
    /// For transformers: fixed window from model metadata.
    /// For recurrent: -1 (theoretically unlimited, practically memory-bound).
    /// @version 1.9.13
    int max_context_length = 0;

    size_t vram_bytes = 0;         ///< VRAM consumed by loaded model (bytes). 0 if COLD.
    size_t ram_bytes = 0;          ///< RAM consumed by loaded model (bytes). 0 if COLD.
    size_t parameter_count = 0;    ///< Number of parameters (from model metadata).
    std::string quantization;      ///< Quantization type (e.g. "IQ3_XXS", "Q8_0", "fp16").
};

} // namespace entropic
