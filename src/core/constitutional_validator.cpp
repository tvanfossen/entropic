// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file constitutional_validator.cpp
 * @brief Constitutional validation pipeline implementation.
 *
 * Post-generation constitutional compliance validation using
 * grammar-constrained critique and automatic revision.
 *
 * @version 1.9.8
 */

#include <entropic/core/constitutional_validator.h>
#include <entropic/core/hook_registry.h>

#include <entropic/types/logging.h>

#include <cstdlib>
#include <cstring>

namespace entropic {

namespace {
auto logger = entropic::log::get("core.constitutional_validator");
} // anonymous namespace

/**
 * @brief Construct validator with config and constitution text.
 * @param config Validation pipeline configuration.
 * @param constitution_text Full constitution text.
 * @version 2.0.4
 */
ConstitutionalValidator::ConstitutionalValidator(
    const ConstitutionalValidationConfig& config,
    const std::string& constitution_text)
    : config_(config),
      constitution_text_(constitution_text),
      global_enabled_(config.enabled) {
    context_.validator = this;
    context_.inference = nullptr;
}

/**
 * @brief Register as POST_GENERATE hook via the HookRegistry C API.
 * @param hook_iface HookInterface with registry pointer.
 * @param inference InferenceInterface for critique generation.
 * @return ENTROPIC_OK on success.
 * @internal
 * @version 1.9.8
 */
entropic_error_t ConstitutionalValidator::attach(
    HookInterface* hook_iface,
    InferenceInterface* inference) {
    inference_ = inference;
    context_.inference = inference;

    if (hook_iface == nullptr || hook_iface->registry == nullptr) {
        return ENTROPIC_ERROR_INVALID_ARGUMENT;
    }

    auto* reg = static_cast<HookRegistry*>(hook_iface->registry);
    return reg->register_hook(
        ENTROPIC_HOOK_POST_GENERATE,
        hook_callback,
        &context_,
        config_.priority);
}

/**
 * @brief Deregister the POST_GENERATE hook.
 * @param hook_iface HookInterface with registry pointer.
 * @internal
 * @version 1.9.8
 */
void ConstitutionalValidator::detach(HookInterface* hook_iface) {
    if (hook_iface == nullptr || hook_iface->registry == nullptr) {
        return;
    }
    auto* reg = static_cast<HookRegistry*>(hook_iface->registry);
    reg->deregister_hook(
        ENTROPIC_HOOK_POST_GENERATE, hook_callback, &context_);
}

/**
 * @brief Check if validation is enabled for a given identity.
 * @param identity_name Identity name to check.
 * @return true if validation should run.
 * @internal
 * @version 2.0.4
 */
bool ConstitutionalValidator::should_validate(
    const std::string& identity_name) const {
    std::lock_guard<std::mutex> lock(overrides_mutex_);
    auto it = identity_overrides_.find(identity_name);
    if (it != identity_overrides_.end()) {
        return it->second;
    }
    return global_enabled_;
}

/**
 * @brief Toggle the global validation gate at runtime.
 * @param enabled New global enable state.
 * @req REQ-VALID-004
 * @version 2.0.4
 */
void ConstitutionalValidator::set_global_enabled(bool enabled) {
    std::lock_guard<std::mutex> lock(overrides_mutex_);
    global_enabled_ = enabled;
}

/**
 * @brief Set per-identity validation override.
 * @param identity_name Identity name.
 * @param enabled Whether validation is enabled.
 * @internal
 * @version 1.9.8
 */
void ConstitutionalValidator::set_identity_validation(
    const std::string& identity_name, bool enabled) {
    std::lock_guard<std::mutex> lock(overrides_mutex_);
    identity_overrides_[identity_name] = enabled;
}

/**
 * @brief Run the full validation pipeline on generated content.
 * @param content The generated text to validate.
 * @param tier The tier/identity that produced the content.
 * @param messages_json Original conversation context.
 * @return ValidationResult with final content and critique metadata.
 * @internal
 * @version 2.0.0
 */
ValidationResult ConstitutionalValidator::validate(
    const std::string& content,
    const std::string& tier,
    const char* messages_json) {
    ValidationResult result;
    result.content = content;

    if (!should_validate(tier)) {
        logger->info("Validation skipped for tier '{}'", tier);
        store_result(result);
        return result;
    }

    logger->info("Validation start: {} chars, tier='{}'",
                 content.size(), tier);
    result = run_validation_loop(content, tier, messages_json);
    logger->info("Validation {}: {} revision(s)",
                 result.was_revised ? "revised" : "passed",
                 result.revision_count);
    store_result(result);
    return result;
}

/**
 * @brief Get the last validation result.
 * @return Most recent ValidationResult.
 * @internal
 * @version 1.9.8
 */
ValidationResult ConstitutionalValidator::last_result() const {
    std::lock_guard<std::mutex> lock(result_mutex_);
    return last_result_;
}

// ── Hook Callback ────────────────────────────────────────

/**
 * @brief POST_GENERATE hook callback.
 * @param hook_point Hook point (POST_GENERATE).
 * @param context_json JSON context from engine.
 * @param modified_json Output: revised JSON or NULL.
 * @param user_data ValidationContext pointer.
 * @return 0 (post-hooks cannot cancel).
 * @callback
 * @version 1.9.8
 */
int ConstitutionalValidator::hook_callback(
    entropic_hook_point_t /*hook_point*/,
    const char* context_json,
    char** modified_json,
    void* user_data) {
    *modified_json = nullptr;
    auto* ctx = static_cast<ValidationContext*>(user_data);
    if (ctx == nullptr || ctx->validator == nullptr) {
        return 0;
    }

    return ctx->validator->handle_hook(context_json, modified_json);
}

// ── Critique Prompt Assembly ──────────────────────────────

/**
 * @brief Build the critique prompt string.
 * @param content Text to critique.
 * @return Formatted system prompt with constitution + content.
 * @utility
 * @version 1.9.8
 */
std::string ConstitutionalValidator::build_critique_prompt(
    const std::string& content) const {
    std::string prompt;
    prompt.reserve(constitution_text_.size() + content.size() + 256);

    prompt += "You are a constitutional compliance evaluator. "
              "Evaluate the following output against these "
              "constitutional rules. Respond ONLY with the "
              "structured JSON evaluation.\n\n"
              "Constitutional Rules:\n";
    prompt += constitution_text_;
    prompt += "\n\nEvaluate this output for constitutional compliance:"
              "\n\n---\n";
    prompt += content;
    prompt += "\n---";
    return prompt;
}

/**
 * @brief Parse critique JSON into CritiqueResult.
 * @param json_str Raw JSON from grammar-constrained generation.
 * @return Parsed CritiqueResult. On parse failure, returns default
 *         (compliant=true) with raw_json set for diagnostics.
 * @utility
 * @version 2.0.6
 */
CritiqueResult ConstitutionalValidator::parse_critique(
    const std::string& json_str) {
    CritiqueResult result;
    result.raw_json = json_str;

    if (!extract_compliant_field(json_str, result)) {
        // Fail-open: if the model produced JSON we can't parse (e.g.,
        // "constitutional_compliance_status" instead of "compliant"),
        // treat it as compliant rather than triggering a revision loop
        // on a parse error. The grammar constraint is the real fix;
        // this is the safety net when grammar loading fails.
        result.compliant = true;
        return result;
    }

    extract_violations(json_str, result);
    extract_revised_field(json_str, result);
    return result;
}

// ── Private Implementation ────────────────────────────────

/**
 * @brief Store a validation result (thread-safe).
 * @param result Result to store.
 * @internal
 * @version 1.9.8
 */
void ConstitutionalValidator::store_result(
    const ValidationResult& result) {
    std::lock_guard<std::mutex> lock(result_mutex_);
    last_result_ = result;
}

/**
 * @brief Run the core validation loop (critique + revise).
 * @param content Original content.
 * @param tier Identity/tier name.
 * @param messages_json Conversation context.
 * @return ValidationResult after critique/revision.
 * @internal
 * @version 1.9.8
 */
ValidationResult ConstitutionalValidator::run_validation_loop(
    const std::string& content,
    const std::string& /*tier*/,
    const char* messages_json) {
    ValidationResult result;
    result.content = content;

    auto critique = run_critique(content);
    result.final_critique = critique;

    if (critique.compliant) {
        return result;
    }

    return apply_revisions(result, critique, messages_json);
}

/**
 * @brief Apply revision strategy (Path A then Path B).
 * @param result Current validation result (mutated).
 * @param initial_critique First critique result.
 * @param messages_json Conversation context.
 * @return Updated ValidationResult.
 * @internal
 * @version 2.0.0
 */
ValidationResult ConstitutionalValidator::apply_revisions(
    ValidationResult result,
    const CritiqueResult& initial_critique,
    const char* messages_json) {
    if (config_.max_revisions <= 0) {
        return result;
    }

    auto critique = initial_critique;
    for (int i = 0; i < config_.max_revisions; ++i) {
        auto revised = attempt_revision(
            result.content, critique, messages_json);
        result.content = revised;
        result.was_revised = true;
        result.revision_count = i + 1;

        critique = run_critique(revised);
        result.final_critique = critique;

        if (critique.compliant) {
            return result;
        }
    }

    logger->warn("Constitutional validation: max revisions ({}) "
                 "exhausted, returning last output",
                 config_.max_revisions);
    return result;
}

/**
 * @brief Attempt a single revision (Path A or Path B).
 * @param content Content to revise.
 * @param critique Critique with violations.
 * @param messages_json Conversation context for Path B.
 * @return Revised content string.
 * @internal
 * @version 1.9.8
 */
std::string ConstitutionalValidator::attempt_revision(
    const std::string& content,
    const CritiqueResult& critique,
    const char* messages_json) {
    if (!critique.revised.empty()) {
        return critique.revised;
    }
    return revise(content, critique, messages_json);
}

/**
 * @brief Generate a critique of the given content.
 * @param content Text to critique.
 * @return Parsed CritiqueResult.
 * @internal
 * @version 2.0.0
 */
CritiqueResult ConstitutionalValidator::run_critique(
    const std::string& content) {
    if (inference_ == nullptr || inference_->generate == nullptr) {
        logger->warn("Constitutional validation: no inference "
                     "interface, skipping critique");
        return {};
    }

    auto messages = build_critique_messages(content);
    auto params = build_critique_params();
    char* result_json = nullptr;

    int rc = inference_->generate(
        messages.c_str(), params.c_str(),
        &result_json, inference_->backend_data);

    if (rc != 0 || result_json == nullptr) {
        logger->warn("Constitutional validation: critique generation "
                     "failed (rc={})", rc);
        return {};
    }

    std::string raw(result_json);
    if (inference_->free_fn != nullptr) {
        inference_->free_fn(result_json);
    }

    return parse_critique(raw);
}

/**
 * @brief Build messages JSON for critique generation.
 * @param content Text to critique.
 * @return JSON array with system + user messages.
 * @internal
 * @version 1.9.8
 */
std::string ConstitutionalValidator::build_critique_messages(
    const std::string& content) const {
    auto prompt = build_critique_prompt(content);
    return build_single_turn_json(prompt);
}

/**
 * @brief Build params JSON for critique generation.
 * @return JSON string with grammar_key, max_tokens, temperature.
 * @internal
 * @version 1.9.8
 */
std::string ConstitutionalValidator::build_critique_params() const {
    std::string params = "{\"grammar_key\":\"";
    params += config_.grammar_key;
    params += "\",\"max_tokens\":";
    params += std::to_string(config_.max_critique_tokens);
    params += ",\"temperature\":0.0}";
    return params;
}

/**
 * @brief Re-generate with critique feedback injected.
 * @param original The content that was critiqued.
 * @param critique The critique result with violations.
 * @param messages_json Original conversation context.
 * @return Revised content string.
 * @internal
 * @version 2.0.0
 */
std::string ConstitutionalValidator::revise(
    const std::string& original,
    const CritiqueResult& critique,
    const char* messages_json) {
    if (inference_ == nullptr || inference_->generate == nullptr) {
        return original;
    }

    auto augmented = build_revision_messages(
        original, critique, messages_json);
    char* result_json = nullptr;

    int rc = inference_->generate(
        augmented.c_str(), "{}", &result_json,
        inference_->backend_data);

    if (rc != 0 || result_json == nullptr) {
        logger->warn("Constitutional validation: revision generation "
                     "failed (rc={})", rc);
        return original;
    }

    std::string revised(result_json);
    if (inference_->free_fn != nullptr) {
        inference_->free_fn(result_json);
    }
    return revised;
}

// ── JSON String Helpers ──────────────────────────────────
// Manual JSON construction avoids nlohmann/json dependency in core.so.

/**
 * @brief Escape a string for JSON embedding.
 * @param s Raw string.
 * @return JSON-safe string (without surrounding quotes).
 * @utility
 * @version 1.9.8
 */
static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:   out += c; break;
        }
    }
    return out;
}

