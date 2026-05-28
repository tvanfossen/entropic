// SPDX-License-Identifier: Apache-2.0
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
 * @version 1.9.4
 */

#pragma once

#include <entropic/types/enums.h>

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include <unordered_map>

namespace entropic {

/**
 * @brief MCP tool access level for per-identity authorization.
 *
 * Two ordered levels: READ < WRITE. WRITE implies READ.
 * Default for ungranted keys is NONE (no access).
 * Default for new tools is WRITE (safe default — tools must opt into READ).
 *
 * @version 1.9.4
 */
enum class MCPAccessLevel : uint8_t {
    NONE  = 0,  ///< No access (default for ungranted keys)
    READ  = 1,  ///< Read-only operations (e.g., read_file, list_directory)
    WRITE = 2,  ///< Read + write operations (e.g., write_file, execute)
};

/**
 * @brief A single authorized MCP key with access level.
 * @version 1.9.4
 */
struct MCPKey {
    std::string tool_pattern;  ///< Tool pattern (e.g., "filesystem.*", "git.status")
    MCPAccessLevel level;      ///< Granted access level (READ or WRITE)
};

/**
 * @brief Convert MCPAccessLevel to string representation.
 * @param level Access level.
 * @return Static string: "NONE", "READ", or "WRITE".
 * @utility
 * @version 1.9.4
 */
const char* mcp_access_level_name(MCPAccessLevel level);

/**
 * @brief Parse MCPAccessLevel from string.
 * @param name String: "NONE", "READ", or "WRITE" (case-sensitive).
 * @param[out] out Parsed access level.
 * @return true if parsed successfully, false on unknown string.
 * @utility
 * @version 1.9.4
 */
bool parse_mcp_access_level(const std::string& name, MCPAccessLevel& out);

/**
 * @brief Metadata for a registered grammar.
 *
 * Registry entries carry metadata alongside the GBNF content string.
 * Used by GrammarRegistry for introspection and validation status.
 *
 * @version 1.9.3
 */
struct GrammarEntry {
    std::string key;            ///< Unique registry key (e.g., "compactor", "chess_executor")
    std::string gbnf_content;   ///< Raw GBNF grammar string
    std::string source;         ///< Origin: "bundled", "file", "runtime", "dynamic"
    bool validated = false;     ///< true if grammar has passed validation
    std::string error;          ///< Non-empty if validation failed
};

/**
 * @brief C++ enum class for model VRAM lifecycle states.
 *
 * Maps 1:1 to C entropic_model_state_t for cross-boundary use.
 * Internal C++ code uses this typed enum; C boundary uses the C enum.
 *
 * @version 1.8.2
 */
enum class ModelState : int {
    COLD   = ENTROPIC_MODEL_STATE_COLD,    ///< On disk only, no RAM consumed
    WARM   = ENTROPIC_MODEL_STATE_WARM,    ///< mmap'd + mlock'd in RAM
    ACTIVE = ENTROPIC_MODEL_STATE_ACTIVE,  ///< GPU layers loaded, full speed
};

/**
 * @brief LoRA adapter lifecycle state.
 *
 * Mirrors ModelState but for LoRA adapters. An adapter must be HOT
 * for its weights to influence generation. Multiple adapters can be
 * WARM simultaneously (loaded in RAM), but only one can be HOT per
 * inference context.
 *
 * @version 1.9.2
 */
enum class AdapterState : int {
    COLD = 0,  ///< Not loaded. No resources consumed.
    WARM = 1,  ///< Loaded in RAM via llama_adapter_lora_init(). Ready to activate.
    HOT  = 2   ///< Active on context via llama_set_adapter_lora(). Influencing generation.
};

/**
 * @brief Metadata for a loaded LoRA adapter.
 *
 * Used for introspection and routing decisions. Returned by
 * AdapterManager::info() and the entropic_adapter_info() C API.
 *
 * @version 1.9.2
 */
struct AdapterInfo {
    std::string name;                    ///< Unique adapter identifier
    std::filesystem::path path;          ///< Resolved path to .gguf adapter file
    AdapterState state = AdapterState::COLD;  ///< Current lifecycle state
    float scale = 1.0f;                 ///< LoRA scaling factor (alpha/rank)
    std::string tier_name;              ///< Tier this adapter is assigned to (empty = unassigned)
    std::string base_model_path;        ///< Path of the base model this adapter targets
    size_t ram_bytes = 0;               ///< RAM consumption when WARM/HOT (0 if COLD)

    /// @brief Adapter-specific metadata for routing decisions.
    std::unordered_map<std::string, std::string> metadata;
};

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

    /// @brief Physical micro-batch size for prompt processing (gh#23 MVP item 5).
    /// llama.cpp's `cparams.n_ubatch`. Decoupled from `n_batch` since
    /// llama.cpp v0.4 — `n_batch` is the LOGICAL batch (max tokens
    /// queued per `llama_decode` call) and `n_ubatch` is the PHYSICAL
    /// chunk the kernels actually process. Smaller `n_ubatch` reduces
    /// peak GPU memory for the same `n_batch`. `0` (default) means
    /// "match `n_batch`" — preserves pre-v2.3.17 behavior bit-for-bit
    /// since llama.cpp's default in that case is `min(n_batch, default)`.
    /// Typical productive values: 128, 256, 512 (== n_batch).
    /// @version 2.3.17
    int n_ubatch = 0;

