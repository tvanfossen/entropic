// SPDX-License-Identifier: LGPL-3.0-or-later
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

#include "llama_cpp_backend.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdlib>
#include <cstring>
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
entropic::ModelConfig parse_config_json(const char* json_str) {
    entropic::ModelConfig config;
    auto j = nlohmann::json::parse(json_str);

    if (j.contains("path"))           config.path = j["path"].get<std::string>();
    if (j.contains("adapter"))        config.adapter = j["adapter"].get<std::string>();
    if (j.contains("context_length")) config.context_length = j["context_length"].get<int>();
    if (j.contains("gpu_layers"))     config.gpu_layers = j["gpu_layers"].get<int>();
    if (j.contains("keep_warm"))      config.keep_warm = j["keep_warm"].get<bool>();
    if (j.contains("use_mlock"))      config.use_mlock = j["use_mlock"].get<bool>();
    if (j.contains("n_batch"))        config.n_batch = j["n_batch"].get<int>();
    if (j.contains("n_threads"))      config.n_threads = j["n_threads"].get<int>();
    if (j.contains("flash_attn"))     config.flash_attn = j["flash_attn"].get<bool>();

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

/**
 * @brief Parse a single content part from a JSON object.
 * @param part JSON object with "type" and content fields.
 * @return Parsed ContentPart.
 * @internal
 * @version 1.9.11
 */
entropic::ContentPart parse_content_part(const nlohmann::json& part) {
    entropic::ContentPart cp;
    auto type_str = part.value("type", "text");
    if (type_str == "image") {
        cp.type = entropic::ContentPartType::IMAGE;
        cp.image_path = part.value("path", "");
        cp.image_url = part.value("url", "");
    } else {
        cp.type = entropic::ContentPartType::TEXT;
        cp.text = part.value("text", "");
    }
    return cp;
}

/**
 * @brief Parse messages from JSON array string.
 *
 * Handles both string content and array content (multimodal).
 * When content is an array, populates content_parts and sets
 * content = extract_text(content_parts).
 *
 * @param json_str JSON array of message objects.
 * @return Vector of Message structs.
 * @internal
 * @version 1.9.11
 */
std::vector<entropic::Message> parse_messages_json(const char* json_str) {
    std::vector<entropic::Message> messages;
    auto arr = nlohmann::json::parse(json_str);
    for (const auto& m : arr) {
        entropic::Message msg;
        msg.role = m.value("role", "user");
        if (m.contains("content") && m["content"].is_array()) {
            for (const auto& part : m["content"]) {
                msg.content_parts.push_back(parse_content_part(part));
            }
            msg.content = entropic::extract_text(msg.content_parts);
        } else {
            msg.content = m.value("content", "");
        }
        messages.push_back(std::move(msg));
    }
    return messages;
}

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
 * @version 2.0.0
 */
ENTROPIC_EXPORT entropic_error_t entropic_inference_generate(
    entropic_inference_backend_t backend,
    const char* messages_json,
    const char* params_json,
    char** result_json)
{
    logger->info("C API: inference_generate");
    try {
        auto msgs = parse_messages_json(messages_json);
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
 * @version 2.0.0
 */
ENTROPIC_EXPORT entropic_error_t entropic_inference_generate_streaming(
    entropic_inference_backend_t backend,
    const char* messages_json,
    const char* params_json,
    void (*on_token)(const char* token, size_t len, void* user_data),
    void* user_data,
    int* cancel_flag)
{
    try {
        auto msgs = parse_messages_json(messages_json);
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
void entropic_inference_log_to_file(const char* path) {
    if (s_ggml_log_fp) {
        fclose(s_ggml_log_fp);
        s_ggml_log_fp = nullptr;
    }
    if (path) {
        s_ggml_log_fp = fopen(path, "w");
        if (s_ggml_log_fp) {
            llama_log_set(ggml_log_to_file, nullptr);
            return;
        }
    }
    llama_log_set(ggml_log_noop, nullptr);
}

/**
 * @brief Silence all llama/ggml output.
 * @internal
 * @version 2.0.1
 */
void entropic_inference_log_silence(void) {
    if (s_ggml_log_fp) {
        fclose(s_ggml_log_fp);
        s_ggml_log_fp = nullptr;
    }
    llama_log_set(ggml_log_noop, nullptr);
}

} // extern "C"