/**
 * @brief Build a single-turn messages JSON (system + user).
 * @param prompt Full prompt text (system role).
 * @return JSON array string.
 * @internal
 * @version 1.9.8
 */
std::string ConstitutionalValidator::build_single_turn_json(
    const std::string& prompt) const {
    std::string json = "[{\"role\":\"user\",\"content\":\"";
    json += json_escape(prompt);
    json += "\"}]";
    return json;
}

/**
 * @brief Build revision messages JSON with feedback injection.
 * @param original Content that was critiqued.
 * @param critique Critique with violations.
 * @param messages_json Original conversation context.
 * @return Augmented messages JSON.
 * @internal
 * @version 1.9.8
 */
std::string ConstitutionalValidator::build_revision_messages(
    const std::string& original,
    const CritiqueResult& critique,
    const char* messages_json) const {
    std::string feedback = build_feedback_text(critique);
    return inject_feedback_into_messages(
        original, feedback, messages_json);
}

/**
 * @brief Build human-readable feedback text from violations.
 * @param critique Critique result with violations.
 * @return Feedback string for re-generation prompt.
 * @internal
 * @version 1.9.8
 */
std::string ConstitutionalValidator::build_feedback_text(
    const CritiqueResult& critique) const {
    std::string feedback =
        "Your response violated these constitutional rules:\\n";
    for (const auto& v : critique.violations) {
        feedback += "- " + v.rule + ": " + v.explanation + "\\n";
    }
    feedback += "Please revise your response to comply with all "
                "constitutional rules.";
    return feedback;
}