    int n_threads = 0;                       ///< CPU threads (0 = auto-detect)
    std::string tensor_split;                ///< Multi-GPU tensor split ratios (empty = single GPU)

    /// @brief Multi-GPU split mode for model load (gh#23 MVP item 6).
    /// Maps to llama.cpp's `enum llama_split_mode`. Accepted values:
    ///   - `""` (default) — keep llama.cpp's default (`LAYER`).
    ///   - `"none"`  — single GPU, no split. Use with `main_gpu`.
    ///   - `"layer"` — split layers + KV across GPUs (llama.cpp default).
    ///   - `"row"`   — split layers + KV with tensor parallelism.
    /// Unrecognized values fall back to the default with a logged warning.
    /// Empty (default) preserves pre-v2.3.18 model load bit-for-bit.
    /// @version 2.3.18
    std::string split_mode;

    /// @brief Primary GPU index for model load (gh#23 MVP item 7).
    /// llama.cpp's `mparams.main_gpu`. Effective when `split_mode ==
    /// "none"` (single-GPU pinning) or `"row"` (small tensors go to
    /// this GPU). Ignored when `split_mode == "layer"`. `0` (default)
    /// preserves pre-v2.3.19 behavior bit-for-bit.
    /// @version 2.3.19
    int main_gpu = 0;

    /// @brief Offload KQV ops (incl. KV cache) to the GPU (gh#23 MVP item 8).
    /// llama.cpp's `cparams.offload_kqv`. `true` (default) matches
    /// llama.cpp's default — KQV runs on GPU for max throughput.
    /// Set `false` to keep KQV on the CPU side; saves VRAM at a
    /// throughput cost. Useful for tight-VRAM single-GPU setups.
    /// @version 2.3.20
    bool offload_kqv = true;

    /// @brief RoPE base frequency override (gh#23 MVP item 9).
    /// llama.cpp's `cparams.rope_freq_base`. `0.0` (default) takes
    /// the model's trained value — preserves pre-v2.3.21 behavior
    /// bit-for-bit. Positive overrides typically range 10000–10000000;
    /// raising it stretches the RoPE period (extends effective context
    /// at a quality cost). Pair with `rope_freq_scale` for YaRN-style
    /// context-extension setups.
    /// @version 2.3.21
    float rope_freq_base = 0.0f;

    /// @brief RoPE frequency scaling factor (gh#23 MVP item 10).
    /// llama.cpp's `cparams.rope_freq_scale`. `0.0` (default) takes
    /// the model's trained value — preserves pre-v2.3.22 behavior
    /// bit-for-bit. Values in `(0, 1)` shrink the effective context
    /// (denser RoPE positions); values `> 1` stretch it. Typical
    /// YaRN-style extension uses values like `0.5` (2× context).
    /// Pairs with `rope_freq_base`.
    /// @version 2.3.22
    float rope_freq_scale = 0.0f;

    /// @brief Max parallel sequences per context (gh#23 MVP item 11).
    /// llama.cpp's `cparams.n_seq_max`. `1` (default) matches
    /// llama.cpp's default — single-sequence context, bit-identical
    /// pre-v2.3.23 behavior. Raising this enables KV-cache slot reuse
    /// across multiple concurrent generations (e.g. speculative
    /// rejection batches, batched-server scenarios). Effective max is
    /// `LLAMA_MAX_SEQ`; consult llama.cpp for the current ceiling.
    /// @version 2.3.23
    int n_parallel = 1;
    bool flash_attn = true;                  ///< Enable flash attention

    /* ── Tool filtering ────────────────────────────────── */
    std::optional<std::vector<std::string>> allowed_tools; ///< Tool whitelist (nullopt = all)

    /* ── Vision / multimodal (v1.9.11) ────────────────── */

    /// @brief Vision projector GGUF path. When non-empty, the backend
    /// loads an mtmd_context alongside the base model for multimodal
    /// inference. Empty (default) = text-only model.
    /// @version 1.9.11
    std::filesystem::path mmproj_path;

    /* ── Model format (v1.9.13) ───────────────────────── */

    /// @brief Expected model format.
    /// "gguf" (default), "axmodel", "onnx", or empty (auto-detect).
    /// The backend validates that the file matches the expected format
    /// during load(). Mismatch returns ENTROPIC_ERROR_LOAD_FAILED with
    /// a diagnostic message identifying the actual format.
    /// @version 1.9.13
    std::string model_format = "gguf";
};

/**
 * @brief Prompt caching configuration.
 *
 * Controls host-memory KV cache prefix storage for system prompts.
 * Setting enabled=false or max_bytes=0 both disable caching.
 *
 * @par YAML key: inference.prompt_cache
 * @version 1.8.3
 */
