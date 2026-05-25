// SPDX-License-Identifier: Apache-2.0
/**
 * @file inference_c_api.cpp
 * @brief C API wrappers for InferenceBackend.
 *
 * Thin bridge between the pure C interface (i_inference_backend.h)
 * and the C++ implementation. Catches all exceptions at the boundary.
 *
 * @version 1.8.2
 */

#include <entropic/interfaces/i_inference_backend.h>
#include <entropic/entropic_export.h>
#include <entropic/types/logging.h>
#include <entropic/types/messages_json.h>

#include "llama_cpp_backend.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>

namespace {

auto logger = entropic::log::get("inference.c_api");

/**
 * @brief Cast opaque handle to C++ backend pointer.
 * @param h Opaque handle.
 * @return Backend pointer.
 * @internal
 * @version 1.8.2
 */
entropic::InferenceBackend* to_backend(entropic_inference_backend_t h) {
    return reinterpret_cast<entropic::InferenceBackend*>(h);
}

/**
 * @brief Allocate a C string copy (caller must free).
 * @param s Source string.
 * @return Heap-allocated copy.
 * @utility
 * @version 1.8.2
 */
char* alloc_string(const std::string& s) {
    char* buf = static_cast<char*>(std::malloc(s.size() + 1));
    if (buf) {
        std::memcpy(buf, s.c_str(), s.size() + 1);
    }
    return buf;
}

/**
 * @brief Parse ModelConfig from JSON string.
 * @param json_str JSON config string.
 * @return Parsed ModelConfig.
 * @internal
 * @version 1.8.2
 */
/**
 * @brief Assign j[key] to out if present (typed by out).
 * @param j Parsed JSON.
 * @param key Field name.
 * @param[out] out Destination; unchanged when key absent.
 * @utility
 * @version 2.3.7
 */
template <typename T>
static void set_if(const nlohmann::json& j, const char* key, T& out) {
    if (j.contains(key)) { out = j[key].get<T>(); }
}

/**
 * @brief Parse a ModelConfig from a JSON config string.
 * @internal
 * @version 2.3.7
 */
entropic::ModelConfig parse_config_json(const char* json_str) {
    entropic::ModelConfig config;
    auto j = nlohmann::json::parse(json_str);

    set_if(j, "path", config.path);
    set_if(j, "adapter", config.adapter);
    set_if(j, "context_length", config.context_length);
    set_if(j, "gpu_layers", config.gpu_layers);
    set_if(j, "keep_warm", config.keep_warm);
    set_if(j, "use_mlock", config.use_mlock);
    set_if(j, "n_batch", config.n_batch);
    set_if(j, "n_threads", config.n_threads);
    set_if(j, "flash_attn", config.flash_attn);

    return config;
}

/**
 * @brief Parse GenerationParams from JSON string.
 * @param json_str JSON params string.
 * @return Parsed GenerationParams.
 * @internal
 * @version 1.8.2
 */
entropic::GenerationParams parse_params_json(const char* json_str) {
    entropic::GenerationParams params;
    auto j = nlohmann::json::parse(json_str);

    if (j.contains("temperature"))    params.temperature = j["temperature"].get<float>();
    if (j.contains("top_p"))          params.top_p = j["top_p"].get<float>();
    if (j.contains("top_k"))          params.top_k = j["top_k"].get<int>();
    if (j.contains("min_p"))          params.min_p = j["min_p"].get<float>();
    if (j.contains("repeat_penalty")) params.repeat_penalty = j["repeat_penalty"].get<float>();
    if (j.contains("max_tokens"))     params.max_tokens = j["max_tokens"].get<int>();
    if (j.contains("grammar"))        params.grammar = j["grammar"].get<std::string>();

    return params;
}

/**
 * @brief Serialize GenerationResult to JSON string.
 * @param result Generation result.
 * @return JSON string.
 * @internal
 * @version 1.8.2
 */
std::string serialize_result_json(const entropic::GenerationResult& result) {
    nlohmann::json j;
    j["content"] = result.content;
    j["finish_reason"] = result.finish_reason;
    j["token_count"] = result.token_count;
    j["generation_time_ms"] = result.generation_time_ms;
    j["error_code"] = static_cast<int>(result.error_code);
    j["error_message"] = result.error_message;
    return j.dump();
}

/* parse_content_part + parse_messages_json moved to the shared
 * utility include/entropic/types/messages_json.h (v2.1.8, gh#37) so
 * the facade's entropic_run_messages can reuse them. Calls below
 * dispatch to entropic::parse_messages_json directly. */

} // anonymous namespace

// ── C API Implementation ───────────────────────────────────