/**
 * @brief Inject feedback into the conversation messages.
 * @param original The assistant's original response.
 * @param feedback The feedback message.
 * @param messages_json Base conversation messages (may be NULL).
 * @return Augmented JSON array with assistant + user feedback appended.
 * @internal
 * @version 2.0.6
 */
std::string ConstitutionalValidator::inject_feedback_into_messages(
    const std::string& original,
    const std::string& feedback,
    const char* messages_json) const {
    std::string base;
    if (messages_json != nullptr) {
        base = std::string(messages_json);
    } else {
        // No conversation context passed — inject the constitution as
        // a system prompt so the revision model stays in character.
        // Without this, the 4B model reverts to its pretrained persona.
        base = "[{\"role\":\"system\",\"content\":\""
             + json_escape(constitution_text_) + "\"}";
    }

    // Strip trailing ]
    if (!base.empty() && base.back() == ']') {
        base.pop_back();
    }

    // Add comma if there were existing messages
    if (base.size() > 1) {
        base += ",";
    }

    base += "{\"role\":\"assistant\",\"content\":\"";
    base += json_escape(original);
    base += "\"},{\"role\":\"system\",\"content\":\"";
    base += json_escape("[CONSTITUTIONAL REVIEW] " + feedback);
    base += "\"}]";
    return base;
}

