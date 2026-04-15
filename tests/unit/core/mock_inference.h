// SPDX-License-Identifier: LGPL-3.0-or-later
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
    mock->generate_call_count++;
    if (!mock->response_queue.empty()) {
        *result_json = mock_strdup(mock->response_queue.front());
        mock->response_queue.erase(mock->response_queue.begin());
    } else {
        *result_json = mock_strdup(mock->response);
    }
    return 0;
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
    mock->generate_call_count++;

    const auto& resp = mock->response_queue.empty()
        ? mock->response
        : mock->response_queue.front();

    if (mock->stream_token_by_token) {
        for (size_t i = 0; i < resp.size(); ++i) {
            if (cancel != nullptr && *cancel != 0) {
                return 0;
            }
            on_token(&resp[i], 1, token_ud);
        }
    } else {
        on_token(resp.c_str(), resp.size(), token_ud);
    }

    if (!mock->response_queue.empty()) {
        mock->response_queue.erase(mock->response_queue.begin());
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
    *cleaned_content = mock_strdup(raw_content);
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