struct PromptCacheConfig {
    size_t max_bytes = 536870912;  ///< Maximum cache RAM (512 MB default)
    bool enabled = true;           ///< Master switch (false = no caching)
    bool log_hits = true;          ///< Log cache hit/miss at INFO level
};

/**
 * @brief Named GPU resource profile for controlling inference hardware knobs.
 *
 * Profiles are applied at the LlamaCppBackend level before each generation
 * call. They control batch size and thread counts without requiring a model
 * reload. Profile switching is sub-millisecond.
 *
 * @par Bundled profiles
 * Four profiles ship with the engine: maximum, balanced, background, minimal.
 * Consumers can register custom profiles at runtime via ProfileRegistry.
 *
 * @version 1.9.7
 */
struct GPUResourceProfile {
    std::string name;              ///< Profile name ("maximum", "balanced", "background", "minimal")
    int n_batch = 512;             ///< Batch size for prompt processing (1-2048)
    int n_threads = 0;             ///< CPU threads for generation (0 = auto-detect)
    int n_threads_batch = 0;       ///< CPU threads for batch processing (0 = use n_threads)
    std::string description;       ///< Human-readable description
};

/**
 * @brief Generation parameters for a single inference call.
 * @version 2.3.16 — added logit_bias (gh#23 MVP item 4)
 */
struct GenerationParams {
    float temperature = 0.7f;                ///< Sampling temperature
    float top_p = 0.9f;                      ///< Nucleus sampling threshold
    int top_k = 40;                          ///< Top-K sampling
    float repeat_penalty = 1.1f;             ///< Repetition penalty

    /// @brief Min-p nucleus sampling threshold (gh#23 MVP item 1).
    /// Filters tokens whose probability is below `min_p * P(top_token)`.
    /// `0.0` (default) disables — preserves pre-v2.3.10 behavior bit-for-bit.
    /// Typical productive range: 0.01–0.2. Values >= 1.0 filter every
    /// non-top token (degenerate but not crash-inducing; the dist sampler
    /// still selects the single survivor).
    /// @version 2.3.10
    float min_p = 0.0f;

    /// @brief Presence-penalty term in llama.cpp's penalties sampler (gh#23 MVP item 2).
    /// Subtracts a constant from any token that has appeared at least
    /// once in the recent window. `0.0` (default) disables — preserves
    /// pre-v2.3.14 chain bit-for-bit. Typical range: 0.0–2.0.
    /// @version 2.3.14
    float presence_penalty = 0.0f;

    /// @brief Per-token logit bias map (gh#23 MVP item 4).
    /// Maps token id → additive bias (in logit-space). Applied at the
    /// start of the sampler chain (before penalties), so it shapes
    /// the post-softmax distribution that every downstream filter
    /// (top-k, top-p, min-p, etc.) sees. Common uses:
    ///   - Suppress a token: `bias = -INFINITY` (or a large negative
    ///     value like -100 if -INFINITY doesn't survive JSON).
    ///   - Force a token: `bias = +INFINITY` (or large positive).
    ///   - Subtle nudges: `bias = ±1.0..±5.0`.
    /// Empty map (default) disables the stage entirely — preserves
    /// pre-v2.3.16 chain shape bit-for-bit.
    /// @version 2.3.16
    std::unordered_map<int32_t, float> logit_bias;

    /// @brief Frequency-penalty term in llama.cpp's penalties sampler (gh#23 MVP item 3).
    /// Subtracts a per-occurrence linear amount from any token that
    /// has appeared in the recent window — penalizes by COUNT rather
    /// than presence. `0.0` (default) disables — preserves pre-v2.3.15
    /// chain bit-for-bit. Typical range: 0.0–2.0. Pairs with
    /// `repeat_penalty` (multiplicative) and `presence_penalty`
    /// (per-presence constant); all three run in one
    /// `llama_sampler_init_penalties` call. Any non-default value
    /// here activates the penalties stage even when
    /// `repeat_penalty == 1.0` and `presence_penalty == 0.0`.
    /// @version 2.3.15
    float frequency_penalty = 0.0f;

    int max_tokens = 4096;                   ///< Maximum tokens to generate

    /// @brief RNG seed for reproducible sampling. -1 = random (default).
    /// Maps to LLAMA_DEFAULT_SEED when negative. (P2-14)
    /// @version 2.0.6-rc16
    int seed = -1;
    int reasoning_budget = -1;               ///< Per-call think budget override (-1 = unlimited)
    bool enable_thinking = true;             ///< Enable <think> blocks (false if reasoning_budget == 0)
    std::string grammar;                     ///< GBNF grammar string (empty = unconstrained)
    /// @brief Grammar registry key. Resolved to GBNF content by orchestrator
    /// before passing to the backend. If both grammar and grammar_key are set,
    /// grammar (raw string) takes precedence.
    /// @version 1.9.3
    std::string grammar_key;
    std::vector<std::string> stop;           ///< Stop sequences
    int logprobs = 0;                        ///< Top log-probs per token (0 = disabled)

