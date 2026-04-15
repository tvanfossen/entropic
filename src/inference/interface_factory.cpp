// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file interface_factory.cpp
 * @brief InferenceInterface factory — bridges orchestrator to C callbacks.
 * @version 2.0.1
 */

#include <entropic/inference/interface_factory.h>
#include <entropic/inference/orchestrator.h>
#include <entropic/types/config.h>
#include <entropic/types/message.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace entropic {

// ── Context struct for callbacks ───────────────────────────

/**
 * @brief Holds orchestrator + tier for C callback user_data.
 * @internal
 * @version 2.0.1
 */
struct InterfaceContext {
    ModelOrchestrator* orchestrator; ///< Orchestrator pointer
    std::string default_tier;        ///< Default tier name
};

// Leaked intentionally — lives for the process lifetime.
// One per configure call. Acceptable tradeoff for C callback safety.
static InterfaceContext* s_ctx = nullptr;

// ── JSON helpers ───────────────────────────────────────────

/**
 * @brief Parse JSON message array into Message vector.
 * @param json_str JSON array of {role, content} objects.
 * @return Parsed messages.
 * @utility
 * @version 2.0.1
 */
static std::vector<Message> parse_msgs(const char* json_str) {
    std::vector<Message> msgs;
    if (!json_str) { return msgs; }
    auto arr = nlohmann::json::parse(json_str, nullptr, false);
    if (!arr.is_array()) { return msgs; }
    for (const auto& obj : arr) {
        Message m;
        m.role = obj.value("role", "");
        m.content = obj.value("content", "");
        msgs.push_back(std::move(m));
    }
    return msgs;
}

/**
 * @brief Parse generation params from JSON string.
 * @param json_str JSON params (may be null).
 * @return GenerationParams with parsed overrides.
 * @utility
 * @version 2.0.1
 */
static GenerationParams parse_params(const char* json_str) {
    GenerationParams p;
    if (!json_str) { return p; }
    auto j = nlohmann::json::parse(json_str, nullptr, false);
    if (!j.is_object()) { return p; }
    if (j.contains("max_tokens")) { p.max_tokens = j["max_tokens"]; }
    if (j.contains("temperature")) { p.temperature = j["temperature"]; }
    return p;
}

/**
 * @brief Extract tier name from params JSON, falling back to default.
 * @param json_str Params JSON (may contain "tier" field).
 * @param default_tier Fallback tier name.
 * @return Tier name.
 * @utility
 * @version 2.0.1
 */
static std::string extract_tier(const char* json_str,
                                const std::string& default_tier) {
    if (!json_str) { return default_tier; }
    auto j = nlohmann::json::parse(json_str, nullptr, false);
    if (j.is_object() && j.contains("tier")) {
        return j["tier"].get<std::string>();
    }
    return default_tier;
}

/**
 * @brief Heap-allocate a C string copy.
 * @param s Source string.
 * @return strdup'd copy (caller frees).
 * @utility
 * @version 2.0.1
 */
static char* dup(const std::string& s) {
    return strdup(s.c_str());
}

// ── C-callable wrappers ────────────────────────────────────

/**
 * @brief Generate via orchestrator.
 * @callback
 * @version 2.0.1
 */
static int iface_generate(const char* msgs_json,
                          const char* params_json,
                          char** result_json,
                          void* user_data) {
    auto* ctx = static_cast<InterfaceContext*>(user_data);
    auto messages = parse_msgs(msgs_json);
    auto params = parse_params(params_json);
    auto tier = extract_tier(params_json, ctx->default_tier);
    auto result = ctx->orchestrator->generate(
        messages, params, tier);
    auto& out = result.raw_content.empty()
        ? result.content : result.raw_content;
    *result_json = dup(out);
    return 0;
}

/**
 * @brief Streaming generate via orchestrator.
 * @callback
 * @version 2.0.1
 */