/**
 * @brief Strip `<think>...</think>` blocks from content.
 *
 * The model's internal reasoning is not subject to constitutional
 * review. Passing think blocks to the critique model causes it to
 * evaluate planning text as if it were claims about the codebase.
 *
 * @param content Raw model output.
 * @return Content with think blocks removed.
 * @utility
 * @version 2.0.6
 */
static std::string strip_think_blocks(const std::string& content) {
    std::string result;
    size_t pos = 0;
    while (pos < content.size()) {
        auto open = content.find("<think>", pos);
        if (open == std::string::npos) {
            result.append(content, pos);
            break;
        }
        result.append(content, pos, open - pos);
        auto close = content.find("</think>", open);
        pos = (close == std::string::npos)
            ? content.size()
            : close + 8;
    }
    return result;
}

/**
 * @brief Check if content is a pure tool call with no prose.
 *
 * Tool-call-only outputs contain no prose claims that constitutional
 * rules can evaluate. Validating them wastes inference and risks
 * false non-compliance on XML syntax.
 *
 * @param content Content with think blocks already stripped.
 * @return true if content is only whitespace and tool_call XML.
 * @utility
 * @version 2.0.6
 */
static bool is_pure_tool_call(const std::string& content) {
    std::string stripped = content;
    // Remove all <tool_call>...</tool_call> blocks
    while (true) {
        auto open = stripped.find("<tool_call>");
        if (open == std::string::npos) { break; }
        auto close = stripped.find("</tool_call>", open);
        if (close == std::string::npos) { break; }
        stripped.erase(open, close + 12 - open);
    }
    // If only whitespace remains, it was a pure tool call
    return stripped.find_first_not_of(" \t\n\r") == std::string::npos;
}

