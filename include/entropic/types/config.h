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
 * @version 1.9.3 — added grammar_key
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
 * @brief Audit log configuration within StorageConfig.
 *
 * Controls JSONL audit logging of MCP tool calls. When enabled,
 * every tool execution is recorded to audit.jsonl alongside the
 * session persistence files.
 *
 * @version 1.9.5
 */
struct AuditLogConfig {
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
};

/**
 * @brief Constitutional validation pipeline configuration.
 *
 * Controls the post-generation validation hook that critiques engine
 * output against constitutional rules. Default disabled — doubles
 * inference cost per generation when active.
 *
 * @version 1.9.8
 */
struct ConstitutionalValidationConfig {
    bool enabled = false;             ///< Global enable/disable (default OFF)
    int max_revisions = 2;            ///< Max re-generation attempts (0 = critique only)
    int max_critique_tokens = 512;    ///< Token budget for critique generation
    int priority = 100;               ///< Hook priority (higher = later)
    std::string grammar_key = "constitutional_critique";  ///< Grammar registry key
};

} // namespace entropic