static int iface_generate_stream(
    const char* msgs_json, const char* params_json,
    void (*on_token)(const char*, size_t, void*),
    void* token_ud, int* cancel, void* user_data) {
    auto* ctx = static_cast<InterfaceContext*>(user_data);
    auto messages = parse_msgs(msgs_json);
    auto params = parse_params(params_json);
    std::atomic<bool> cancel_flag(cancel && *cancel);
    auto cb = [on_token, token_ud](std::string_view tok) {
        on_token(tok.data(), tok.size(), token_ud);
    };
    auto tier = extract_tier(params_json, ctx->default_tier);
    ctx->orchestrator->generate_streaming(
        messages, params, cb, cancel_flag, tier);
    return 0;
}

/**
 * @brief Route messages to tier via orchestrator.
 * @callback
 * @version 2.0.1
 */
static int iface_route(const char* msgs_json,
                       char** result_json, void* user_data) {
    auto* ctx = static_cast<InterfaceContext*>(user_data);
    auto messages = parse_msgs(msgs_json);
    auto tier = ctx->orchestrator->route(messages);
    *result_json = dup(tier);
    return 0;
}

/**
 * @brief Raw text completion via orchestrator.
 * @callback
 * @version 2.0.1
 */
static int iface_complete(const char* prompt,
                          const char* params_json,
                          char** result_json, void* user_data) {
    auto* ctx = static_cast<InterfaceContext*>(user_data);
    auto tier = extract_tier(params_json, ctx->default_tier);
    Message msg;
    msg.role = "user";
    msg.content = prompt;
    GenerationParams params{};
    params.max_tokens = 1;
    auto result = ctx->orchestrator->generate(
        {msg}, params, tier);
    *result_json = dup(result.content);
    return 0;
}

/**
 * @brief Parse tool calls from raw model output via adapter.
 * @callback
 * @version 2.0.4
 */
static int iface_parse_tool_calls(const char* raw,
                                  char** cleaned,
                                  char** tool_calls_json,
                                  void* user_data) {
    auto* ctx = static_cast<InterfaceContext*>(user_data);
    auto* adapter = ctx->orchestrator->get_adapter(ctx->default_tier);
    if (!adapter) {
        *cleaned = dup(raw ? raw : "");
        *tool_calls_json = dup("[]");
        return 0;
    }
    auto parsed = adapter->parse_tool_calls(raw ? raw : "");
    *cleaned = dup(parsed.cleaned_content);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& tc : parsed.tool_calls) {
        nlohmann::json args;
        for (const auto& [k, v] : tc.arguments) {
            auto parsed_val = nlohmann::json::parse(v, nullptr, false);
            args[k] = parsed_val.is_discarded()
                ? nlohmann::json(v) : parsed_val;
        }
        arr.push_back({{"name", tc.name}, {"arguments", args}});
    }
    *tool_calls_json = dup(arr.dump());
    return 0;
}

/**
 * @brief Check if response is complete (no pending tool calls).
 * @callback
 * @version 2.0.1
 */
static int iface_is_complete(const char* /*content*/,
                             const char* tool_calls_json,
                             void* /*user_data*/) {
    if (!tool_calls_json) { return 1; }
    auto tc = nlohmann::json::parse(tool_calls_json, nullptr, false);
    return (tc.is_array() && !tc.empty()) ? 0 : 1;
}

// ── Factory ────────────────────────────────────────────────

/**
 * @brief Build InferenceInterface wired to an orchestrator.
 * @param orchestrator Orchestrator to wire.
 * @param default_tier Default tier name.
 * @return Wired interface.
 * @internal
 * @version 2.0.1
 */
InferenceInterface build_orchestrator_interface(
    ModelOrchestrator* orchestrator,
    const std::string& default_tier) {
    // Replace previous context (one active at a time)
    delete s_ctx;
    s_ctx = new InterfaceContext{orchestrator, default_tier};

    InferenceInterface iface;
    iface.generate = iface_generate;
    iface.generate_stream = iface_generate_stream;
    iface.route = iface_route;
    iface.complete = iface_complete;
    iface.parse_tool_calls = iface_parse_tool_calls;
    iface.is_response_complete = iface_is_complete;
    iface.free_fn = free;
    iface.backend_data = s_ctx;
    iface.orchestrator_data = s_ctx;
    iface.adapter_data = s_ctx;
    return iface;
}

} // namespace entropic