/**
 * @brief Handle the POST_GENERATE hook callback.
 * @param context_json JSON context from engine.
 * @param modified_json Output: revised JSON or NULL.
 * @return 0 (post-hooks cannot cancel).
 * @internal
 * @version 2.0.6
 */
int ConstitutionalValidator::handle_hook(
    const char* context_json,
    char** modified_json) {
    *modified_json = nullptr;

    auto raw = extract_json_string(context_json, "content");
    auto tier = extract_json_string(context_json, "tier");

    // Strip think blocks — internal reasoning is not reviewable.
    auto content = strip_think_blocks(raw);

    // Skip validation on pure tool-call outputs — no prose to review.
    if (content.empty() || is_pure_tool_call(content)) {
        return 0;
    }

    auto result = validate(content, tier, nullptr);
    if (!result.was_revised) {
        return 0;
    }

    write_modified_json(result.content, modified_json);
    return 0;
}

/**
 * @brief Write revised content into the modified_json output.
 * @param content Revised content.
 * @param modified_json Output buffer.
 * @internal
 * @version 1.9.8
 */
void ConstitutionalValidator::write_modified_json(
    const std::string& content,
    char** modified_json) {
    std::string out = "{\"content\":\"";
    out += json_escape(content);
    out += "\"}";

    auto* buf = static_cast<char*>(malloc(out.size() + 1));
    if (buf != nullptr) {
        std::memcpy(buf, out.c_str(), out.size() + 1);
        *modified_json = buf;
    }
}

// ── Minimal JSON Field Extraction ─────────────────────────
// No nlohmann/json in core.so. These extract known fields from
// simple flat JSON objects by string search.

