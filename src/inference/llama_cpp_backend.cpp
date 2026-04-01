/**
 * @file llama_cpp_backend.cpp
 * @brief LlamaCppBackend implementation — direct llama.cpp C API.
 *
 * Pinned against llama.cpp submodule b8420. Uses:
 * - llama_model_load_from_file() for model loading
 * - llama_init_from_model() for context creation
 * - llama_decode() + llama_batch for token processing
 * - llama_sampler_chain for sampling
 * - llama_chat_apply_template() for chat formatting
 *
 * @version 1.8.3
 */

#include "llama_cpp_backend.h"

#include <entropic/types/logging.h>

#include <cmath>
#include <cstring>
#include <stdexcept>

namespace entropic {

namespace {

auto logger = entropic::log::get("inference.llama_cpp");

/**
 * @brief Check if generated text ends with any stop sequence.
 * @param text Generated text so far.
 * @param stop_sequences Stop sequence list.
 * @return true if any stop sequence found at end of text.
 * @utility
 * @version 1.8.2
 */
bool ends_with(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size()
        && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/**
 * @brief Check if generated text ends with any stop sequence.
 * @param text Generated text so far.
 * @param stop_sequences List of stop sequences to check.
 * @return true if any stop sequence found at end of text.
 * @utility
 * @version 1.8.2
 */
bool check_stop_sequences(
    const std::string& text,
    const std::vector<std::string>& stop_sequences)
{
    for (const auto& stop : stop_sequences) {
        if (!stop.empty() && ends_with(text, stop)) {
            return true;
        }
    }
    return false;
}

} // anonymous namespace

// ── Lifecycle ──────────────────────────────────────────────

/**
 * @brief Load model into CPU RAM (COLD → WARM).
 *
 * Uses llama_model_load_from_file with n_gpu_layers=0 for CPU-only
 * mmap+mlock loading. Model stays in page cache for fast reactivation.
 *
 * @param config Validated model config.
 * @return true on success.
 * @internal
 * @version 1.8.2
 */
bool LlamaCppBackend::do_load(const ModelConfig& config) {
    llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = 0;
    mparams.use_mmap = true;
    mparams.use_mlock = config.use_mlock;

    model_ = llama_model_load_from_file(config.path.c_str(), mparams);
    if (!model_) {
        last_error_ = "llama_model_load_from_file failed: " + config.path.string();
        return false;
    }

    vocab_ = llama_model_get_vocab(model_);
    logger->info("Model loaded (CPU): {} tokens in vocab",
              llama_vocab_n_tokens(vocab_));
    return true;
}

/**
 * @brief Activate model on GPU (WARM → ACTIVE).
 *
 * Reloads model with n_gpu_layers from config, then creates
 * inference context with KV cache.
 *
 * @return true on success.
 * @internal
 * @version 1.8.2
 */
bool LlamaCppBackend::do_activate() {
    // Reload model with GPU layers
    llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = config().gpu_layers;
    mparams.use_mmap = true;
    mparams.use_mlock = config().use_mlock;

    if (!config().tensor_split.empty()) {
        // TODO: parse tensor_split string into float array for multi-GPU
        logger->warn("tensor_split not yet implemented, ignoring");
    }

    llama_model* new_model = llama_model_load_from_file(
        config().path.c_str(), mparams);
    if (!new_model) {
        last_error_ = "Failed to reload model with GPU layers";
        return false;
    }

    // Free old CPU-only model, replace with GPU model
    if (model_) {
        llama_model_free(model_);
    }
    model_ = new_model;
    vocab_ = llama_model_get_vocab(model_);

    // Create inference context
    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = static_cast<uint32_t>(config().context_length);
    cparams.n_batch = static_cast<uint32_t>(config().n_batch);
    cparams.n_threads = config().n_threads > 0
        ? static_cast<uint32_t>(config().n_threads)
        : std::thread::hardware_concurrency();
    cparams.flash_attn_type = config().flash_attn
        ? LLAMA_FLASH_ATTN_TYPE_ENABLED
        : LLAMA_FLASH_ATTN_TYPE_DISABLED;

    ctx_ = llama_init_from_model(model_, cparams);
    if (!ctx_) {
        last_error_ = "llama_init_from_model failed";
        return false;
    }

    logger->info("Context created: n_ctx={}, n_batch={}, flash_attn={}",
              config().context_length, config().n_batch, config().flash_attn);

    // Initialize prompt cache if not already created
    if (!prompt_cache_) {
        prompt_cache_ = std::make_unique<PromptCache>(
            prompt_cache_config_.max_bytes);
        logger->info("Prompt cache initialized: max_bytes={}",
                     prompt_cache_config_.max_bytes);
    }

    return true;
}

/**
 * @brief Deactivate: free context, reload model CPU-only.
 * @internal
 * @version 1.8.2
 */
void LlamaCppBackend::do_deactivate() {
    if (ctx_) {
        llama_free(ctx_);
        ctx_ = nullptr;
    }

    // Reload model CPU-only for WARM state
    llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = 0;
    mparams.use_mmap = true;
    mparams.use_mlock = config().use_mlock;

    llama_model* cpu_model = llama_model_load_from_file(
        config().path.c_str(), mparams);
    if (cpu_model) {
        llama_model_free(model_);
        model_ = cpu_model;
        vocab_ = llama_model_get_vocab(model_);
    } else {
        logger->warn("Failed to reload CPU model during deactivate, keeping GPU model");
    }
}

/**
 * @brief Full unload — free all resources, clear prompt cache.
 * @internal
 * @version 1.8.3
 */
void LlamaCppBackend::do_unload() {
    if (prompt_cache_) {
        prompt_cache_->clear();
    }
    if (ctx_) {
        llama_free(ctx_);
        ctx_ = nullptr;
    }
    if (model_) {
        llama_model_free(model_);
        model_ = nullptr;
    }
    vocab_ = nullptr;
}

// ── Tokenization ───────────────────────────────────────────

/**
 * @brief Tokenize text using model vocabulary.
 * @param text Input text.
 * @param add_special Add BOS/EOS special tokens.
 * @return Vector of token IDs.
 * @internal
 * @version 1.8.2
 */
std::vector<llama_token> LlamaCppBackend::tokenize(
    const std::string& text, bool add_special) const
{
    // First call: get required size
    int n = llama_tokenize(vocab_, text.c_str(),
                           static_cast<int32_t>(text.size()),
                           nullptr, 0, add_special, true);
    if (n < 0) {
        n = -n;
    }

    std::vector<llama_token> tokens(static_cast<size_t>(n));
    int actual = llama_tokenize(vocab_, text.c_str(),
                                static_cast<int32_t>(text.size()),
                                tokens.data(), n, add_special, true);
    if (actual < 0) {
        logger->error("Tokenization failed for text of length {}", text.size());
        return {};
    }
    tokens.resize(static_cast<size_t>(actual));
    return tokens;
}

/**
 * @brief Detokenize a single token to string.
 * @param token Token ID.
 * @return String representation.
 * @internal
 * @version 1.8.2
 */
std::string LlamaCppBackend::detokenize(llama_token token) const {
    char buf[256];
    int n = llama_token_to_piece(vocab_, token, buf, sizeof(buf), 0, true);
    if (n < 0) {
        // Buffer too small — retry with exact size
        std::vector<char> large(static_cast<size_t>(-n));
        n = llama_token_to_piece(vocab_, token, large.data(),
                                 static_cast<int32_t>(large.size()), 0, true);
        if (n > 0) {
            return std::string(large.data(), static_cast<size_t>(n));
        }
        return "";
    }
    return std::string(buf, static_cast<size_t>(n));
}

/**
 * @brief Count tokens in text.
 * @param text Input text.
 * @return Token count.
 * @internal
 * @version 1.8.2
 */
int LlamaCppBackend::do_count_tokens(const std::string& text) const {
    auto tokens = tokenize(text, false);
    return static_cast<int>(tokens.size());
}

// ── Evaluation (v1.9.10) ──────────────────────────────────

/**
 * @brief Evaluate per-token log-probabilities (stub).
 *
 * Full implementation will use llama_decode with logits_all=true
 * on a temporary seq_id. Stubbed until Phase 3 wiring.
 *
 * @param tokens Token IDs to evaluate.
 * @param n_tokens Number of tokens.
 * @return LogprobResult — stub throws not-implemented.
 * @internal
 * @version 1.9.10
 */
LogprobResult LlamaCppBackend::do_evaluate_logprobs(
    const int32_t* /*tokens*/,
    int /*n_tokens*/)
{
    throw std::runtime_error(
        "LlamaCppBackend::do_evaluate_logprobs not yet implemented");
}

/**
 * @brief Allocate a temporary sequence ID for evaluation (stub).
 * @return -1 — not yet implemented.
 * @internal
 * @version 1.9.10
 */
llama_seq_id LlamaCppBackend::allocate_temp_seq_id() {
    return -1;
}

/**
 * @brief Release a temporary sequence ID (stub).
 * @param seq_id The seq_id to release.
 * @internal
 * @version 1.9.10
 */
void LlamaCppBackend::release_temp_seq_id(llama_seq_id /*seq_id*/) {
}

/**
 * @brief Extract log-probability for a token from logits.
 *
 * Uses numerically stable log-softmax:
 *   log P(t) = logits[t] - max - log(sum(exp(logits - max)))
 *
 * @param logits Raw logits array.
 * @param next_token Token to score.
 * @param n_vocab Vocabulary size.
 * @return log P(next_token | context).
 * @internal
 * @version 1.9.10
 */
float LlamaCppBackend::extract_token_logprob(
    const float* logits,
    int32_t next_token,
    int n_vocab)
{
    float max_logit = logits[0];
    for (int v = 1; v < n_vocab; v++) {
        if (logits[v] > max_logit) {
            max_logit = logits[v];
        }
    }
    float sum_exp = 0.0f;
    for (int v = 0; v < n_vocab; v++) {
        sum_exp += std::exp(logits[v] - max_logit);
    }
    float log_sum_exp = max_logit + std::log(sum_exp);
    return logits[next_token] - log_sum_exp;
}

// ── Chat template ──────────────────────────────────────────

/**
 * @brief Apply GGUF-embedded chat template to messages.
 *
 * Uses llama_chat_apply_template() which reads the template from
 * GGUF metadata. Passes enable_thinking as a template variable
 * via the add_generation_prompt parameter.
 *
 * @param messages Conversation history.
 * @param params Generation parameters.
 * @return Formatted prompt string.
 * @internal
 * @version 1.8.2
 */
std::string LlamaCppBackend::apply_chat_template(
    const std::vector<Message>& messages,
    const GenerationParams& params) const
{
    // Convert to llama_chat_message array
    std::vector<llama_chat_message> chat_msgs;
    chat_msgs.reserve(messages.size());
    for (const auto& msg : messages) {
        chat_msgs.push_back({msg.role.c_str(), msg.content.c_str()});
    }

    // First call: measure required buffer size
    int n = llama_chat_apply_template(
        nullptr, chat_msgs.data(), chat_msgs.size(),
        true, nullptr, 0);
    if (n < 0) {
        logger->error("llama_chat_apply_template failed (size query)");
        // Fallback: concatenate messages directly
        std::string fallback;
        for (const auto& msg : messages) {
            fallback += msg.role + ": " + msg.content + "\n";
        }
        return fallback;
    }

    std::vector<char> buf(static_cast<size_t>(n + 1));
    int written = llama_chat_apply_template(
        nullptr, chat_msgs.data(), chat_msgs.size(),
        true, buf.data(), static_cast<int32_t>(buf.size()));
    if (written < 0) {
        logger->error("llama_chat_apply_template failed (render)");
        return "";
    }

    return std::string(buf.data(), static_cast<size_t>(written));
}

// ── Sampler ────────────────────────────────────────────────

/**
 * @brief Create sampler chain from generation params.
 *
 * Chain order per llama.cpp convention:
 * grammar → penalties → temperature → top-k → top-p → dist
 *
 * @param params Generation parameters.
 * @return Sampler chain (caller frees).
 * @internal
 * @version 1.8.2
 */
llama_sampler* LlamaCppBackend::create_sampler(
    const GenerationParams& params) const
{
    llama_sampler_chain_params chain_params = llama_sampler_chain_default_params();
    llama_sampler* chain = llama_sampler_chain_init(chain_params);

    // Grammar constraint (applied first)
    if (!params.grammar.empty()) {
        llama_sampler* grammar = llama_sampler_init_grammar(
            vocab_, params.grammar.c_str(), "root");
        if (grammar) {
            llama_sampler_chain_add(chain, grammar);
        }
    }

    // Repetition penalty
    if (params.repeat_penalty != 1.0f) {
        llama_sampler_chain_add(chain,
            llama_sampler_init_penalties(
                64, params.repeat_penalty, 0.0f, 0.0f));
    }

    // Temperature
    if (params.temperature > 0.0f) {
        llama_sampler_chain_add(chain,
            llama_sampler_init_temp(params.temperature));
    }

    // Top-K
    if (params.top_k > 0) {
        llama_sampler_chain_add(chain,
            llama_sampler_init_top_k(params.top_k));
    }

    // Top-P (nucleus sampling)
    if (params.top_p < 1.0f) {
        llama_sampler_chain_add(chain,
            llama_sampler_init_top_p(params.top_p, 1));
    }

    // Final distribution sampler
    llama_sampler_chain_add(chain, llama_sampler_init_dist(0));

    return chain;
}

// ── Decode loop ────────────────────────────────────────────

/**
 * @brief Run batched prefill on input tokens.
 * @param tokens Input token sequence.
 * @return true on success.
 * @internal
 * @version 1.8.2
 */
bool LlamaCppBackend::run_prefill(const std::vector<llama_token>& tokens) {
    llama_memory_clear(llama_get_memory(ctx_), true);

    std::vector<llama_token> input(tokens);
    llama_batch batch = llama_batch_get_one(
        input.data(), static_cast<int32_t>(input.size()));

    bool ok = (llama_decode(ctx_, batch) == 0);
    if (!ok) {
        logger->error("Prefill decode failed");
    }
    return ok;
}

/**
 * @brief Generate one token and append to output.
 * @param sampler Sampler chain.
 * @param generated Accumulated output string (mutated).
 * @param on_token Streaming callback (may be nullptr).
 * @param stop Stop sequences.
 * @return "continue", "stop", "eos", or "error".
 * @internal
 * @version 1.8.2
 */
std::string LlamaCppBackend::step_token(
    llama_sampler* sampler,
    std::string& generated,
    std::function<void(std::string_view)>& on_token,
    const std::vector<std::string>& stop)
{
    llama_token new_token = llama_sampler_sample(sampler, ctx_, -1);

    if (new_token == llama_vocab_eos(vocab_)
        || llama_vocab_is_eog(vocab_, new_token)) {
        return "eos";
    }

    std::string piece = detokenize(new_token);
    generated += piece;
    if (on_token) {
        on_token(std::string_view(piece));
    }
    if (check_stop_sequences(generated, stop)) {
        return "stop";
    }

    llama_token tok = new_token;
    llama_batch single = llama_batch_get_one(&tok, 1);
    return (llama_decode(ctx_, single) == 0) ? "continue" : "error";
}

/**
 * @brief Core decode loop shared by generate, streaming, and complete.
 * @param tokens Input token sequence.
 * @param params Generation parameters.
 * @param on_token Per-token callback (nullptr for batch).
 * @param cancel Cancel flag (nullptr for batch).
 * @return GenerationResult.
 * @internal
 * @version 1.8.2
 */
GenerationResult LlamaCppBackend::decode_loop(
    const std::vector<llama_token>& tokens,
    const GenerationParams& params,
    std::function<void(std::string_view)> on_token,
    std::atomic<bool>* cancel)
{
    GenerationResult result;
    llama_sampler* sampler = create_sampler(params);

    if (!run_prefill(tokens)) {
        result.error_code = ENTROPIC_ERROR_GENERATE_FAILED;
        result.error_message = "Prefill decode failed";
        result.finish_reason = "error";
        llama_sampler_free(sampler);
        return result;
    }

    std::string generated;
    int n_generated = 0;

    while (n_generated < params.max_tokens) {
        bool cancelled = cancel && cancel->load(std::memory_order_acquire);
        if (cancelled) {
            result.finish_reason = "cancelled";
            result.error_code = ENTROPIC_ERROR_CANCELLED;
            break;
        }

        auto status = step_token(sampler, generated, on_token, params.stop);
        if (status == "continue") {
            ++n_generated;
        } else {
            result.finish_reason = (status == "error") ? "error" : "stop";
            if (status == "error") {
                result.error_code = ENTROPIC_ERROR_GENERATE_FAILED;
            }
            break;
        }
    }

    if (n_generated >= params.max_tokens && result.finish_reason.empty()) {
        result.finish_reason = "length";
    }

    llama_sampler_free(sampler);
    result.content = generated;
    result.token_count = n_generated;
    return result;
}

// ── Prompt cache helpers ───────────────────────────────────

/**
 * @brief Extract system prompt text from message list.
 * @param messages Conversation history.
 * @return System message content, empty if none found.
 * @internal
 * @version 1.8.3
 */
std::string LlamaCppBackend::extract_system_prompt(
    const std::vector<Message>& messages)
{
    for (const auto& msg : messages) {
        if (msg.role == "system") {
            return msg.content;
        }
    }
    return "";
}

/**
 * @brief Restore cached KV prefix and decode remaining tokens.
 * @param cached Cache entry to restore from.
 * @param tokens Full token sequence.
 * @return true on success, false to fall back to full prefill.
 * @internal
 * @version 1.8.3
 */
bool LlamaCppBackend::restore_cached_prefix(
    const CacheEntry* cached,
    const std::vector<llama_token>& tokens)
{
    llama_memory_clear(llama_get_memory(ctx_), true);

    size_t restored = llama_state_seq_set_data(
        ctx_, cached->data.data(), cached->data_size, 0);
    if (restored == 0) {
        logger->warn("KV state restore failed, falling back to full prefill");
        return false;
    }

    int prefix_len = cached->token_count;
    if (prefix_len >= static_cast<int>(tokens.size())) {
        return true;
    }

    std::vector<llama_token> remaining(
        tokens.begin() + prefix_len, tokens.end());
    llama_batch batch = llama_batch_get_one(
        remaining.data(), static_cast<int32_t>(remaining.size()));

    bool ok = (llama_decode(ctx_, batch) == 0);
    if (!ok) {
        logger->error("Decode of remaining tokens after cache restore failed");
    }
    return ok;
}

/**
 * @brief Save system prefix KV state to the prompt cache.
 * @param key Cache key for the prefix.
 * @param prefix_tokens Number of system prefix tokens.
 * @internal
 * @version 1.8.3
 */
void LlamaCppBackend::save_prefix_to_cache(
    const CacheKey& key, int prefix_tokens)
{
    size_t state_size = llama_state_seq_get_size(ctx_, 0);
    if (state_size == 0) {
        return;
    }

    std::vector<uint8_t> buf(state_size);
    size_t written = llama_state_seq_get_data(
        ctx_, buf.data(), buf.size(), 0);
    if (written > 0) {
        buf.resize(written);
        prompt_cache_->store(key, std::move(buf), prefix_tokens);
    }
}

/**
 * @brief Compute system prefix token count from messages.
 * @param messages Original message list.
 * @param params Generation params (for template).
 * @return Token count of the system prefix, 0 if no system message.
 * @internal
 * @version 1.8.3
 */
int LlamaCppBackend::compute_prefix_token_count(
    const std::vector<Message>& messages,
    const GenerationParams& params)
{
    std::vector<Message> sys_msgs;
    for (const auto& msg : messages) {
        if (msg.role == "system") {
            sys_msgs.push_back(msg);
        }
    }
    if (sys_msgs.empty()) {
        return 0;
    }

    std::string sys_prompt = apply_chat_template(sys_msgs, params);
    auto sys_tokens = tokenize(sys_prompt, true);
    return static_cast<int>(sys_tokens.size());
}

/**
 * @brief Run prefill with prompt cache integration.
 *
 * On cache hit: restores KV state, decodes remaining tokens.
 * On cache miss: full prefill, then saves prefix to cache.
 *
 * @param tokens Full token sequence.
 * @param system_prompt System prompt text for cache key.
 * @param messages Original messages (for prefix boundary).
 * @param params Generation parameters.
 * @return true on success.
 * @internal
 * @version 1.8.3
 */
bool LlamaCppBackend::run_prefill_cached(
    const std::vector<llama_token>& tokens,
    const std::string& system_prompt,
    const std::vector<Message>& messages,
    const GenerationParams& params)
{
    bool cache_enabled = prompt_cache_
        && prompt_cache_config_.enabled
        && !system_prompt.empty();

    // Try cache restore if enabled
    bool restored = false;
    CacheKey key{0};

    if (cache_enabled) {
        key = PromptCache::make_key(
            system_prompt, config().path.string());
        const CacheEntry* cached = prompt_cache_->lookup(key);

        if (cached) {
            if (prompt_cache_config_.log_hits) {
                logger->info("Prompt cache HIT: {} bytes, {} prefix tokens",
                             cached->data_size, cached->token_count);
            }
            restored = restore_cached_prefix(cached, tokens);
        } else if (prompt_cache_config_.log_hits) {
            logger->info("Prompt cache MISS: processing full prompt");
        }
    }

    // Fall back to full prefill if not restored from cache
    if (!restored && !run_prefill(tokens)) {
        return false;
    }

    // Save prefix to cache on miss
    if (cache_enabled && !restored) {
        int prefix_tokens = compute_prefix_token_count(messages, params);
        if (prefix_tokens > 0) {
            save_prefix_to_cache(key, prefix_tokens);
        }
    }
    return true;
}

// ── Generation entry points ────────────────────────────────

/**
 * @brief Generate a complete response using chat template.
 * @param messages Conversation history.
 * @param params Generation parameters.
 * @return GenerationResult.
 * @internal
 * @version 1.8.3
 */
GenerationResult LlamaCppBackend::do_generate(
    const std::vector<Message>& messages,
    const GenerationParams& params)
{
    std::string prompt = apply_chat_template(messages, params);
    auto tokens = tokenize(prompt, true);
    std::string sys = extract_system_prompt(messages);

    logger->info("Generate: {} input tokens, max_tokens={}",
              tokens.size(), params.max_tokens);

    GenerationResult result;
    llama_sampler* sampler = create_sampler(params);

    if (!run_prefill_cached(tokens, sys, messages, params)) {
        result.error_code = ENTROPIC_ERROR_GENERATE_FAILED;
        result.error_message = "Prefill decode failed";
        result.finish_reason = "error";
        llama_sampler_free(sampler);
        return result;
    }

    std::string generated;
    int n_generated = 0;
    std::function<void(std::string_view)> no_cb = nullptr;

    while (n_generated < params.max_tokens) {
        auto status = step_token(sampler, generated, no_cb, params.stop);
        if (status == "continue") {
            ++n_generated;
        } else {
            result.finish_reason = (status == "error") ? "error" : "stop";
            if (status == "error") {
                result.error_code = ENTROPIC_ERROR_GENERATE_FAILED;
            }
            break;
        }
    }

    if (n_generated >= params.max_tokens && result.finish_reason.empty()) {
        result.finish_reason = "length";
    }

    llama_sampler_free(sampler);
    result.content = generated;
    result.token_count = n_generated;
    return result;
}

/**
 * @brief Streaming generation with per-token callback.
 * @param messages Conversation history.
 * @param params Generation parameters.
 * @param on_token Per-token callback.
 * @param cancel Atomic cancel flag.
 * @return GenerationResult.
 * @internal
 * @version 1.8.3
 */
GenerationResult LlamaCppBackend::do_generate_streaming(
    const std::vector<Message>& messages,
    const GenerationParams& params,
    std::function<void(std::string_view token)> on_token,
    std::atomic<bool>& cancel)
{
    std::string prompt = apply_chat_template(messages, params);
    auto tokens = tokenize(prompt, true);
    std::string sys = extract_system_prompt(messages);

    logger->info("Stream: {} input tokens, max_tokens={}",
              tokens.size(), params.max_tokens);

    GenerationResult result;
    llama_sampler* sampler = create_sampler(params);

    if (!run_prefill_cached(tokens, sys, messages, params)) {
        result.error_code = ENTROPIC_ERROR_GENERATE_FAILED;
        result.error_message = "Prefill decode failed";
        result.finish_reason = "error";
        llama_sampler_free(sampler);
        return result;
    }

    std::string generated;
    int n_generated = 0;

    while (n_generated < params.max_tokens) {
        bool cancelled = cancel.load(std::memory_order_acquire);
        if (cancelled) {
            result.finish_reason = "cancelled";
            result.error_code = ENTROPIC_ERROR_CANCELLED;
            break;
        }

        auto status = step_token(sampler, generated, on_token, params.stop);
        if (status == "continue") {
            ++n_generated;
        } else {
            result.finish_reason = (status == "error") ? "error" : "stop";
            if (status == "error") {
                result.error_code = ENTROPIC_ERROR_GENERATE_FAILED;
            }
            break;
        }
    }

    if (n_generated >= params.max_tokens && result.finish_reason.empty()) {
        result.finish_reason = "length";
    }

    llama_sampler_free(sampler);
    result.content = generated;
    result.token_count = n_generated;
    return result;
}

/**
 * @brief Raw text completion without chat template.
 * @param prompt Raw prompt string.
 * @param params Generation parameters.
 * @return GenerationResult.
 * @internal
 * @version 1.8.2
 */
GenerationResult LlamaCppBackend::do_complete(
    const std::string& prompt,
    const GenerationParams& params)
{
    auto tokens = tokenize(prompt, false);

    logger->info("Complete: {} input tokens, max_tokens={}",
              tokens.size(), params.max_tokens);
    return decode_loop(tokens, params, nullptr, nullptr);
}

} // namespace entropic
