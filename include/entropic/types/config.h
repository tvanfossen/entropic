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
 * @version 1.8.2
 */

#pragma once

#include <entropic/types/enums.h>

#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include <unordered_map>

namespace entropic {

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
 * @brief Generation parameters for a single inference call.
 * @version 1.8.2
 */
struct GenerationParams {
    float temperature = 0.7f;                ///< Sampling temperature
    float top_p = 0.9f;                      ///< Nucleus sampling threshold
    int top_k = 40;                          ///< Top-K sampling
    float repeat_penalty = 1.1f;             ///< Repetition penalty
    int max_tokens = 4096;                   ///< Maximum tokens to generate
    int reasoning_budget = -1;               ///< Per-call think budget override (-1 = unlimited)
    bool enable_thinking = true;             ///< Enable <think> blocks (false if reasoning_budget == 0)
    std::string grammar;                     ///< GBNF grammar string (empty = unconstrained)
    std::vector<std::string> stop;           ///< Stop sequences
    int logprobs = 0;                        ///< Top log-probs per token (0 = disabled)
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
 * @version 1.8.1
 */
struct TierConfig : ModelConfig {
    std::optional<std::filesystem::path> identity;  ///< Identity prompt path (nullopt = bundled)
    bool identity_disabled = false;                 ///< true if identity explicitly disabled
    std::optional<std::filesystem::path> grammar;   ///< Grammar file path
    std::optional<std::string> auto_chain;           ///< Target tier name (nullopt = defer to identity)
    std::optional<bool> routable;                   ///< None = defer to identity frontmatter
};

/**
 * @brief Configuration for all models (tiers + router).
 * @version 1.8.1
 */
struct ModelsConfig {
    std::unordered_map<std::string, TierConfig> tiers; ///< Tier name → config
    std::optional<ModelConfig> router;                  ///< Router model (separate from tiers)
    std::string default_tier = "lead";                  ///< Default tier name
};

/**
 * @brief Configuration for model routing.
 * @version 1.8.1
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
    bool enable_filesystem = true;   ///< Enable filesystem server
    bool enable_bash = true;         ///< Enable bash server
    bool enable_git = true;          ///< Enable git server
    bool enable_diagnostics = true;  ///< Enable diagnostics server
    bool enable_web = true;          ///< Enable web server
    FilesystemConfig filesystem;     ///< Filesystem server config
    ExternalMCPConfig external;      ///< External MCP server config (Entropic-as-server)
    int server_timeout_seconds = 30; ///< Server timeout (5–300)

    /* ── v1.8.7: External MCP client settings ──────────── */
    std::unordered_map<std::string, ExternalServerEntry> external_servers; ///< Named external servers
    ReconnectConfig reconnect;                   ///< Reconnection backoff policy
    uint32_t health_check_interval_ms = 0;       ///< Ping interval (0 = disabled)
    uint32_t tool_call_timeout_ms = 30000;       ///< Per-call timeout for external tools
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
    int tool_result_ttl = 10;                  ///< Tool result TTL in turns (1–20)
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
};

} // namespace entropic