/**
 * @brief Extract a string value for a given key from flat JSON.
 * @param json JSON string.
 * @param key Key name (without quotes).
 * @return Extracted value, or empty string if not found.
 * @utility
 * @version 1.9.8
 */
std::string ConstitutionalValidator::extract_json_string(
    const char* json, const char* key) {
    if (json == nullptr || key == nullptr) {
        return {};
    }

    std::string needle = std::string("\"") + key + "\"";
    const char* pos = strstr(json, needle.c_str());
    if (pos == nullptr) {
        return {};
    }

    return extract_string_after_colon(pos + needle.size());
}

/**
 * @brief Extract the string value after a colon in JSON.
 * @param pos Position after the key in JSON.
 * @return Extracted string content (unescaped).
 * @utility
 * @version 1.9.8
 */
std::string ConstitutionalValidator::extract_string_after_colon(
    const char* pos) {
    // Skip whitespace and colon
    while (*pos == ' ' || *pos == ':' || *pos == '\t') {
        ++pos;
    }
    if (*pos != '"') {
        return {};
    }
    ++pos; // skip opening quote

    std::string value;
    while (*pos != '\0' && *pos != '"') {
        if (*pos == '\\' && *(pos + 1) != '\0') {
            ++pos;
            switch (*pos) {
                case 'n': value += '\n'; break;
                case 't': value += '\t'; break;
                case 'r': value += '\r'; break;
                default:  value += *pos; break;
            }
        } else {
            value += *pos;
        }
        ++pos;
    }
    return value;
}

/**
 * @brief Extract the "compliant" boolean field from critique JSON.
 * @param json Raw JSON string.
 * @param result CritiqueResult to populate.
 * @return true if field was found and parsed.
 * @utility
 * @version 2.0.0
 */
bool ConstitutionalValidator::extract_compliant_field(
    const std::string& json, CritiqueResult& result) {
    auto pos = json.find("\"compliant\"");
    if (pos == std::string::npos) {
        logger->warn("Constitutional validation: malformed critique "
                     "JSON — missing 'compliant' field");
        return false;
    }

    result.compliant = (json.find("true", pos) < json.find("false", pos));
    return true;
}

/**
 * @brief Extract violations array from critique JSON.
 * @param json Raw JSON string.
 * @param result CritiqueResult to populate.
 * @utility
 * @version 1.9.8
 */
void ConstitutionalValidator::extract_violations(
    const std::string& json, CritiqueResult& result) {
    size_t search_pos = 0;

    while (true) {
        auto v = extract_next_violation(json, search_pos);
        if (!v.has_value()) {
            break;
        }
        result.violations.push_back(std::move(v.value()));
    }
}

/**
 * @brief Extract the next violation object from JSON.
 * @param json Full JSON string.
 * @param pos Search position (updated on success).
 * @return Violation if found, nullopt if no more.
 * @utility
 * @version 1.9.8
 */
std::optional<Violation>
ConstitutionalValidator::extract_next_violation(
    const std::string& json, size_t& pos) {
    auto rule_pos = json.find("\"rule\"", pos);
    if (rule_pos == std::string::npos) {
        return std::nullopt;
    }

    Violation v;
    v.rule = extract_json_string(
        json.c_str() + rule_pos, "rule");
    v.excerpt = extract_json_string(
        json.c_str() + rule_pos, "excerpt");
    v.explanation = extract_json_string(
        json.c_str() + rule_pos, "explanation");

    pos = rule_pos + 1;
    return v;
}

/**
 * @brief Extract the "revised" string field from critique JSON.
 * @param json Raw JSON string.
 * @param result CritiqueResult to populate.
 * @utility
 * @version 1.9.8
 */
void ConstitutionalValidator::extract_revised_field(
    const std::string& json, CritiqueResult& result) {
    result.revised = extract_json_string(json.c_str(), "revised");
}

} // namespace entropic
