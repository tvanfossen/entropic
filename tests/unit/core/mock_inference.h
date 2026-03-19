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
 * @version 1.8.4
 */
struct MockInference {
    std::string response = "Hello, world!";    ///< Scripted response
    std::string tier = "default";              ///< Routed tier name
    std::string finish_reason = "stop";        ///< Finish reason
    bool stream_token_by_token = false;        ///< Stream char-by-char
    int generate_call_count = 0;               ///< Call counter
    int route_call_count = 0;                  ///< Route call counter
    bool is_complete = true;                   ///< is_response_complete result
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
 * @internal
 * @version 1.8.4
 */
inline int mock_generate(
    const char* /*messages_json*/,
    const char* /*params_json*/,
    char** result_json,
    void* user_data) {
    auto* mock = static_cast<MockInference*>(user_data);
    mock->generate_call_count++;
    *result_json = mock_strdup(mock->response);
    return 0;
}

/**
 * @brief Mock streaming generate — fires tokens then completes.
 * @internal
 * @version 1.8.4
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

    if (mock->stream_token_by_token) {
        for (size_t i = 0; i < mock->response.size(); ++i) {
            if (cancel != nullptr && *cancel != 0) {
                return 0;
            }
            on_token(&mock->response[i], 1, token_ud);
        }
    } else {
        on_token(mock->response.c_str(), mock->response.size(),
                 token_ud);
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
 * @brief Mock is_response_complete.
 * @internal
 * @version 1.8.4
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
 * @return Wired interface.
 * @version 1.8.4
 */
inline entropic::InferenceInterface make_mock_interface(
    MockInference& mock) {
    entropic::InferenceInterface iface;
    iface.generate = mock_generate;
    iface.generate_stream = mock_generate_stream;
    iface.route = mock_route;
    iface.is_response_complete = mock_is_complete;
    iface.free_fn = mock_free;
    iface.backend_data = &mock;
    iface.orchestrator_data = &mock;
    iface.adapter_data = &mock;
    return iface;
}

} // namespace entropic::test