    /* ── v1.9.7: Time cap + profile fields ────────────── */

    /// @brief Wall-clock time cap in milliseconds. Generation is cancelled
    /// if this limit is reached. 0 = no time limit (default).
    /// @version 1.9.7
    int time_limit_ms = 0;

    /// @brief GPU resource profile name. Resolved to GPUResourceProfile
    /// by the orchestrator before passing to the backend. Empty string
    /// means use the "balanced" profile.
    /// @version 1.9.7
    std::string profile;

    /// @brief Enable throughput-based max_tokens auto-adaptation.
    /// When true, the orchestrator may reduce max_tokens to fit within
    /// time_limit_ms based on recent throughput measurements.
    /// Ignored if time_limit_ms == 0.
    /// @version 1.9.7
    bool auto_adapt = true;

    /// @brief Target time usage fraction for auto-adaptation.
    /// 0.9 means "use at most 90% of time_limit_ms for generation".
    /// @version 1.9.7
    float adapt_headroom = 0.9f;
};

/**
 * @brief Tier-specific model configuration.
 *
 * Extends ModelConfig with identity resolution and tier-specific
 * behavioral fields. Identity prompt resolution:
 *   absent/nullopt → bundled default (ships with entropic-engine)
 *   disabled=true  → disabled entirely
 *   path set       → custom file (must exist, validated at load)
 *
 * @version 1.9.2 — added adapter_path, adapter_scale
 */
struct TierConfig : ModelConfig {
    std::optional<std::filesystem::path> identity;  ///< Identity prompt path (nullopt = bundled)
    bool identity_disabled = false;                 ///< true if identity explicitly disabled
    std::optional<std::filesystem::path> grammar;   ///< Grammar file path
    std::optional<std::string> auto_chain;           ///< Target tier name (nullopt = defer to identity)
    std::optional<bool> routable;                   ///< None = defer to identity frontmatter

    /// @brief Optional path to LoRA adapter .gguf file.
    /// If set, orchestrator loads and activates on tier transition.
    /// @version 1.9.2
    std::optional<std::filesystem::path> adapter_path;

    /// @brief LoRA scaling factor (0.0–2.0, default 1.0).
    /// @version 1.9.2
    float adapter_scale = 1.0f;

    /// @brief Declared tier capabilities (gh#41).
    ///
    /// Free-form lowercase strings — canonical values are "text" and
    /// "vision". Missing / empty in YAML resolves to `{"text"}` at
    /// config-load time so every pre-v2.1.8 tier config stays valid
    /// without modification. The orchestrator inspects this set when
    /// it sees a message with image content_parts; if no configured
    /// tier carries `"vision"`, the run is rejected with
    /// `ENTROPIC_ERROR_NO_VISION_TIER`.
    ///
    /// Order is not significant. Duplicates are tolerated but
    /// idiomatic configs deduplicate.
    ///
    /// @version 2.1.8
    std::vector<std::string> capabilities;

    /// @brief Per-tier sampler temperature from identity frontmatter (gh#82).
    /// nullopt = not configured; the orchestrator leaves the incoming
    /// GenerationParams temperature in place. When set, applied as the
    /// tier baseline (an explicit per-call override still wins).
    /// @version 2.4.4
    std::optional<float> temperature;

    /// @brief Per-tier max output tokens from identity frontmatter (gh#82).
    /// nullopt = not configured; same precedence as `temperature`.
    /// @version 2.4.4
    std::optional<int> max_output_tokens;

    /// @brief Per-tier sampler knobs from identity frontmatter (gh#85).
    /// nullopt = not configured; applied as the tier baseline (an
    /// explicit per-call param override wins). Same precedence policy
    /// as `temperature`.
    /// @version 2.5.3
    std::optional<float> top_p;
    std::optional<int>   top_k;             ///< gh#85
    std::optional<float> min_p;             ///< gh#85
    std::optional<float> presence_penalty;  ///< gh#85
    std::optional<float> frequency_penalty; ///< gh#85

    /// @brief Per-tier repeat_penalty + enable_thinking from identity
    /// frontmatter (gh#86). nullopt = not configured; same precedence
    /// as the other sampler knobs. enable_thinking flows to the GGUF
    /// chat template via GenerationParams.enable_thinking.
    /// @version 2.5.4
    std::optional<float> repeat_penalty;
    std::optional<bool>  enable_thinking;   ///< gh#86

    /**
     * @brief Return true if this tier declares the named capability.
     * @param name Lowercase capability name (e.g., "vision").
     * @return true if the capabilities vector contains `name`.
     * @utility
     * @version 2.1.8
     */
    bool has_capability(const std::string& name) const {
        for (const auto& c : capabilities) {
            if (c == name) { return true; }
        }
        return false;
    }

