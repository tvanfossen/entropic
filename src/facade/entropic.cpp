/**
 * @file entropic.cpp
 * @brief librentropic facade — lifecycle stubs for v1.8.0.
 *
 * Real implementations land in subsequent versions:
 * - entropic_create/configure/destroy: v1.8.4 (engine loop)
 * - entropic_run/run_streaming: v1.8.4 (engine loop)
 * - entropic_interrupt: v1.8.4 (engine loop)
 *
 * @version 1.9.6
 */

#include <entropic/entropic.h>
#include <entropic/types/logging.h>
#include <cstdlib>
#include <cstring>

static auto s_log = entropic::log::get("facade");

extern "C" {

/**
 * @brief Create a new engine instance (stub).
 * @param handle Pointer to receive the new handle.
 * @return ENTROPIC_ERROR_INTERNAL — not yet implemented.
 * @internal
 * @version 1.8.0
 */
entropic_error_t entropic_create(entropic_handle_t* handle) {
    if (handle == nullptr) {
        return ENTROPIC_ERROR_INVALID_ARGUMENT;
    }
    entropic::log::init(spdlog::level::info);
    s_log->info("entropic_create() — v{}", CONFIG_ENTROPIC_VERSION_STRING);
    *handle = nullptr;
    return ENTROPIC_ERROR_INTERNAL;
}

/**
 * @brief Configure the engine from JSON (stub).
 * @param handle Engine handle.
 * @param config_json JSON config string.
 * @return ENTROPIC_ERROR_INVALID_STATE — not yet implemented.
 * @internal
 * @version 1.8.0
 */
entropic_error_t entropic_configure(
    entropic_handle_t handle,
    const char* config_json) {
    (void)handle;
    (void)config_json;
    return ENTROPIC_ERROR_INVALID_STATE;
}

/**
 * @brief Configure engine from file (stub).
 * @param handle Engine handle.
 * @param config_path Path to config file.
 * @return ENTROPIC_ERROR_INVALID_STATE — not yet implemented.
 * @internal
 * @version 1.8.0
 */
entropic_error_t entropic_configure_from_file(
    entropic_handle_t handle,
    const char* config_path) {
    (void)handle;
    (void)config_path;
    return ENTROPIC_ERROR_INVALID_STATE;
}

/**
 * @brief Destroy an engine instance (stub).
 * @param handle Engine handle. NULL is a no-op.
 * @internal
 * @version 1.8.0
 */
void entropic_destroy(entropic_handle_t handle) {
    if (handle == nullptr) {
        return;
    }
}

/**
 * @brief Get the library version string.
 * @return Static version string.
 * @utility
 * @version 1.8.0
 */
const char* entropic_version(void) {
    return CONFIG_ENTROPIC_VERSION_STRING;
}

/**
 * @brief Get the plugin API version number.
 * @return API version integer.
 * @utility
 * @version 1.8.0
 */
int entropic_api_version(void) {
    return 1;
}

/**
 * @brief Allocate memory using the engine's allocator.
 * @param size Number of bytes.
 * @return Pointer to allocated memory, or NULL on failure.
 * @utility
 * @version 1.8.0
 */
void* entropic_alloc(size_t size) {
    return malloc(size);
}

/**
 * @brief Free memory allocated by the engine.
 * @param ptr Pointer to free. NULL is a no-op.
 * @utility
 * @version 1.8.0
 */
void entropic_free(void* ptr) {
    free(ptr);
}

/**
 * @brief Single-turn blocking generation (stub).
 * @param handle Engine handle.
 * @param input User message.
 * @param result_json Output JSON result.
 * @return ENTROPIC_ERROR_INVALID_STATE — not yet implemented.
 * @internal
 * @version 1.8.0
 */
entropic_error_t entropic_run(
    entropic_handle_t handle,
    const char* input,
    char** result_json) {
    (void)handle;
    (void)input;
    (void)result_json;
    return ENTROPIC_ERROR_INVALID_STATE;
}

/**
 * @brief Streaming generation (stub).
 * @param handle Engine handle.
 * @param input User message.
 * @param on_token Token callback.
 * @param user_data Forwarded to callback.
 * @param cancel_flag Cancellation flag.
 * @return ENTROPIC_ERROR_INVALID_STATE — not yet implemented.
 * @internal
 * @version 1.8.0
 */
entropic_error_t entropic_run_streaming(
    entropic_handle_t handle,
    const char* input,
    void (*on_token)(const char* token, size_t len, void* user_data),
    void* user_data,
    int* cancel_flag) {
    (void)handle;
    (void)input;
    (void)on_token;
    (void)user_data;
    (void)cancel_flag;
    return ENTROPIC_ERROR_INVALID_STATE;
}

/**
 * @brief Interrupt a running generation (stub).
 * @param handle Engine handle.
 * @return ENTROPIC_ERROR_INVALID_STATE — not yet implemented.
 * @internal
 * @version 1.8.0
 */
entropic_error_t entropic_interrupt(entropic_handle_t handle) {
    (void)handle;
    return ENTROPIC_ERROR_INVALID_STATE;
}

// ── LoRA Adapter stubs (v1.9.2) ─────────────────────────────

/**
 * @brief Load a LoRA adapter (stub).
 * @param handle Engine handle.
 * @param adapter_name Adapter identifier.
 * @param adapter_path Path to .gguf file.
 * @param base_model_path Base model path.
 * @param scale LoRA scale factor.
 * @return ENTROPIC_ERROR_INVALID_STATE — not yet wired.
 * @internal
 * @version 1.9.2
 */
entropic_error_t entropic_adapter_load(
    entropic_handle_t handle,
    const char* adapter_name,
    const char* adapter_path,
    const char* base_model_path,
    float scale)
{
    (void)handle;
    (void)adapter_name;
    (void)adapter_path;
    (void)base_model_path;
    (void)scale;
    return ENTROPIC_ERROR_INVALID_STATE;
}

/**
 * @brief Unload a LoRA adapter (stub).
 * @param handle Engine handle.
 * @param adapter_name Adapter to unload.
 * @return ENTROPIC_ERROR_INVALID_STATE — not yet wired.
 * @internal
 * @version 1.9.2
 */
entropic_error_t entropic_adapter_unload(
    entropic_handle_t handle,
    const char* adapter_name)
{
    (void)handle;
    (void)adapter_name;
    return ENTROPIC_ERROR_INVALID_STATE;
}

/**
 * @brief Swap active adapter (stub).
 * @param handle Engine handle.
 * @param adapter_name Target adapter.
 * @return ENTROPIC_ERROR_INVALID_STATE — not yet wired.
 * @internal
 * @version 1.9.2
 */
entropic_error_t entropic_adapter_swap(
    entropic_handle_t handle,
    const char* adapter_name)
{
    (void)handle;
    (void)adapter_name;
    return ENTROPIC_ERROR_INVALID_STATE;
}

/**
 * @brief Query adapter state (stub).
 * @param handle Engine handle.
 * @param adapter_name Adapter identifier.
 * @return -1 — not yet wired.
 * @internal
 * @version 1.9.2
 */
int entropic_adapter_state(
    entropic_handle_t handle,
    const char* adapter_name)
{
    (void)handle;
    (void)adapter_name;
    return -1;
}

/**
 * @brief Get adapter info as JSON (stub).
 * @param handle Engine handle.
 * @param adapter_name Adapter identifier.
 * @return NULL — not yet wired.
 * @internal
 * @version 1.9.2
 */
char* entropic_adapter_info(
    entropic_handle_t handle,
    const char* adapter_name)
{
    (void)handle;
    (void)adapter_name;
    return nullptr;
}

/**
 * @brief List all adapters as JSON (stub).
 * @param handle Engine handle.
 * @return NULL — not yet wired.
 * @internal
 * @version 1.9.2
 */
char* entropic_adapter_list(entropic_handle_t handle)
{
    (void)handle;
    return nullptr;
}

// ── Grammar Registry stubs (v1.9.3) ─────────────────────────

/**
 * @brief Register a grammar by key (stub).
 * @param handle Engine handle.
 * @param key Grammar name.
 * @param gbnf_content GBNF grammar string.
 * @return ENTROPIC_ERROR_INVALID_STATE — not yet wired.
 * @internal
 * @version 1.9.3
 */
entropic_error_t entropic_grammar_register(
    entropic_handle_t handle,
    const char* key,
    const char* gbnf_content)
{
    (void)handle;
    (void)key;
    (void)gbnf_content;
    return ENTROPIC_ERROR_INVALID_STATE;
}

/**
 * @brief Register a grammar from file (stub).
 * @param handle Engine handle.
 * @param key Grammar name.
 * @param path Path to .gbnf file.
 * @return ENTROPIC_ERROR_INVALID_STATE — not yet wired.
 * @internal
 * @version 1.9.3
 */
entropic_error_t entropic_grammar_register_file(
    entropic_handle_t handle,
    const char* key,
    const char* path)
{
    (void)handle;
    (void)key;
    (void)path;
    return ENTROPIC_ERROR_INVALID_STATE;
}

/**
 * @brief Deregister a grammar (stub).
 * @param handle Engine handle.
 * @param key Grammar name.
 * @return ENTROPIC_ERROR_INVALID_STATE — not yet wired.
 * @internal
 * @version 1.9.3
 */
entropic_error_t entropic_grammar_deregister(
    entropic_handle_t handle,
    const char* key)
{
    (void)handle;
    (void)key;
    return ENTROPIC_ERROR_INVALID_STATE;
}

/**
 * @brief Get grammar content by key (stub).
 * @param handle Engine handle.
 * @param key Grammar name.
 * @return NULL — not yet wired.
 * @internal
 * @version 1.9.3
 */
char* entropic_grammar_get(
    entropic_handle_t handle,
    const char* key)
{
    (void)handle;
    (void)key;
    return nullptr;
}

/**
 * @brief Validate a GBNF grammar string (stub).
 *
 * Full implementation will use GrammarRegistry::validate() once
 * the facade is wired to the inference subsystem.
 *
 * @param gbnf_content Raw GBNF string.
 * @return NULL — stub always reports valid.
 * @internal
 * @version 1.9.3
 */
char* entropic_grammar_validate(const char* gbnf_content) {
    (void)gbnf_content;
    return nullptr;
}

/**
 * @brief List all grammars as JSON (stub).
 * @param handle Engine handle.
 * @return NULL — not yet wired.
 * @internal
 * @version 1.9.3
 */
char* entropic_grammar_list(entropic_handle_t handle)
{
    (void)handle;
    return nullptr;
}

// ── GPU Resource Profile stubs (v1.9.7) ──────────────────────

/**
 * @brief Register a custom GPU resource profile (stub).
 * @param handle Engine handle.
 * @param profile_json Profile JSON string.
 * @return ENTROPIC_ERROR_NOT_IMPLEMENTED — not yet wired.
 * @internal
 * @version 1.9.7
 */
entropic_error_t entropic_profile_register(
    entropic_handle_t handle,
    const char* profile_json)
{
    (void)handle;
    (void)profile_json;
    return ENTROPIC_ERROR_NOT_IMPLEMENTED;
}

/**
 * @brief Remove a GPU resource profile (stub).
 * @param handle Engine handle.
 * @param name Profile name.
 * @return ENTROPIC_ERROR_NOT_IMPLEMENTED — not yet wired.
 * @internal
 * @version 1.9.7
 */
entropic_error_t entropic_profile_deregister(
    entropic_handle_t handle,
    const char* name)
{
    (void)handle;
    (void)name;
    return ENTROPIC_ERROR_NOT_IMPLEMENTED;
}

/**
 * @brief Get a profile by name as JSON (stub).
 * @param handle Engine handle.
 * @param name Profile name.
 * @return NULL — not yet wired.
 * @internal
 * @version 1.9.7
 */
char* entropic_profile_get(
    entropic_handle_t handle,
    const char* name)
{
    (void)handle;
    (void)name;
    return nullptr;
}

/**
 * @brief List all profiles as JSON (stub).
 * @param handle Engine handle.
 * @return NULL — not yet wired.
 * @internal
 * @version 1.9.7
 */
char* entropic_profile_list(entropic_handle_t handle)
{
    (void)handle;
    return nullptr;
}

// ── Throughput Query stubs (v1.9.7) ──────────────────────────

/**
 * @brief Get throughput estimate for a model (stub).
 * @param handle Engine handle.
 * @param model_path Model path.
 * @return 0.0 — not yet wired.
 * @internal
 * @version 1.9.7
 */
double entropic_throughput_tok_per_sec(
    entropic_handle_t handle,
    const char* model_path)
{
    (void)handle;
    (void)model_path;
    return 0.0;
}

/**
 * @brief Reset throughput data (stub).
 * @param handle Engine handle.
 * @param model_path Model path (NULL = all).
 * @internal
 * @version 1.9.7
 */
void entropic_throughput_reset(
    entropic_handle_t handle,
    const char* model_path)
{
    (void)handle;
    (void)model_path;
}

// ── MCP Authorization stubs (v1.9.4) ─────────────────────────

/**
 * @brief Grant an MCP tool key to an identity (stub).
 * @param handle Engine handle.
 * @param identity_name Identity name.
 * @param pattern Tool pattern.
 * @param level Access level.
 * @return ENTROPIC_ERROR_INVALID_STATE — not yet wired.
 * @internal
 * @version 1.9.4
 */
entropic_error_t entropic_grant_mcp_key(
    entropic_handle_t handle,
    const char* identity_name,
    const char* pattern,
    entropic_mcp_access_level_t level)
{
    (void)handle;
    (void)identity_name;
    (void)pattern;
    (void)level;
    return ENTROPIC_ERROR_INVALID_STATE;
}

/**
 * @brief Revoke an MCP tool key (stub).
 * @param handle Engine handle.
 * @param identity_name Identity name.
 * @param pattern Tool pattern.
 * @return ENTROPIC_ERROR_INVALID_STATE — not yet wired.
 * @internal
 * @version 1.9.4
 */
entropic_error_t entropic_revoke_mcp_key(
    entropic_handle_t handle,
    const char* identity_name,
    const char* pattern)
{
    (void)handle;
    (void)identity_name;
    (void)pattern;
    return ENTROPIC_ERROR_INVALID_STATE;
}

/**
 * @brief Check MCP key authorization (stub).
 * @param handle Engine handle.
 * @param identity_name Identity name.
 * @param tool_name Tool name.
 * @param level Required access level.
 * @return -1 — not yet wired.
 * @internal
 * @version 1.9.4
 */
int entropic_check_mcp_key(
    entropic_handle_t handle,
    const char* identity_name,
    const char* tool_name,
    entropic_mcp_access_level_t level)
{
    (void)handle;
    (void)identity_name;
    (void)tool_name;
    (void)level;
    return -1;
}

/**
 * @brief List MCP keys for identity (stub).
 * @param handle Engine handle.
 * @param identity_name Identity name.
 * @return NULL — not yet wired.
 * @internal
 * @version 1.9.4
 */
char* entropic_list_mcp_keys(
    entropic_handle_t handle,
    const char* identity_name)
{
    (void)handle;
    (void)identity_name;
    return nullptr;
}

/**
 * @brief Grant key from one identity to another (stub).
 * @param handle Engine handle.
 * @param granter Granting identity.
 * @param grantee Receiving identity.
 * @param pattern Tool pattern.
 * @param level Access level.
 * @return ENTROPIC_ERROR_INVALID_STATE — not yet wired.
 * @internal
 * @version 1.9.4
 */
entropic_error_t entropic_grant_mcp_key_from(
    entropic_handle_t handle,
    const char* granter,
    const char* grantee,
    const char* pattern,
    entropic_mcp_access_level_t level)
{
    (void)handle;
    (void)granter;
    (void)grantee;
    (void)pattern;
    (void)level;
    return ENTROPIC_ERROR_INVALID_STATE;
}

/**
 * @brief Serialize all MCP key sets (stub).
 * @param handle Engine handle.
 * @return NULL — not yet wired.
 * @internal
 * @version 1.9.4
 */
char* entropic_serialize_mcp_keys(entropic_handle_t handle)
{
    (void)handle;
    return nullptr;
}

/**
 * @brief Deserialize all MCP key sets (stub).
 * @param handle Engine handle.
 * @param json JSON string.
 * @return ENTROPIC_ERROR_INVALID_STATE — not yet wired.
 * @internal
 * @version 1.9.4
 */
entropic_error_t entropic_deserialize_mcp_keys(
    entropic_handle_t handle,
    const char* json)
{
    (void)handle;
    (void)json;
    return ENTROPIC_ERROR_INVALID_STATE;
}

// ── Dynamic Identity Management stubs (v1.9.6) ──────────────

/**
 * @brief Create a dynamic identity (stub).
 * @param handle Engine handle.
 * @param config_json Identity config JSON.
 * @return ENTROPIC_ERROR_INVALID_STATE — not yet wired.
 * @internal
 * @version 1.9.6
 */
entropic_error_t entropic_create_identity(
    entropic_handle_t handle,
    const char* config_json)
{
    (void)handle;
    (void)config_json;
    return ENTROPIC_ERROR_INVALID_STATE;
}

/**
 * @brief Update a dynamic identity (stub).
 * @param handle Engine handle.
 * @param name Identity name.
 * @param config_json Replacement config JSON.
 * @return ENTROPIC_ERROR_INVALID_STATE — not yet wired.
 * @internal
 * @version 1.9.6
 */
entropic_error_t entropic_update_identity(
    entropic_handle_t handle,
    const char* name,
    const char* config_json)
{
    (void)handle;
    (void)name;
    (void)config_json;
    return ENTROPIC_ERROR_INVALID_STATE;
}

/**
 * @brief Destroy a dynamic identity (stub).
 * @param handle Engine handle.
 * @param name Identity name.
 * @return ENTROPIC_ERROR_INVALID_STATE — not yet wired.
 * @internal
 * @version 1.9.6
 */
entropic_error_t entropic_destroy_identity(
    entropic_handle_t handle,
    const char* name)
{
    (void)handle;
    (void)name;
    return ENTROPIC_ERROR_INVALID_STATE;
}

/**
 * @brief Get identity config as JSON by name (stub).
 * @param handle Engine handle.
 * @param name Identity name.
 * @return NULL — not yet wired.
 * @internal
 * @version 1.9.6
 */
char* entropic_get_identity_config(
    entropic_handle_t handle,
    const char* name)
{
    (void)handle;
    (void)name;
    return nullptr;
}

/**
 * @brief List all identity names as JSON (stub).
 * @param handle Engine handle.
 * @return NULL — not yet wired.
 * @internal
 * @version 1.9.6
 */
char* entropic_list_identities(entropic_handle_t handle)
{
    (void)handle;
    return nullptr;
}

/**
 * @brief Get identity count (stub).
 * @param handle Engine handle.
 * @param total Output: total count.
 * @param dynamic Output: dynamic count (may be NULL).
 * @return ENTROPIC_ERROR_INVALID_STATE — not yet wired.
 * @internal
 * @version 1.9.6
 */
entropic_error_t entropic_identity_count(
    entropic_handle_t handle,
    size_t* total,
    size_t* dynamic)
{
    (void)handle;
    (void)total;
    (void)dynamic;
    return ENTROPIC_ERROR_INVALID_STATE;
}

// ── Log-Probability Evaluation (v1.9.10) ──────────────────────

/**
 * @brief Evaluate per-token log-probabilities (stub).
 * @param handle Engine handle.
 * @param model_id Model identifier.
 * @param tokens Array of token IDs.
 * @param n_tokens Number of tokens.
 * @param result Output logprob result.
 * @return ENTROPIC_ERROR_NOT_IMPLEMENTED — facade not yet wired.
 * @internal
 * @version 1.9.10
 */
entropic_error_t entropic_get_logprobs(
    entropic_handle_t handle,
    const char* model_id,
    const int32_t* tokens,
    int n_tokens,
    entropic_logprob_result_t* result)
{
    (void)handle;
    (void)model_id;
    (void)tokens;
    (void)n_tokens;
    (void)result;
    return ENTROPIC_ERROR_NOT_IMPLEMENTED;
}

/**
 * @brief Compute perplexity for a token sequence (stub).
 * @param handle Engine handle.
 * @param model_id Model identifier.
 * @param tokens Array of token IDs.
 * @param n_tokens Number of tokens.
 * @param perplexity Output perplexity value.
 * @return ENTROPIC_ERROR_NOT_IMPLEMENTED — facade not yet wired.
 * @internal
 * @version 1.9.10
 */
entropic_error_t entropic_compute_perplexity(
    entropic_handle_t handle,
    const char* model_id,
    const int32_t* tokens,
    int n_tokens,
    float* perplexity)
{
    (void)handle;
    (void)model_id;
    (void)tokens;
    (void)n_tokens;
    (void)perplexity;
    return ENTROPIC_ERROR_NOT_IMPLEMENTED;
}

/**
 * @brief Free internal arrays of a logprob result.
 *
 * Frees logprobs and tokens arrays, then NULLs the pointers to
 * prevent double-free. The struct itself is caller-owned.
 *
 * @param result Pointer to result struct. NULL-safe.
 * @utility
 * @version 1.9.10
 */
void entropic_free_logprob_result(entropic_logprob_result_t* result)
{
    if (result == nullptr) {
        return;
    }
    free(result->logprobs);
    result->logprobs = nullptr;
    free(result->tokens);
    result->tokens = nullptr;
}

// ── Constitutional Validation stubs (v1.9.8) ─────────────────

/**
 * @brief Enable or disable constitutional validation (stub).
 * @param handle Engine handle.
 * @param enabled true to enable, false to disable.
 * @return ENTROPIC_ERROR_NOT_IMPLEMENTED — not yet wired.
 * @internal
 * @version 1.9.8
 */
entropic_error_t entropic_validation_set_enabled(
    entropic_handle_t handle,
    bool enabled)
{
    (void)handle;
    (void)enabled;
    return ENTROPIC_ERROR_NOT_IMPLEMENTED;
}

/**
 * @brief Set per-identity validation override (stub).
 * @param handle Engine handle.
 * @param identity_name Identity name.
 * @param enabled Whether validation is enabled.
 * @return ENTROPIC_ERROR_NOT_IMPLEMENTED — not yet wired.
 * @internal
 * @version 1.9.8
 */
entropic_error_t entropic_validation_set_identity(
    entropic_handle_t handle,
    const char* identity_name,
    bool enabled)
{
    (void)handle;
    (void)identity_name;
    (void)enabled;
    return ENTROPIC_ERROR_NOT_IMPLEMENTED;
}

/**
 * @brief Get last validation result as JSON (stub).
 * @param handle Engine handle.
 * @return NULL — not yet wired.
 * @internal
 * @version 1.9.8
 */
char* entropic_validation_last_result(entropic_handle_t handle)
{
    (void)handle;
    return nullptr;
}

/**
 * @brief Get diagnostic prompt text for /diagnose command (stub).
 * @param handle Engine handle.
 * @param prompt_out Output: diagnostic prompt string.
 * @return ENTROPIC_OK on success, error code on failure.
 * @internal
 * @version 1.9.12
 */
entropic_error_t entropic_get_diagnostic_prompt(
    entropic_handle_t handle,
    char** prompt_out) {
    if (handle == nullptr || prompt_out == nullptr) {
        return ENTROPIC_ERROR_INVALID_ARGUMENT;
    }
    (void)handle;
    static const char* prompt =
        "[SYSTEM DIRECTIVE: SELF-DIAGNOSIS]\n\n"
        "Analyze your recent actions and identify any issues. "
        "Follow these steps:\n\n"
        "1. Call entropic.diagnose to get a full engine state "
        "snapshot.\n"
        "2. Review the tool call history for:\n"
        "   - Repeated failures (same tool, same error)\n"
        "   - Duplicate tool calls (circuit breaker risk)\n"
        "   - Tool calls that returned errors\n"
        "   - Unexpected state (wrong phase, wrong tier)\n"
        "3. Review your reasoning for:\n"
        "   - Actions that didn't achieve the stated goal\n"
        "   - Unnecessary tool calls\n"
        "   - Missing context that led to errors\n"
        "4. Produce a structured assessment:\n"
        "   - FINDINGS: What went wrong (be specific)\n"
        "   - ROOT CAUSE: Why it went wrong\n"
        "   - RECOMMENDATION: What to do differently\n\n"
        "Be honest and specific. The goal is accurate "
        "self-assessment, not self-defense.\n";
    *prompt_out = strdup(prompt);
    return ENTROPIC_OK;
}

} // extern "C"
