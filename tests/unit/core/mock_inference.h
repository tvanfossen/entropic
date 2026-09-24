// SPDX-License-Identifier: Apache-2.0
/**
 * @file mock_inference.h
 * @brief Mock inference interface for core engine testing.
 *
 * Provides scripted responses for generate/stream/route/complete
 * without requiring a real model. Grows with each version.
 *
 * @version 1.8.4
 */

#pragma once

#include <entropic/interfaces/i_inference_callbacks.h>

#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace entropic::test {

/**
 * @brief Mock inference state for scripted test responses.
 * @version 1.10.1
 */
struct MockInference {
    std::string response = "Hello, world!";    ///< Scripted response (fallback)
    std::string tier = "default";              ///< Routed tier name
    std::string finish_reason = "stop";        ///< Finish reason
    bool stream_token_by_token = false;        ///< Stream char-by-char
    int generate_call_count = 0;               ///< Call counter
    int route_call_count = 0;                  ///< Route call counter
    bool is_complete = true;                   ///< is_response_complete result

    // v1.10.1: Multi-turn and regression test support
    std::vector<std::string> response_queue;      ///< Pop front per generate, fall back to response
    std::string complete_response;                ///< Scripted complete() output
    int complete_call_count = 0;                  ///< Complete call counter
    std::string tool_calls_json = "[]";           ///< Default parse result (no tools)
    std::vector<std::string> tool_calls_queue;    ///< Pop front per parse, fall back to tool_calls_json

    // gh#111: when non-empty, mock_parse_tool_calls returns this as the
    // *cleaned output instead of passing raw_content through. Simulates the
    // real backend's common_chat/adapter parse_response, which derives cleaned
    // content from the RAW generation (not the engine-sanitized result.content)
    // — so a split multi-byte UTF-8 codepoint from MTP survives into *cleaned.
    std::string parse_cleaned_override;

    /// @brief v2.13.0: return code the generate callbacks report.
    ///
    /// The inference ABI reports failure as the callback's int return, and
    /// nothing in the harness could produce a non-zero one — so the engine's
    /// handling of a typed backend refusal (ENTROPIC_ERROR_EVAL_CONTEXT_FULL,
    /// "this prompt cannot fit the tier context") had no CPU coverage at all.
    int generate_rc = 0;

    /// @brief gh#158 (v2.13.0): guards the mutable fields above.
    ///
    /// One AgentEngine now serves several concurrent runs, so the
    /// concurrency suite drives TWO turns through ONE MockInference — and
    /// `generate_call_count++` plus `response_queue.erase()` from two
    /// threads is a data race in the HARNESS. ThreadSanitizer reported it
    /// inside `mock_generate_stream`, which would otherwise have been read
    /// as a finding about the engine. Every single-threaded test is
    /// unaffected: the lock is uncontended and the fields keep their types,
    /// so `mock.generate_call_count == 3` still compiles everywhere.
    mutable std::mutex mutex;
};

/**
 * @brief Allocate a C string copy (freed by mock_free).
 * @param s Source string.
 * @return Heap-allocated copy.
 * @internal
 * @version 1.8.4
 */
inline char* mock_strdup(const std::string& s) {
    auto* p = new char[s.size() + 1];
    std::memcpy(p, s.c_str(), s.size() + 1);
    return p;
}

/**
 * @brief Mock free function.
 * @param ptr Pointer to free.
 * @internal
 * @version 1.8.4
 */
inline void mock_free(void* ptr) {
    delete[] static_cast<char*>(ptr);
}

/**
 * @brief Mock batch generate — returns scripted response.
 * @param messages_json Messages (unused).
 * @param params_json Params (unused).
 * @param result_json Output response string.
 * @param user_data MockInference pointer.
 * @return 0.
 * @internal
 * @version 1.10.1
 */
inline int mock_generate(
    const char* /*messages_json*/,
    const char* /*params_json*/,
    char** result_json,
    void* user_data) {
    auto* mock = static_cast<MockInference*>(user_data);
    std::lock_guard<std::mutex> lock(mock->mutex);
    mock->generate_call_count++;
    if (!mock->response_queue.empty()) {
        *result_json = mock_strdup(mock->response_queue.front());
        mock->response_queue.erase(mock->response_queue.begin());
    } else {
        *result_json = mock_strdup(mock->response);
    }
    return mock->generate_rc;
}

/**
 * @brief Mock streaming generate — fires tokens then completes.
 * @param messages_json Messages (unused).
 * @param params_json Params (unused).
 * @param on_token Token callback.
 * @param token_ud Token callback user data.
 * @param cancel Cancel flag pointer.
 * @param user_data MockInference pointer.
 * @return 0.
 * @internal
 * @version 1.10.1
 */