    /**
     * @brief Get a named parameter derived from tier config fields.
     *
     * Encapsulates policy mappings (e.g., auto_chain presence implies
     * no explicit_completion). Keeps this logic on the data struct
     * instead of in facade callbacks.
     *
     * @param param_name Parameter name (e.g., "explicit_completion").
     * @return Value string, or empty if unknown.
     * @utility
     * @version 2.0.1
     */
    std::string get_param(const std::string& param_name) const {
        if (param_name == "explicit_completion") {
            return auto_chain.has_value() ? "false" : "true";
        }
        return "";
    }
};

/**
 * @brief Configuration for all models (tiers + router).
 * @version 1.8.1
 */
struct ModelsConfig {
    std::unordered_map<std::string, TierConfig> tiers; ///< Tier name → config
    std::optional<ModelConfig> router;                  ///< Router model (separate from tiers)
    std::string default_tier = "lead";                  ///< Default tier name

    /**
     * @brief Find tier name by model path.
     *
     * Iterates tiers to find one whose resolved path matches.
     * Returns empty string if no tier uses the given path.
     *
     * @param model_path Model file path to match.
     * @return Tier name, or empty string if not found.
     * @utility
     * @version 2.0.1
     */
    std::string find_tier_by_path(
        const std::filesystem::path& model_path) const {
        for (const auto& [name, tier] : tiers) {
            if (tier.path == model_path) { return name; }
        }
        return "";
    }
};

/**
 * @brief Configuration for model routing.
 * @deprecated Since v2.1.0 — router model removed; use a dedicated identity
 *             as the lead/router instead. This struct is retained for ABI
 *             compatibility only and will be removed in v2.2.0.
 * @version 2.1.0
 */
struct RoutingConfig {
    bool enabled = false;                                          ///< Enable routing
    std::string fallback_tier = "lead";                            ///< Fallback when routing fails
    std::optional<std::string> classification_prompt;               ///< Custom prompt (nullopt = auto)
    std::unordered_map<std::string, std::string> tier_map;          ///< Classification → tier mapping
    std::unordered_map<std::string, std::vector<std::string>> handoff_rules; ///< Tier handoff rules
};

/**
 * @brief Tool permission configuration.
 * @version 1.8.1
 */
struct PermissionsConfig {
    std::vector<std::string> allow;   ///< Allowed tool patterns (glob)
    std::vector<std::string> deny;    ///< Denied tool patterns (glob)
    bool auto_approve = false;        ///< Skip confirmation prompts
};

/**
 * @brief Filesystem MCP server configuration.
 * @version 1.8.1
 */
struct FilesystemConfig {
    bool diagnostics_on_edit = true;   ///< Proactive diagnostics on edit/write
    bool fail_on_errors = true;        ///< Rollback edit if it introduces errors
    float diagnostics_timeout = 1.0f;  ///< Diagnostics timeout (0.1–5.0)
    bool allow_outside_root = false;   ///< Allow file ops outside workspace root
    std::optional<int> max_read_bytes; ///< Max file read size (nullopt = derive from context)
    float max_read_context_pct = 0.25f; ///< Max context % for single file read
};

/**
 * @brief External MCP server configuration (Entropic-as-server).
 * @version 1.8.1
 */
struct ExternalMCPConfig {
    bool enabled = false;                                ///< Enable external MCP
    std::optional<std::filesystem::path> socket_path;    ///< Socket path (nullopt = derived)
    int rate_limit = 10;                                 ///< Requests per minute (1–100)
};

/**
 * @brief Reconnection policy configuration for external MCP servers.
 * @version 1.8.7
 */
struct ReconnectConfig {
    uint32_t base_delay_ms = 1000;   ///< Initial retry delay
    uint32_t max_delay_ms = 60000;   ///< Maximum retry delay cap
    uint32_t max_retries = 5;        ///< Max attempts (0 = infinite)
    double backoff_factor = 2.0;     ///< Exponential backoff multiplier
};

/**
 * @brief Configuration for a single external MCP server entry.
 * @version 1.8.7
 */
struct ExternalServerEntry {
    std::string command;                             ///< Stdio command (empty for SSE)
    std::vector<std::string> args;                   ///< Stdio command arguments
    std::unordered_map<std::string, std::string> env; ///< Stdio environment variables
    std::string url;                                 ///< SSE endpoint URL (empty for stdio)
};

/**
 * @brief MCP server configuration.
 * @version 1.8.7
 */
struct MCPConfig {
    bool enable_entropic = true;     ///< Enable entropic internal server (handoff, delegate, pipeline)
    bool enable_filesystem = true;   ///< Enable filesystem server
    bool enable_bash = true;         ///< Enable bash server
    bool enable_git = true;          ///< Enable git server
    bool enable_diagnostics = true;  ///< Enable diagnostics server
    bool enable_web = true;          ///< Enable web server
    FilesystemConfig filesystem;     ///< Filesystem server config
    ExternalMCPConfig external;      ///< External MCP server config (Entropic-as-server)
    int server_timeout_seconds = 30; ///< Server timeout (5–300)
    std::string working_dir;         ///< Server working directory (empty = CWD) (v2.0.4)