extern "C" {

/**
 * @brief Plugin C API: load a model into the inference backend.
 * @param backend Opaque backend handle from entropic_create_inference_backend().
 * @param config_json JSON-serialized ModelConfig string.
 * @return ENTROPIC_OK on success, ENTROPIC_ERROR_LOAD_FAILED otherwise.
 * @req REQ-INFER-017
 * @version 2.0.0
 */
ENTROPIC_EXPORT entropic_error_t entropic_inference_load(
    entropic_inference_backend_t backend,
    const char* config_json)
{
    // v2.3.10: null-handle guard. Pre-v2.3.10 to_backend(nullptr)
    // returned nullptr and the next ->load() dereferenced it, taking
    // the whole process down with SIGSEGV. The plugin ABI is the
    // load-bearing boundary for misuse; rejecting null with a clean
    // error code is safer than crashing.
    if (!backend) { return ENTROPIC_ERROR_INVALID_ARGUMENT; }
    logger->info("C API: inference_load");
    try {
        auto config = parse_config_json(config_json);
        auto rc = to_backend(backend)->load(config)
            ? ENTROPIC_OK : ENTROPIC_ERROR_LOAD_FAILED;
        logger->info("C API: inference_load -> {}",
                     static_cast<int>(rc));
        return rc;
    } catch (const std::exception& e) {
        logger->error("inference_load exception: {}", e.what());
        return ENTROPIC_ERROR_LOAD_FAILED;
    }
}

/**
 * @brief Plugin C API: promote backend from WARM to ACTIVE (GPU load).
 * @param backend Opaque backend handle.
 * @return ENTROPIC_OK on success, ENTROPIC_ERROR_LOAD_FAILED otherwise.
 * @req REQ-INFER-017
 * @version 2.0.0
 */
ENTROPIC_EXPORT entropic_error_t entropic_inference_activate(
    entropic_inference_backend_t backend)
{
    if (!backend) { return ENTROPIC_ERROR_INVALID_ARGUMENT; }
    try {
        return to_backend(backend)->activate() ? ENTROPIC_OK : ENTROPIC_ERROR_LOAD_FAILED;
    } catch (const std::exception& e) {
        logger->error("inference_activate exception: {}", e.what());
        return ENTROPIC_ERROR_LOAD_FAILED;
    }
}

/**
 * @brief Plugin C API: demote backend from ACTIVE to WARM (release GPU).
 * @param backend Opaque backend handle.
 * @return ENTROPIC_OK on success, ENTROPIC_ERROR_INTERNAL on exception.
 * @req REQ-INFER-017
 * @version 2.0.0
 */
ENTROPIC_EXPORT entropic_error_t entropic_inference_deactivate(
    entropic_inference_backend_t backend)
{
    if (!backend) { return ENTROPIC_ERROR_INVALID_ARGUMENT; }
    try {
        to_backend(backend)->deactivate();
        return ENTROPIC_OK;
    } catch (const std::exception& e) {
        logger->error("inference_deactivate exception: {}", e.what());
        return ENTROPIC_ERROR_INTERNAL;
    }
}

/**
 * @brief Plugin C API: release the loaded model (transition to COLD).
 * @param backend Opaque backend handle.
 * @return ENTROPIC_OK on success, ENTROPIC_ERROR_INTERNAL on exception.
 * @req REQ-INFER-017
 * @version 2.0.0
 */
ENTROPIC_EXPORT entropic_error_t entropic_inference_unload(
    entropic_inference_backend_t backend)
{
    if (!backend) { return ENTROPIC_ERROR_INVALID_ARGUMENT; }
    try {
        to_backend(backend)->unload();
        return ENTROPIC_OK;
    } catch (const std::exception& e) {
        logger->error("inference_unload exception: {}", e.what());
        return ENTROPIC_ERROR_INTERNAL;
    }
}

/**
 * @brief Plugin C API: query current lifecycle state (lock-free).
 * @param backend Opaque backend handle.
 * @return Integer cast of ModelState (0=COLD, 1=WARM, 2=ACTIVE).
 * @req REQ-INFER-018
 * @version 2.0.0
 */
ENTROPIC_EXPORT int entropic_inference_state(
    entropic_inference_backend_t backend)
{
    // v2.3.10: null-handle returns COLD (0) — the safest "I don't
    // own anything" answer. Callers gating activation on state should
    // treat null + COLD identically anyway.
    if (!backend) { return static_cast<int>(ENTROPIC_MODEL_STATE_COLD); }
    return static_cast<int>(to_backend(backend)->state());
}

/**
 * @brief Plugin C API: blocking generation returning full result.
 * @param backend Opaque backend handle.
 * @param messages_json JSON-serialized message list.
 * @param params_json JSON-serialized GenerationParams.
 * @param result_json Out-param: newly allocated result JSON (free with entropic_inference_free).
 * @return ENTROPIC_OK on success, result.error_code or ENTROPIC_ERROR_GENERATE_FAILED otherwise.
 * @req REQ-INFER-003
 * @version 2.1.8
 */
ENTROPIC_EXPORT entropic_error_t entropic_inference_generate(
    entropic_inference_backend_t backend,
    const char* messages_json,
    const char* params_json,
    char** result_json)
{
    if (!backend || !result_json) {
        return ENTROPIC_ERROR_INVALID_ARGUMENT;
    }
    logger->info("C API: inference_generate");
    try {
        auto msgs = entropic::parse_messages_json(messages_json);
        auto params = parse_params_json(params_json);
        auto result = to_backend(backend)->generate(msgs, params);
        *result_json = alloc_string(serialize_result_json(result));
        logger->info("C API: inference_generate -> {}",
                     result.ok() ? "ok" : "error");
        return result.ok() ? ENTROPIC_OK : result.error_code;
    } catch (const std::exception& e) {
        logger->error("inference_generate exception: {}", e.what());
        return ENTROPIC_ERROR_GENERATE_FAILED;
    }
}

/**
 * @brief Plugin C API: streaming generation with token callback and cancel flag.
 * @param backend Opaque backend handle.
 * @param messages_json JSON-serialized message list.
 * @param params_json JSON-serialized GenerationParams.
 * @param on_token Callback fired per token (token bytes, length, user_data).
 * @param user_data Opaque pointer passed to on_token.
 * @param cancel_flag Optional pointer; setting *cancel_flag to non-zero stops generation.
 * @return ENTROPIC_OK on success, result.error_code or ENTROPIC_ERROR_GENERATE_FAILED otherwise.
 * @req REQ-INFER-003
 * @version 2.1.8
 */
ENTROPIC_EXPORT entropic_error_t entropic_inference_generate_streaming(
    entropic_inference_backend_t backend,
    const char* messages_json,
    const char* params_json,
    void (*on_token)(const char* token, size_t len, void* user_data),
    void* user_data,
    int* cancel_flag)
{
    if (!backend || !on_token) {
        return ENTROPIC_ERROR_INVALID_ARGUMENT;
    }
    try {
        auto msgs = entropic::parse_messages_json(messages_json);
        auto params = parse_params_json(params_json);
        std::atomic<bool> cancel{false};

        auto callback = [on_token, user_data, cancel_flag, &cancel]
            (std::string_view token) {
                on_token(token.data(), token.size(), user_data);
                if (cancel_flag && *cancel_flag) {
                    cancel.store(true, std::memory_order_release);
                }
            };

        auto result = to_backend(backend)->generate_streaming(
            msgs, params, callback, cancel);
        return result.ok() ? ENTROPIC_OK : result.error_code;
    } catch (const std::exception& e) {
        logger->error("inference_generate_streaming exception: {}", e.what());
        return ENTROPIC_ERROR_GENERATE_FAILED;
    }
}

/**
 * @brief Plugin C API: raw text completion without chat template.
 * @param backend Opaque backend handle.
 * @param prompt Null-terminated prompt string.
 * @param params_json JSON-serialized GenerationParams.
 * @param result_json Out-param: newly allocated result JSON.
 * @return ENTROPIC_OK on success, result.error_code or ENTROPIC_ERROR_GENERATE_FAILED otherwise.
 * @req REQ-INFER-004
 * @version 2.0.0
 */
ENTROPIC_EXPORT entropic_error_t entropic_inference_complete(
    entropic_inference_backend_t backend,
    const char* prompt,
    const char* params_json,
    char** result_json)
{
    if (!backend || !prompt || !result_json) {
        return ENTROPIC_ERROR_INVALID_ARGUMENT;
    }
    try {
        auto params = parse_params_json(params_json);
        auto result = to_backend(backend)->complete(prompt, params);
        *result_json = alloc_string(serialize_result_json(result));
        return result.ok() ? ENTROPIC_OK : result.error_code;
    } catch (const std::exception& e) {
        logger->error("inference_complete exception: {}", e.what());
        return ENTROPIC_ERROR_GENERATE_FAILED;
    }
}

/**
 * @brief Plugin C API: count tokens for a text span (exact when loaded, estimate when COLD).
 * @param backend Opaque backend handle.
 * @param text Pointer to UTF-8 text bytes.
 * @param text_len Length of the text in bytes.
 * @return Exact token count when backend is WARM/ACTIVE, text_len/4 estimate on error.
 * @req REQ-INFER-019
 * @version 2.0.0
 */
ENTROPIC_EXPORT int entropic_inference_count_tokens(
    entropic_inference_backend_t backend,
    const char* text,
    size_t text_len)
{
    // v2.3.10: null guards. text=nullptr is a real misuse (caller
    // owes us bytes). Returning 0 keeps the contract simple (no
    // tokens to count when there's no string).
    if (!backend || !text) { return 0; }
    try {
        return to_backend(backend)->count_tokens(std::string(text, text_len));
    } catch (...) {
        return static_cast<int>(text_len) / 4;
    }
}

/**
 * @brief Plugin C API: destroy the backend and free its resources.
 * @param backend Opaque backend handle (must not be used after this call).
 * @req REQ-INFER-017
 * @version 2.0.0
 */
ENTROPIC_EXPORT void entropic_inference_destroy(
    entropic_inference_backend_t backend)
{
    delete to_backend(backend);
}

/**
 * @brief Plugin C API: free memory allocated by the inference backend.
 * @param ptr Pointer returned by a previous generate/complete call.
 * @utility
 * @version 2.0.0
 */
ENTROPIC_EXPORT void entropic_inference_free(void* ptr) {
    std::free(ptr);
}

/**
 * @brief Factory: create inference backend instance.
 * @return Opaque handle to LlamaCppBackend.
 * @utility
 * @version 1.8.2
 */
ENTROPIC_EXPORT entropic_inference_backend_t entropic_create_inference_backend() {
    return reinterpret_cast<entropic_inference_backend_t>(
        new entropic::LlamaCppBackend());
}

/**
 * @brief Plugin API version.
 * @return Version number.
 * @utility
 * @version 1.8.2
 */
ENTROPIC_EXPORT int entropic_plugin_api_version() {
    return 1;
}

// ── Log redirect (v2.0.1) ──────────────────────────────────

static FILE* s_ggml_log_fp = nullptr;
// llama.cpp's llama_log_set is a single-slot process global. Track
// the active path so a second handle in the same process gets a
// predictable answer: same path → no-op (don't truncate the first
// handle's live log), conflicting path → reject with a warning
// (rather than clobber).
static std::mutex s_ggml_log_mu;
static std::optional<std::string> s_ggml_log_path;

/**
 * @brief Callback that writes to the ggml log file.
 * @callback
 * @version 2.0.1
 */
static void ggml_log_to_file(enum ggml_log_level /*level*/,
                             const char* text, void* /*ud*/) {
    if (s_ggml_log_fp && text) {
        fputs(text, s_ggml_log_fp);
        fflush(s_ggml_log_fp);
    }
}

/**
 * @brief No-op callback.
 * @callback
 * @version 2.0.1
 */
static void ggml_log_noop(enum ggml_log_level /*level*/,
                          const char* /*text*/, void* /*ud*/) {
}

/**
 * @brief Redirect llama/ggml logs to a file or silence them.
 * @internal
 * @version 2.0.1
 */
/**
 * @brief Close the active ggml log fp and route llama logs to noop.
 * Caller must hold s_ggml_log_mu.
 * @internal
 * @version 2.2.5
 */
static void ggml_log_silence_locked() {
    if (s_ggml_log_fp) {
        fclose(s_ggml_log_fp);
        s_ggml_log_fp = nullptr;
    }
    s_ggml_log_path.reset();
    llama_log_set(ggml_log_noop, nullptr);
}

/**
 * @brief Resolve path via weakly_canonical, fall back to raw on error.
 * @internal
 * @version 2.2.5
 */
static std::string canonicalize_or_passthrough(const char* path) {
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(path, ec).string();
    return ec ? std::string(path) : canonical;
}

/**
 * @brief Redirect llama/ggml logs to a file or silence them.
 *
 * First-call-wins under multi-handle (gh#58): a second handle whose
 * canonical path differs is rejected with a warning rather than
 * clobbering the live redirect. Same-path re-call truncates and
 * reopens (preserves pre-v2.2.5 reset-on-recall behavior).
 *
 * @internal
 * @version 2.2.5
 */
void entropic_inference_log_to_file(const char* path) {
    std::lock_guard lk(s_ggml_log_mu);

    if (!path || path[0] == '\0') {
        ggml_log_silence_locked();
        return;
    }
    auto canonical = canonicalize_or_passthrough(path);

    // llama_log_set has one process-global slot; first-call wins so a
    // second handle's redirect cannot clobber the first.
    if (s_ggml_log_path && *s_ggml_log_path != canonical) {
        logger->warn(
            "ggml log redirect already wired to {}; ignoring request for {}",
            *s_ggml_log_path, canonical);
        return;
    }

    FILE* fp = fopen(path, "w");
    if (!fp) {
        logger->warn("ggml log fopen failed for {}: {}",
                     path, std::strerror(errno));
        return;
    }
    if (s_ggml_log_fp) { fclose(s_ggml_log_fp); }
    s_ggml_log_fp = fp;
    s_ggml_log_path = canonical;
    llama_log_set(ggml_log_to_file, nullptr);
}

/**
 * @brief Silence all llama/ggml output.
 * @internal
 * @version 2.0.1
 */
void entropic_inference_log_silence(void) {
    std::lock_guard lk(s_ggml_log_mu);
    ggml_log_silence_locked();
}

} // extern "C"
