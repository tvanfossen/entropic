// SPDX-License-Identifier: Apache-2.0
/**
 * @file interface_factory.cpp
 * @brief InferenceInterface factory — bridges orchestrator to C callbacks.
 * @version 2.0.1
 */

#include <entropic/inference/interface_factory.h>
#include <entropic/inference/orchestrator.h>
#include <entropic/types/config.h>
#include <entropic/types/message.h>
#include <entropic/inference/adapters/adapter_base.h>  // gh#88 recovery

#include "llama_cpp_backend.h"  // gh#87 3b: common_chat parse routing
#include "tool_call_serialize.h"  // gh#93: shared (typed) tool-call serialization

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace entropic {

// ── Context struct for callbacks ───────────────────────────

/**
 * @brief Holds orchestrator + tier for C callback user_data.
 *
 * Owned by the engine handle (since v2.2.6, gh#58 follow-up).
 * Pre-v2.2.6 a single process-global `s_ctx` was reassigned on
 * every configure, which broke as soon as two handles existed.
 *
 * @internal
 * @version 2.2.6
 */
struct InterfaceContext {
    ModelOrchestrator* orchestrator; ///< Orchestrator pointer
    std::string default_tier;        ///< Default tier name
};

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
 * @brief Conditionally assign a typed JSON field into a destination.
 *
 * Extracted (v2.3.14, gh#23 MVP item 2) to keep `parse_params` under
 * the knots ABC gate as new MVP-10 knobs land.
 * @utility
 * @version 2.3.14
 */
template <typename T>
static void assign_if_present(const nlohmann::json& j,
                              const char* key, T& dst) {
    if (j.contains(key)) { dst = j[key].get<T>(); }
}

/**
 * @brief Populate `logit_bias` map from a JSON object of token→bias.
 *
 * Accepts `{"123": -100.0}` shape — string keys parsed to `int32_t`.
 * Skips un-parseable keys silently.
 * @utility
 * @version 2.3.16
 */
static void parse_logit_bias_into(
    const nlohmann::json& j,
    std::unordered_map<int32_t, float>& dst)
{
    if (!j.contains("logit_bias") || !j["logit_bias"].is_object()) {
        return;
    }
    for (auto it = j["logit_bias"].begin(); it != j["logit_bias"].end(); ++it) {
        try {
            dst[std::stoi(it.key())] = it.value().get<float>();
        } catch (const std::exception&) {
            // skip un-parseable keys
        }
    }
}

/**
 * @brief Parse generation params from JSON string.
 * @param json_str JSON params (may be null).
 * @return GenerationParams with parsed overrides.
 * @utility
 * @version 2.7.0
 */
static GenerationParams parse_params(const char* json_str) {
    GenerationParams p;
    if (!json_str) { return p; }
    auto j = nlohmann::json::parse(json_str, nullptr, false);
    if (!j.is_object()) { return p; }
    assign_if_present(j, "max_tokens",       p.max_tokens);
    assign_if_present(j, "temperature",      p.temperature);
    assign_if_present(j, "grammar_key",      p.grammar_key);
    assign_if_present(j, "enable_thinking",  p.enable_thinking);
    assign_if_present(j, "top_p",            p.top_p);
    assign_if_present(j, "top_k",            p.top_k);
    assign_if_present(j, "min_p",            p.min_p);
    assign_if_present(j, "presence_penalty", p.presence_penalty);
    assign_if_present(j, "frequency_penalty",p.frequency_penalty);
    assign_if_present(j, "repeat_penalty",   p.repeat_penalty);
    assign_if_present(j, "seed",             p.seed);
    assign_if_present(j, "tools",            p.tools);   // gh#87 3b
    parse_logit_bias_into(j, p.logit_bias);
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
 *
 * gh#81 (v2.4.2): bridges `int* cancel` (engine-side ABI) to
 * `std::atomic<bool> cancel_flag` (backend contract) per-token
 * inside the on_token lambda. Pre-fix the local atomic was
 * initialized once from `*cancel` at entry and never re-read,
 * so an interrupt raised mid-stream by the engine's per-token
 * callback (`stream_token_callback` raising `*cancel_flag = 1`)
 * never reached the backend's decode loop. The backend polls
 * the atomic but the atomic stayed false.
 *
 * @callback
 * @version 2.4.2
 */
static int iface_generate_stream(
    const char* msgs_json, const char* params_json,
    void (*on_token)(const char*, size_t, void*),
    void* token_ud, int* cancel, void* user_data) {
    auto* ctx = static_cast<InterfaceContext*>(user_data);
    auto messages = parse_msgs(msgs_json);
    auto params = parse_params(params_json);
    std::atomic<bool> cancel_flag(false);
    auto cb = [on_token, token_ud, cancel, &cancel_flag]
              (std::string_view tok) {
        on_token(tok.data(), tok.size(), token_ud);
        if (cancel != nullptr && *cancel != 0) {
            cancel_flag.store(true, std::memory_order_release);
        }
    };
    auto tier = extract_tier(params_json, ctx->default_tier);
    ctx->orchestrator->generate_streaming(
        messages, params, cb, cancel_flag, tier);
    return 0;
}

/**
 * @brief Batch generate with cancel via orchestrator (gh#81, v2.4.2).
 *
 * Batch shape with mid-decode cancellation. The pre-v2.4.2
 * `iface_generate` accepted no cancel pointer and ran the backend
 * to natural stop; this entry adds the cancel bridge using a
 * small poller thread (no per-token hook in the batch contract).
 *
 * @callback
 * @version 2.4.2
 */
static int iface_generate_with_cancel(
    const char* msgs_json, const char* params_json,
    char** result_json, int* cancel, void* user_data) {
    auto* ctx = static_cast<InterfaceContext*>(user_data);
    auto messages = parse_msgs(msgs_json);
    auto params = parse_params(params_json);
    auto tier = extract_tier(params_json, ctx->default_tier);

    std::atomic<bool> cancel_flag(false);
    std::atomic<bool> done(false);
    std::thread poller;
    if (cancel != nullptr) {
        poller = std::thread([cancel, &cancel_flag, &done]() {
            while (!done.load(std::memory_order_acquire)) {
                if (*cancel != 0) {
                    cancel_flag.store(true, std::memory_order_release);
                    return;
                }
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(10));
            }
        });
    }

    auto result = ctx->orchestrator->generate(
        messages, params, cancel_flag, tier);

    done.store(true, std::memory_order_release);
    if (poller.joinable()) { poller.join(); }

    auto& out = result.raw_content.empty()
        ? result.content : result.raw_content;
    *result_json = dup(out);
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
 * @brief Build a ToolCall from a parsed JSON object with name/tool + arguments/parameters.
 * @return Single-element vector on success; empty vector on schema mismatch.
 * @utility
 * @version 2.9.18
 */
static std::vector<ToolCall> tc_from_json_obj(const nlohmann::json& j) {
    // j.value() returns default when key absent or wrong type — fewer branches.
    std::string name = j.value("name", std::string{});
    if (name.empty()) { name = j.value("tool", std::string{}); }

    auto it = j.find("arguments");
    if (it == j.end() || !it->is_object()) { it = j.find("parameters"); }
    const nlohmann::json* args =
        (it != j.end() && it->is_object()) ? &(*it) : nullptr;

    std::vector<ToolCall> result;
    if (!name.empty() && args != nullptr) {
        ToolCall tc;
        tc.name = std::move(name);
        for (const auto& [k, v] : args->items()) {
            tc.arguments[k] = v.is_string() ? v.get<std::string>() : v.dump();
        }
        result.push_back(std::move(tc));
    }
    return result;
}

/**
 * @brief Extract a tool call from a single fenced json block in raw model output (gh#122).
 *
 * Fires only when both the PEG parser and adapter return 0 calls and the
 * output contains exactly one fenced ```json``` block that matches the
 * {name/tool, arguments/parameters} schema. Conservative: does nothing if
 * the block is missing, malformed, or there are multiple blocks.
 *
 * @param raw Raw model output.
 * @return Single-element vector on success; empty on mismatch.
 * @utility
 * @version 2.9.18
 */
static std::vector<ToolCall> try_fenced_json_call(const std::string& raw) {
    const std::string kOpen = "```json";
    const std::string kClose = "```";
    auto ob = raw.find(kOpen);
    auto bs = (ob != std::string::npos) ? (ob + kOpen.size()) : std::string::npos;
    auto cb = (bs != std::string::npos) ? raw.find(kClose, bs) : std::string::npos;
    bool single = (ob != std::string::npos) && (cb != std::string::npos)
        && (raw.find(kOpen, ob + 1) == std::string::npos
            || raw.find(kOpen, ob + 1) > cb);
    if (!single) { return {}; }
    auto j = nlohmann::json::parse(raw.substr(bs, cb - bs), nullptr, false);
    if (!j.is_object()) { return {}; }
    return tc_from_json_obj(j);
}

/**
 * @brief Parse tool calls via the adapter for the given tier (gh#89).
 *
 * gh#89: uses the SAME routed tier (last_used_tier) so a non-default
 * autoparser tier parses with the correct adapter, not the default.
 * Returns the raw content and empty calls when no adapter is registered.
 *
 * @param orch Model orchestrator (adapter lookup).
 * @param raw Raw model output.
 * @param tier Active tier name.
 * @param out_cleaned Cleaned content (raw if no adapter).
 * @param out_calls Parsed tool calls (empty if no adapter or 0 found).
 * @utility
 * @version 2.9.18
 */
static void parse_via_adapter(ModelOrchestrator* orch,
                              const std::string& raw,
                              const std::string& tier,
                              std::string& out_cleaned,
                              std::vector<ToolCall>& out_calls) {
    auto* adapter = orch->get_adapter(tier);
    if (adapter == nullptr) { out_cleaned = raw; return; }
    auto p = adapter->parse_tool_calls(raw);
    out_cleaned = std::move(p.cleaned_content);
    out_calls = std::move(p.tool_calls);
}

/**
 * @brief Parse tool calls from raw model output (gh#87 3b).
 *
 * gh#87 Phase D: parse via common_chat (`parse_response`) only when its
 * format is multi-parameter safe (`common_chat_parse_reliable` — the
 * dedicated PEG_GEMMA4 grammar). Autoparser families (Qwen, nemotron3) fall
 * back to their hand-rolled multi-parameter adapter. Routed on the last-used
 * tier's backend so a routed (non-default) tier parses correctly.
 *
 * gh#122 (v2.9.18): when both paths yield 0 calls and the output contains
 * exactly one fenced ```json``` block, try extracting a {name, arguments}
 * call from it. Handles models that emit tool calls in markdown fence format.
 *
 * @callback
 * @version 2.9.18
 */
static int iface_parse_tool_calls(const char* raw,
                                  char** cleaned,
                                  char** tool_calls_json,
                                  void* user_data) {
    auto* ctx = static_cast<InterfaceContext*>(user_data);
    std::string raw_str = raw ? raw : "";
    auto tier = ctx->orchestrator->last_used_tier();
    if (tier.empty()) { tier = ctx->default_tier; }
    auto* llama = dynamic_cast<LlamaCppBackend*>(
        ctx->orchestrator->get_backend(tier));

    std::string cleaned_str;
    std::vector<ToolCall> calls;
    if (llama != nullptr && llama->common_chat_parse_reliable()) {
        auto parsed = llama->parse_response(raw_str);
        apply_action_envelope_recovery(parsed.tool_calls, raw_str);  // gh#88
        cleaned_str = std::move(parsed.content);
        calls = std::move(parsed.tool_calls);
    } else {
        parse_via_adapter(ctx->orchestrator, raw_str, tier, cleaned_str, calls);
    }

    if (calls.empty()) {
        auto fb = try_fenced_json_call(raw_str);  // gh#122
        if (!fb.empty()) { calls = std::move(fb); }
    }

    *cleaned = dup(cleaned_str);
    *tool_calls_json = dup(serialize_tool_calls(calls));
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
 * @version 2.4.2
 */
InferenceInterface build_orchestrator_interface(
    ModelOrchestrator* orchestrator,
    const std::string& default_tier,
    InterfaceContext** out_context) {
    auto* ctx = new InterfaceContext{orchestrator, default_tier};
    if (out_context) { *out_context = ctx; }

    InferenceInterface iface;
    iface.generate = iface_generate;
    iface.generate_cancellable = iface_generate_with_cancel;  // gh#81, v2.4.2
    iface.generate_stream = iface_generate_stream;
    iface.route = iface_route;
    iface.complete = iface_complete;
    iface.parse_tool_calls = iface_parse_tool_calls;
    iface.is_response_complete = iface_is_complete;
    iface.free_fn = free;
    iface.backend_data = ctx;
    iface.orchestrator_data = ctx;
    iface.adapter_data = ctx;
    return iface;
}

/**
 * @brief Free a context returned by build_orchestrator_interface().
 * @internal
 * @version 2.2.6
 */
void destroy_orchestrator_interface(InterfaceContext* context) {
    delete context;
}

} // namespace entropic