    /* ── v1.8.7: External MCP client settings ──────────── */
    std::unordered_map<std::string, ExternalServerEntry> external_servers; ///< Named external servers
    ReconnectConfig reconnect;                   ///< Reconnection backoff policy
    uint32_t health_check_interval_ms = 0;       ///< Ping interval (0 = disabled)
    uint32_t tool_call_timeout_ms = 30000;       ///< Per-call timeout for external tools
};

/**
 * @brief Audit log configuration within StorageConfig.
 *
 * Controls JSONL audit logging of MCP tool calls. When enabled,
 * every tool execution is recorded to audit.jsonl alongside the
 * session persistence files.
 *
 * @version 1.9.5
 */
struct AuditLogConfig {
    std::filesystem::path log_dir;       ///< Directory for audit log files
    std::string session_id;              ///< UUID for this session
    bool enabled = true;                 ///< Master toggle for audit logging
    size_t flush_interval_entries = 10;  ///< Flush every N entries (0 = every entry)
    size_t max_file_size = 0;            ///< Rotation size in bytes (0 = unlimited)
    size_t max_files = 5;                ///< Max rotated files to keep
};

/**
 * @brief Storage backend configuration.
 * @version 1.9.5
 */
struct StorageConfig {
    bool enabled = true;                           ///< Enable storage backend
    std::filesystem::path db_path;                 ///< SQLite database path (derived from config_dir)
    size_t log_max_file_size = 10 * 1024 * 1024;   ///< Max log file size before rotation (10MB)
    size_t log_max_files = 3;                       ///< Max rotated log files to keep
    AuditLogConfig audit_log;                       ///< Audit log settings (v1.9.5)
};

/**
 * @brief Auto-compaction configuration.
 * @version 1.8.1
 */
struct CompactionConfig {
    bool enabled = true;                       ///< Enable auto-compaction
    float threshold_percent = 0.75f;           ///< Compaction trigger (0.5–0.99)
    int preserve_recent_turns = 2;             ///< Turns to preserve (1–10)
    int summary_max_tokens = 1500;             ///< Summary max tokens (500–4000)
    bool notify_user = true;                   ///< Notify user on compaction
    bool save_full_history = true;             ///< Save full history before compaction
    int tool_result_ttl = 10;                  ///< Tool result TTL in turns (>= 1; v2.1.3 #6: gated on fill, no upper bound)
    float warning_threshold_percent = 0.6f;    ///< Warning trigger (0.3–0.9)
};

/**
 * @brief Generation parameters configuration (top-level defaults).
 * @version 1.8.1
 */
struct GenerationConfig {
    int max_tokens = 4096;            ///< Default max tokens (64–32768)
    float default_temperature = 0.7f; ///< Default temperature (0.0–2.0)
    float default_top_p = 0.9f;       ///< Default top_p (0.0–1.0)

    /// @brief gh#80 (v2.5.0) thinking-budget mode: "off" (default),
    /// "tokens", or "wall_clock". Stored as a string here to keep
    /// types/config.h free of the core engine_types BudgetMode enum;
    /// the facade's build_loop_config maps it. Unknown values resolve
    /// to "off" with a warning.
    /// @version 2.5.0
    std::string budget_mode = "off";

    /// @brief gh#80 (v2.5.0) budget ceiling: generated tokens
    /// (budget_mode "tokens") or wall-clock seconds (budget_mode
    /// "wall_clock") of tool-call-free generation before the engine
    /// nudges-then-hard-cuts. Must be > 0 to engage.
    /// @version 2.5.0
    int budget_limit = 0;
};

/**
 * @brief Configuration for a single LSP server.
 * @version 1.8.1
 */
struct LSPServerConfig {
    std::string command;                ///< Server command
    std::vector<std::string> args;     ///< Command arguments
    std::vector<std::string> extensions; ///< File extensions
};

/**
 * @brief LSP integration configuration.
 * @version 1.8.1
 */
struct LSPConfig {
    bool enabled = true;          ///< Enable LSP integration
    bool python_enabled = true;   ///< Enable Python LSP
    bool c_enabled = true;        ///< Enable C/C++ LSP
    std::unordered_map<std::string, LSPServerConfig> servers; ///< Custom server overrides
};

/**
 * @brief Constitutional validation pipeline configuration.
 *
 * Controls the post-generation validation hook that critiques engine
 * output against constitutional rules. Default disabled — doubles
 * inference cost per generation when active.
 *
 * @par skip_tiers
 * Tiers listed here are exempt from validation. Defaults to {"lead"}
 * because lead's output streams directly to the user — the hook fires
 * after the stream completes, so modifying lead output has no effect
 * and wastes inference. Validation applies to delegated child tiers.
 *
 * @version 2.0.7
 */