inline int mock_generate_stream(
    const char* /*messages_json*/,
    const char* /*params_json*/,
    void (*on_token)(const char*, size_t, void*),
    void* token_ud,
    int* cancel,
    void* user_data) {
    auto* mock = static_cast<MockInference*>(user_data);
    // gh#158: take the scripted response and pop it under the lock, then
    // stream the COPY outside it. Holding the lock across on_token would
    // serialize the two runs at the generate call, which is exactly the
    // overlap the concurrency scenario exists to produce.
    std::string resp;
    bool token_by_token = false;
    int rc = 0;
    {
        std::lock_guard<std::mutex> lock(mock->mutex);
        mock->generate_call_count++;
        rc = mock->generate_rc;
        resp = mock->response_queue.empty()
            ? mock->response
            : mock->response_queue.front();
        if (!mock->response_queue.empty()) {
            mock->response_queue.erase(mock->response_queue.begin());
        }
        token_by_token = mock->stream_token_by_token;
    }

    // v2.13.0: a refused turn emits nothing. Streaming tokens first and
    // THEN reporting the failure would give the engine partial content to
    // work with, which is not what a pre-decode refusal looks like.
    if (rc != 0) { return rc; }

    if (token_by_token) {
        for (size_t i = 0; i < resp.size(); ++i) {
            if (cancel != nullptr && *cancel != 0) {
                return 0;
            }
            on_token(&resp[i], 1, token_ud);
        }
    } else {
        on_token(resp.c_str(), resp.size(), token_ud);
    }

    return 0;
}

/**
 * @brief Mock route — returns scripted tier name.
 * @internal
 * @version 1.8.4
 */
inline int mock_route(
    const char* /*messages_json*/,
    char** result_json,
    void* user_data) {
    auto* mock = static_cast<MockInference*>(user_data);
    std::lock_guard<std::mutex> lock(mock->mutex);
    mock->route_call_count++;
    *result_json = mock_strdup(mock->tier);
    return 0;
}

/**
 * @brief Mock raw text completion (router classification path).
 * @param prompt Raw prompt (unused).
 * @param params_json Params (unused).
 * @param result_json Output response string.
 * @param user_data MockInference pointer.
 * @return 0.
 * @internal
 * @version 1.10.1
 */
inline int mock_complete(
    const char* /*prompt*/,
    const char* /*params_json*/,
    char** result_json,
    void* user_data) {
    auto* mock = static_cast<MockInference*>(user_data);
    std::lock_guard<std::mutex> lock(mock->mutex);
    mock->complete_call_count++;
    *result_json = mock_strdup(mock->complete_response);
    return 0;
}

/**
 * @brief Mock tool call parsing — returns scripted tool calls.
 * @param raw_content Raw model output.
 * @param cleaned_content Output: cleaned content (passthrough).
 * @param tool_calls_json Output: tool calls JSON.
 * @param user_data MockInference pointer.
 * @return 0.
 * @internal
 * @version 1.10.1
 */
inline int mock_parse_tool_calls(
    const char* raw_content,
    char** cleaned_content,
    char** tool_calls_json,
    void* user_data) {
    auto* mock = static_cast<MockInference*>(user_data);
    std::lock_guard<std::mutex> lock(mock->mutex);
    // gh#111: emulate a backend parse that returns raw-derived cleaned content
    // (bypassing the engine's content sanitize) when an override is set.
    *cleaned_content = mock_strdup(
        mock->parse_cleaned_override.empty()
            ? std::string(raw_content ? raw_content : "")
            : mock->parse_cleaned_override);
    if (!mock->tool_calls_queue.empty()) {
        *tool_calls_json = mock_strdup(
            mock->tool_calls_queue.front());
        mock->tool_calls_queue.erase(
            mock->tool_calls_queue.begin());
    } else {
        *tool_calls_json = mock_strdup(mock->tool_calls_json);
    }
    return 0;
}

/**
 * @brief Mock is_response_complete.
 * @param content Content (unused).
 * @param tool_calls_json Tool calls (unused).
 * @param user_data MockInference pointer.
 * @return 1 if complete, 0 otherwise.
 * @internal
 * @version 1.10.1
 */
inline int mock_is_complete(
    const char* /*content*/,
    const char* /*tool_calls_json*/,
    void* user_data) {
    auto* mock = static_cast<MockInference*>(user_data);
    return mock->is_complete ? 1 : 0;
}

/**
 * @brief Build an InferenceInterface wired to a MockInference.
 * @param mock Mock state (caller must keep alive).
 * @return Wired interface with all callbacks.
 * @version 1.10.1
 */
inline entropic::InferenceInterface make_mock_interface(
    MockInference& mock) {
    entropic::InferenceInterface iface;
    iface.generate = mock_generate;
    iface.generate_stream = mock_generate_stream;
    iface.route = mock_route;
    iface.complete = mock_complete;
    iface.parse_tool_calls = mock_parse_tool_calls;
    iface.is_response_complete = mock_is_complete;
    iface.free_fn = mock_free;
    iface.backend_data = &mock;
    iface.orchestrator_data = &mock;
    iface.adapter_data = &mock;
    return iface;
}

} // namespace entropic::test
