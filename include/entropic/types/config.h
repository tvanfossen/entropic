/**
 * @file config.h
 * @brief Configuration structs with defaults.
 *
 * Internal to the engine (C++ types). Cross-.so config is passed as
 * JSON strings via the C API. These structs are deserialized from JSON
 * inside the .so boundary.
 *
 * Structs use aggregate initialization with defaults. Validation is
 * separate — each struct has a standalone validate() function.
 *
 * @version 1.8.0
 */

#pragma once

#include <string>
#include <vector>
#include <optional>
#include <filesystem>

namespace entropic {

/**
 * @brief Model configuration for a single tier.
 *
 * Contains all parameters needed to load and configure a model, including
 * llama.cpp pass-through fields for KV cache, batching, threading, and
 * attention.
 *
 * @version 1.8.0
 */
struct ModelConfig {
    std::filesystem::path path;              ///< Resolved model file path
    std::string adapter = "qwen35";          ///< Chat adapter name
    int context_length = 16384;              ///< Context window size (512–131072)
    int gpu_layers = -1;                     ///< GPU offload layers (-1 = all)
    bool keep_warm = false;                  ///< Pre-warm model at startup
    bool use_mlock = true;                   ///< Lock model in system RAM

    /* ── llama.cpp pass-through ────────────────────────── */
    int reasoning_budget = -1;               ///< Think token budget (-1 = unlimited)
    std::string cache_type_k = "f16";        ///< KV cache key quantization type
    std::string cache_type_v = "f16";        ///< KV cache value quantization type
    int n_batch = 512;                       ///< Batch size for prompt processing
    int n_threads = 0;                       ///< CPU threads (0 = auto-detect)
    std::string tensor_split;                ///< Multi-GPU tensor split ratios (empty = single GPU)
    bool flash_attn = true;                  ///< Enable flash attention

    /* ── Tool filtering ────────────────────────────────── */
    std::optional<std::vector<std::string>> allowed_tools; ///< Tool whitelist (nullopt = all)
};

/**
 * @brief Generation parameters for a single inference call.
 * @version 1.8.0
 */
struct GenerationParams {
    float temperature = 0.7f;                ///< Sampling temperature
    float top_p = 0.9f;                      ///< Nucleus sampling threshold
    int top_k = 40;                          ///< Top-K sampling
    float repeat_penalty = 1.1f;             ///< Repetition penalty
    int max_tokens = 4096;                   ///< Maximum tokens to generate
    int reasoning_budget = -1;               ///< Per-call think budget override (-1 = use model default)
    std::string grammar;                     ///< GBNF grammar string (empty = unconstrained)
};

} // namespace entropic