struct ConstitutionalValidationConfig {
    bool enabled = false;             ///< Global enable/disable (default OFF)
    int max_revisions = 2;            ///< Max re-generation attempts (0 = critique only)
    int max_critique_tokens = 1024;   ///< Token budget for critique generation
    float temperature = 0.0f;         ///< Critique generation temperature
    bool enable_thinking = false;     ///< Enable think-blocks for critique (default OFF)
    int priority = 100;               ///< Hook priority (higher = later)
    std::string grammar_key = "constitutional_critique";  ///< Grammar registry key
    /// Tiers exempt from validation (default: lead — streams before hook fires)
    std::vector<std::string> skip_tiers = {"lead"};
    /// @brief Tier to route critique generation on. Empty = use the
    /// active tier (previous behavior). Pointing this at a smaller tier
    /// (e.g. "eng") avoids burning 35B primary inference on grammar-
    /// constrained critique work. (E4, 2.0.6-rc17)
    std::string critique_tier;
};

/**
 * @brief Speculative decoding configuration (v2.1.11, gh#36).
 *
 * Off by default. When `enabled` is true and a `draft_model` is set,
 * the engine loads the draft into the SecondaryModelLoader's `"draft"`
 * slot at init and routes main-tier generation through
 * `LlamaCppBackend::do_generate_speculative`.
 *
 * Placement axis: `draft_n_gpu_layers` selects CPU (`0`), full GPU
 * (`-1`), or hybrid partial offload (`>0` and `< model_layers`). The
 * default of `0` puts the draft on CPU so the GPU stays saturated on
 * the verifier — minimal VRAM cost.
 *
 * @version 2.1.11
 */
/**
 * @brief Build the v2.1.11 default ModelConfig for a speculative
 *        draft model.
 *
 * Differs from the standard tier defaults in three places that the
 * kernel currently depends on:
 *  - `gpu_layers = 0` — CPU-resident draft (zero VRAM on top of
 *    primary). Override to -1 for full GPU when VRAM allows.
 *  - `flash_attn = false` — required for partial seq_rm support on
 *    MoE / GQA architectures (the kernel rolls back draft KV after
 *    each speculative round). Override to true ONLY if your model
 *    + backend can do partial seq_rm with flash attn enabled.
 *  - `context_length = 8192` — modest; speculative drafts are
 *    typically 4–32 tokens per round, no need for a wide window.
 *  - `n_threads = 4` — works on most laptops; bump to match physical
 *    core count for higher CPU draft throughput.
 *
 * Every other field comes from `ModelConfig`'s standard defaults.
 *
 * @utility
 * @version 2.1.11
 */
inline ModelConfig make_default_draft_model_config() {
    ModelConfig cfg;
    cfg.gpu_layers = 0;
    cfg.flash_attn = false;
    cfg.context_length = 8192;
    cfg.n_threads = 4;
    return cfg;
}

/**
 * @brief Speculative-decoding configuration (`inference.speculative.*`).
 *
 * @par Architecture compatibility (v2.1.11 pin `253ba110b`)
 *
 * The orchestrator's `check_speculative_compat()` refuses the
 * pairing when the target model is recurrent OR hybrid at the
 * pinned llama.cpp commit. Among bundled primaries today, that
 * leaves a SINGLE workable family:
 *
 * | Bundled key      | llama.cpp arch       | Speculative? |
 * |------------------|----------------------|--------------|
 * | qwen3_5_0_8b     | QWEN35 (hybrid SSM)  | refused      |
 * | qwen3_5_2b       | QWEN35 (hybrid SSM)  | refused      |
 * | qwen3_5_4b       | QWEN35 (hybrid SSM)  | refused      |
 * | qwen3_5_9b       | QWEN35 (hybrid SSM)  | refused      |
 * | primary (3.5-A3B)| QWEN35MOE (hybrid)   | refused      |
 * | qwen3_6_a3b      | QWEN35MOE (hybrid)   | refused      |
 * | nemotron3_nano_4b| NEMOTRON_H (hybrid)  | refused      |
 * | gemma4_a4b       | GEMMA4 (pure xformer)| **OK**       |
 * | gemma4_e4b       | GEMMA4 (pure xformer)| **OK**       |
 * | gemma4_e2b       | GEMMA4 (pure xformer)| **OK**       |
 *
 * Bit-identical correctness was verified empirically on the
 * Gemma 4 family in Session 5 (proposal Implementation Log,
 * Gate A). Hybrid SSM targets produce divergent KV state across
 * upstream's split-prefill scheme — the issue is structural to
 * `common_speculative_*` at this pin, not entropic-side. Consumers
 * pairing a non-Gemma primary with a Gemma draft (or vice versa)
 * will also be refused, since the gate looks at the TARGET arch.
 *
 * @par Recommended pairings (bundled)
 *   - target=`gemma4_e4b` + draft=`gemma4_e2b` (CPU): bit-identical,
 *     measurable speedup on long generations.
 *   - target=`gemma4_a4b` + draft=`gemma4_e2b` (CPU): more aggressive
 *     verifier; needs ~16 GB VRAM at modest context.
 *
 * Future llama.cpp pins that fix the cross-ubatch SSM state issue
 * (or alternate non-hybrid Qwen/Llama arches added to the bundled
 * registry) will widen this set without code change — the gate is
 * data-driven via `llama_model_is_hybrid` / `llama_model_is_recurrent`.
 *
 * @version 2.1.11
 */
