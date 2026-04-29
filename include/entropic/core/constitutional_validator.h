// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file constitutional_validator.h
 * @brief Post-generation constitutional compliance validator.
 *
 * @par Responsibilities:
 * - Run grammar-constrained critique pass on generation output
 * - Parse structured JSON verdict
 * - Trigger revision loop when violations found
 * - Per-identity validation opt-out
 * - Register/deregister as POST_GENERATE hook
 *
 * @par Thread safety:
 * - Config is immutable after construction (global_enabled_ is a
 *   separate runtime toggle, not part of the frozen config)
 * - Critique generation is stateless (uses inference interface)
 * - Per-identity overrides and global_enabled_ guarded by mutex
 * - last_result_ guarded by mutex for concurrent access
 *
 * @par Ownership:
 * Owned by the engine handle. One ConstitutionalValidator per engine.
 * Single concrete class (no three-layer hierarchy — one implementation,
 * like HookRegistry and GrammarRegistry).
 *
 * @par Cross-.so design:
 * Lives in core.so. Uses InferenceInterface function pointers for
 * critique generation (no compile-time dependency on inference.so).
 * Grammar key "constitutional_critique" resolved by the inference
 * layer from the GrammarRegistry.
 *
 * @version 1.9.8
 */

#pragma once

#include <entropic/interfaces/i_hook_handler.h>
#include <entropic/interfaces/i_inference_callbacks.h>
#include <entropic/types/config.h>
#include <entropic/types/error.h>
#include <entropic/types/hooks.h>
#include <entropic/types/validation.h>

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace entropic {

/**
 * @brief Context passed through hook user_data.
 *
 * Carries the validator instance and inference interface through the
 * POST_GENERATE hook callback. Allocated once at attach() time.
 *
 * @version 1.9.8
 */
struct ValidationContext {
    class ConstitutionalValidator* validator; ///< Validator instance
    InferenceInterface* inference;            ///< For critique generation
};

/**
 * @brief Post-generation constitutional compliance validator.
 *
 * Evaluates engine output against constitutional rules using a
 * grammar-constrained critique generation pass, parses a structured
 * JSON verdict, and optionally re-generates with feedback when
 * violations are found.
 *
 * @par Usage
 * @code
 *   ConstitutionalValidator validator(config, constitution_text);
 *   validator.attach(&hook_registry, &inference);
 *   // ... engine runs, POST_GENERATE hook triggers validation ...
 *   validator.detach(&hook_registry);
 * @endcode
 *
 * @version 1.9.8
 */
class ConstitutionalValidator {
public:
    /**
     * @brief Construct validator with config and constitution text.
     * @param config Validation pipeline configuration.
     * @param constitution_text Full constitution text (from PromptManager).
     * @version 1.9.8
     */
    ConstitutionalValidator(
        const ConstitutionalValidationConfig& config,
        const std::string& constitution_text);

    /**
     * @brief Register this validator as a POST_GENERATE hook.
     * @param hook_iface HookInterface for registration (via fire_post).
     * @param inference InferenceInterface for critique generation.
     * @return ENTROPIC_OK on success.
     * @version 1.9.8
     */
    entropic_error_t attach(
        HookInterface* hook_iface,
        InferenceInterface* inference);

    /**
     * @brief Deregister the POST_GENERATE hook.
     * @param hook_iface HookInterface for deregistration.
     * @version 1.9.8
     */
    void detach(HookInterface* hook_iface);

    /**
     * @brief Check if validation is enabled for a given identity.
     *
     * Returns false if the tier is in config_.skip_tiers (e.g. "lead").
     * Per-identity overrides (set_identity_validation) take precedence.
     *
     * @param identity_name Identity name to check.
     * @return true if validation should run for this identity.
     * @version 2.0.7
     */
    bool should_validate(const std::string& identity_name) const;

    /**
     * @brief Set per-identity validation override.
     * @param identity_name Identity name.
     * @param enabled Whether validation is enabled for this identity.
     * @version 1.9.8
     */
    void set_identity_validation(
        const std::string& identity_name, bool enabled);

    /**
     * @brief Set per-identity validation rules from frontmatter.
     * @param identity_name Identity/tier name.
     * @param rules Validation rules specific to this identity.
     * @version 2.0.6
     */
    void set_tier_rules(
        const std::string& identity_name,
        const std::vector<std::string>& rules);

    /**
     * @brief Toggle the global validation gate at runtime.
     *
     * Per-identity overrides set via set_identity_validation() still
     * take precedence; this only changes the fallback value used when
     * no per-identity override exists.
     *
     * @param enabled true to enable validation globally, false to disable.
     * @req REQ-VALID-004
     * @version 2.0.2
     */
    void set_global_enabled(bool enabled);

    /**
     * @brief Run the validation pipeline on generated content.
     * @param content The generated text to validate.
     * @param tier The tier/identity that produced the content.
     * @param messages_json The conversation context (for revision).
     * @return ValidationResult with final content and critique metadata.
     *
     * Called from the POST_GENERATE hook callback. Flow:
     * 1. Check per-identity override
     * 2. Run critique generation pass
     * 3. Parse structured JSON verdict
     * 4. If violations and revision enabled, revise
     * 5. Return final (possibly revised) content
     *
     * @version 1.9.8
     */
    ValidationResult validate(
        const std::string& content,
        const std::string& tier,
        const char* messages_json);

    /**
     * @brief Get the last validation result.
     * @return Most recent ValidationResult, or default if none.
     * @version 1.9.8
     */
    ValidationResult last_result() const;

    /**
     * @brief Get the config (read-only after construction).
     * @return Reference to config.
     * @utility
     * @version 1.9.8
     */
    const ConstitutionalValidationConfig& config() const { return config_; }

    /**
     * @brief POST_GENERATE hook callback for constitutional validation.
     *
     * Static function registered with HookRegistry. Extracts
     * ValidationContext from user_data, runs the validation pipeline,
     * and writes modified_json if content was revised.
     *
     * @param hook_point Hook point (POST_GENERATE).
     * @param context_json JSON context from engine.
     * @param modified_json Output: revised JSON or NULL.
     * @param user_data ValidationContext pointer.
     * @return 0 (post-hooks cannot cancel).
     * @callback
     * @version 1.9.8
     */
    static int hook_callback(
        entropic_hook_point_t hook_point,
        const char* context_json,
        char** modified_json,
        void* user_data);

    /**
     * @brief Build the critique prompt (exposed for testing).
     * @param content Text to critique.
     * @return Formatted critique prompt string.
     * @version 1.9.8
     */
    std::string build_critique_prompt(const std::string& content) const;

    /**
     * @brief Parse critique JSON into structured result (exposed for testing).
     * @param json_str Raw JSON string from grammar-constrained generation.
     * @return Parsed CritiqueResult.
     * @version 1.9.8
     */
    static CritiqueResult parse_critique(const std::string& json_str);

private:
    /**
     * @brief Generate a critique of the given content.
     * @param content Text to critique.
     * @return Parsed CritiqueResult.
     * @internal
     * @version 1.9.8
     */
    CritiqueResult run_critique(const std::string& content);

    /**
     * @brief Build messages JSON for critique generation.
     * @param content Text to critique.
     * @return JSON array with system + user messages.
     * @internal
     * @version 1.9.8
     */
    std::string build_critique_messages(const std::string& content) const;

    /**
     * @brief Build params JSON for critique generation.
     * @return JSON string with grammar_key, max_tokens, temperature.
     * @internal
     * @version 1.9.8
     */
    std::string build_critique_params() const;

    /**
     * @brief Re-generate with critique feedback injected.
     * @param original The content that was critiqued.
     * @param critique The critique result with violations.
     * @param messages_json Original conversation context.
     * @return Revised content string.
     * @internal
     * @version 1.9.8
     */
    std::string revise(
        const std::string& original,
        const CritiqueResult& critique,
        const char* messages_json);

    /**
     * @brief Store a validation result (thread-safe).
     * @param result Result to store.
     * @internal
     * @version 1.9.8
     */
    void store_result(const ValidationResult& result);

    /**
     * @brief Emit a disambiguating log line per verdict.
     * @param result Validation result with verdict set.
     * @internal
     * @version 2.0.6-rc17
     */
    void log_verdict(const ValidationResult& result) const;

    /**
     * @brief Run the core validation loop (critique + revise).
     * @param content Original content.
     * @param tier Identity/tier name.
     * @param messages_json Conversation context.
     * @return ValidationResult after critique/revision.
     * @internal
     * @version 1.9.8
     */
    ValidationResult run_validation_loop(
        const std::string& content,
        const std::string& tier,
        const char* messages_json);

    /**
     * @brief Apply revision strategy (Path A then Path B).
     * @param result Current validation result.
     * @param initial_critique First critique result.
     * @param messages_json Conversation context.
     * @return Updated ValidationResult.
     * @internal
     * @version 1.9.8
     */
    ValidationResult apply_revisions(
        ValidationResult result,
        const CritiqueResult& initial_critique,
        const char* messages_json);

    /**
     * @brief Attempt a single revision (Path A or Path B).
     * @param content Content to revise.
     * @param critique Critique with violations.
     * @param messages_json Conversation context for Path B.
     * @return Revised content string.
     * @internal
     * @version 1.9.8
     */
    std::string attempt_revision(
        const std::string& content,
        const CritiqueResult& critique,
        const char* messages_json);

    /**
     * @brief Build a single-turn messages JSON.
     * @param prompt Full prompt text.
     * @return JSON array string.
     * @internal
     * @version 1.9.8
     */
    std::string build_single_turn_json(
        const std::string& prompt) const;

    /**
     * @brief Build revision messages JSON with feedback injection.
     * @param original Content that was critiqued.
     * @param critique Critique with violations.
     * @param messages_json Original conversation context.
     * @return Augmented messages JSON.
     * @internal
     * @version 1.9.8
     */
    std::string build_revision_messages(
        const std::string& original,
        const CritiqueResult& critique,
        const char* messages_json) const;

    /**
     * @brief Build human-readable feedback text from violations.
     * @param critique Critique result with violations.
     * @return Feedback string.
     * @internal
     * @version 1.9.8
     */
    std::string build_feedback_text(
        const CritiqueResult& critique) const;

    /**
     * @brief Inject feedback into conversation messages.
     * @param original Assistant's original response.
     * @param feedback Feedback message.
     * @param messages_json Base conversation messages.
     * @return Augmented JSON array.
     * @internal
     * @version 1.9.8
     */
    std::string inject_feedback_into_messages(
        const std::string& original,
        const std::string& feedback,
        const char* messages_json) const;

    /**
     * @brief Handle the POST_GENERATE hook invocation.
     * @param context_json JSON context from engine.
     * @param modified_json Output: revised JSON or NULL.
     * @return 0 (post-hooks cannot cancel).
     * @internal
     * @version 1.9.8
     */
    int handle_hook(const char* context_json, char** modified_json);

    /**
     * @brief Write revised content into modified_json output.
     * @param content Revised content.
     * @param modified_json Output buffer (malloc'd).
     * @internal
     * @version 1.9.8
     */
    static void write_modified_json(
        const std::string& content, char** modified_json);

    /**
     * @brief Extract a string value from flat JSON.
     * @param json JSON string.
     * @param key Key name.
     * @return Extracted value, or empty string.
     * @utility
     * @version 1.9.8
     */
    static std::string extract_json_string(
        const char* json, const char* key);

    /**
     * @brief Extract string value after a colon in JSON.
     * @param pos Position after the key.
     * @return Extracted string content.
     * @utility
     * @version 1.9.8
     */
    static std::string extract_string_after_colon(const char* pos);

    /**
     * @brief Extract the "compliant" boolean from critique JSON.
     * @param json Raw JSON string.
     * @param result CritiqueResult to populate.
     * @return true if field found and parsed.
     * @utility
     * @version 1.9.8
     */
    static bool extract_compliant_field(
        const std::string& json, CritiqueResult& result);

    /**
     * @brief Extract violations array from critique JSON.
     * @param json Raw JSON string.
     * @param result CritiqueResult to populate.
     * @utility
     * @version 1.9.8
     */
    static void extract_violations(
        const std::string& json, CritiqueResult& result);

    /**
     * @brief Extract the next violation from JSON.
     * @param json Full JSON string.
     * @param pos Search position (updated).
     * @return Violation if found, nullopt if no more.
     * @utility
     * @version 1.9.8
     */
    static std::optional<Violation> extract_next_violation(
        const std::string& json, size_t& pos);

    /**
     * @brief Extract the "revised" field from critique JSON.
     * @param json Raw JSON string.
     * @param result CritiqueResult to populate.
     * @utility
     * @version 1.9.8
     */
    static void extract_revised_field(
        const std::string& json, CritiqueResult& result);

    ConstitutionalValidationConfig config_;   ///< Pipeline configuration (immutable)
    std::string constitution_text_;           ///< Full constitution content
    InferenceInterface* inference_ = nullptr; ///< For critique/revision generation
    ValidationContext context_;                ///< Hook user_data context

    /// @brief Runtime global enable toggle (decoupled from frozen config_).
    bool global_enabled_;
    /// @brief Per-identity validation overrides.
    std::unordered_map<std::string, bool> identity_overrides_;
    /// @brief Per-identity validation rules from frontmatter (v2.0.6).
    std::unordered_map<std::string, std::vector<std::string>> tier_rules_;
    mutable std::mutex overrides_mutex_;      ///< Guards identity_overrides_ + tier_rules_ + global_enabled_

    /// @brief Current tier being validated (set at handle_hook() entry).
    std::string current_tier_;
    /// @brief Tool call manifest for this turn (set at handle_hook() entry).
    /// Summarises tool names + result sizes from the conversation turn.
    /// Prepended to the critique prompt so the validator knows which
    /// tool calls preceded the output being evaluated.
    std::string current_tool_context_;

    /// @brief Un-pruned tool-result evidence for this turn (v2.1.3 #5).
    /// Set at handle_hook() entry from the POST_GENERATE hook context's
    /// optional ``tool_evidence`` field. Carries the actual content of
    /// recent tool results (truncated per-entry, bounded at 20 results)
    /// so the critique pass can verify ``file:line`` citations against
    /// real evidence rather than the stubs that ``ContextManager::
    /// prune_old_tool_results`` leaves in the message stream. Empty
    /// when running against pre-2.1.3 engines that don't surface this
    /// field — critique falls back to manifest-only as before.
    std::string current_tool_evidence_;
    /// @brief Identity system prompt for this tier (set at handle_hook() entry).
    /// Injected into revision context so the model maintains its persona
    /// rather than reverting to base behaviour (apology, self-flagellation).
    std::string current_system_prompt_;

    /// @brief Last validation result for C API query.
    ValidationResult last_result_;
    mutable std::mutex result_mutex_;         ///< Guards last_result_
};

} // namespace entropic