struct SpeculativeConfig {
    bool enabled = false;                  ///< Master switch (off by default)
    int n_draft = 16;                      ///< Window size (proposed tokens)

    /**
     * @brief Full ModelConfig for the draft model.
     *
     * Mirrors how tier configs are structured: every llama.cpp knob
     * (gpu_layers, n_threads, n_batch, flash_attn, context_length,
     * use_mlock, cache_type_k/v, tensor_split, ...) is
     * consumer-tunable from YAML via
     * `inference.speculative.draft.<field>`. `path` accepts a
     * bundled-model registry key OR a literal filesystem path;
     * resolved at config-parse time by `BundledModels::resolve()`.
     *
     * Defaults are kernel-aware — see
     * `make_default_draft_model_config()`.
     */
    ModelConfig draft = make_default_draft_model_config();
};

/**
 * @brief Inference-side configuration knobs (v2.1.11).
 *
 * Sub-tree mirroring the proposal's `inference.*` YAML namespace.
 * Currently holds speculative decoding only; future inference-level
 * settings land here rather than spreading across ParsedConfig.
 *
 * @version 2.1.11
 */
struct InferenceConfig {
    SpeculativeConfig speculative;         ///< Speculative decoding (gh#36)
};

/**
 * @brief Full parsed configuration.
 *
 * Aggregates all config sections. C++ equivalent of Python's
 * LibraryConfig (engine-only fields, no TUI).
 *
 * @version 1.8.1
 */
struct ParsedConfig {
    ModelsConfig models;              ///< Tiers + router
    RoutingConfig routing;            ///< Routing rules
    GenerationConfig generation;      ///< Default generation params
    PermissionsConfig permissions;    ///< Tool permissions
    MCPConfig mcp;                    ///< MCP server settings
    CompactionConfig compaction;      ///< Auto-compaction settings
    LSPConfig lsp;                    ///< LSP integration
    PromptCacheConfig prompt_cache;   ///< Prompt KV cache settings
    StorageConfig storage;            ///< Storage backend settings (v1.8.8)
    std::string log_level = "INFO";   ///< Log level string

    /// Constitution: nullopt = bundled default, disabled = explicit false
    std::optional<std::filesystem::path> constitution;
    bool constitution_disabled = false; ///< true if constitution explicitly disabled

    /// App context: nullopt = disabled by default
    std::optional<std::filesystem::path> app_context;
    bool app_context_disabled = false; ///< true if app_context explicitly disabled

    bool inject_model_context = true;  ///< Auto-inject model context into system prompt
    int vram_reserve_mb = 512;         ///< Reserved VRAM headroom (MB, 0–65536)

    /// Config dir — base for bundled data discovery.
    std::filesystem::path config_dir;

    /// Session log directory (session.log + session_model.log).
    /// When non-empty, the facade creates a SessionLogger on configure.
    std::filesystem::path log_dir;

    /// Enable ggml/llama.cpp logging to llama_ggml.log in log_dir.
    /// Default false — when off, ggml/llama output is silenced entirely.
    bool ggml_logging = false;

    /// Override path for ggml/llama log when `ggml_logging == true`
    /// (gh#23 MVP item 12, v2.3.24). When non-empty, the orchestrator
    /// installs llama_log_set against this exact path. When empty
    /// (default), keeps the pre-v2.3.24 hardcoded
    /// `<log_dir>/llama_ggml.log` — bit-identical for callers not
    /// opting in. Accepts absolute or relative paths; relative paths
    /// resolve from the process CWD (not log_dir).
    /// @version 2.3.24
    std::filesystem::path llama_log_path;

    /// Emit engine spdlog output to the stderr console sink.
    /// Default true (operators reading stderr see everything). TUI
    /// consumers that paint to fd 2 must set this false so engine log
    /// lines don't corrupt the screen — with it off, logs route to the
    /// per-handle session.log file sink only. (gh#59 follow-up, v2.3.7)
    bool console_logging = true;

    /// Constitutional validation pipeline settings.
    ConstitutionalValidationConfig constitutional_validation;

    /// Inference-side knobs (currently speculative decoding only).
    /// @version 2.1.11
    InferenceConfig inference;
};

/**
 * @brief Inference parameters for a single identity phase.
 *
 * Each identity has one or more named phases. The engine resolves
 * inference params from the active phase at generation time.
 * Lives in types/ so core.so can use it without depending on prompts.so.
 *
 * @version 1.8.1
 */
struct PhaseConfig {
    float temperature = 0.7f;            ///< Sampling temperature
    int max_output_tokens = 4096;        ///< Max tokens per generation
    bool enable_thinking = false;        ///< Enable think-block output
    float repeat_penalty = 1.1f;         ///< Repetition penalty
    std::optional<std::vector<std::string>> bash_commands; ///< Phase-specific bash commands
};

} // namespace entropic
