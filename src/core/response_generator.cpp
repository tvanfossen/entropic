/**
 * @file response_generator.cpp
 * @brief Response generation implementation.
 * @version 1.8.4
 */

#include <entropic/core/response_generator.h>
#include <entropic/types/logging.h>

#include <cstring>

static auto logger = entropic::log::get("core.response_generator");

namespace entropic {

/**
 * @brief Construct a response generator.
 * @param inference Inference interface.
 * @param loop_config Loop configuration.
 * @param callbacks Shared callbacks.
 * @param events Interrupt/pause flags.
 * @internal
 * @version 1.8.4
 */
ResponseGenerator::ResponseGenerator(
    const InferenceInterface& inference,
    const LoopConfig& loop_config,
    EngineCallbacks& callbacks,
    GenerationEvents events)
    : inference_(inference),
      loop_config_(loop_config),
      callbacks_(callbacks),
      events_(events) {}

/**
 * @brief Generate model response, routing first if needed.
 * @param ctx Loop context.
 * @return Generation result.
 * @internal
 * @version 1.8.4
 */
GenerateResult ResponseGenerator::generate_response(LoopContext& ctx) {
    lock_tier_if_needed(ctx);

    if (loop_config_.stream_output) {
        return generate_streaming(ctx);
    }
    return generate_batch(ctx);
}

/**
 * @brief Check if a response indicates completion.
 * @param content Response content.
 * @param tool_calls_json Tool calls JSON.
 * @return true if complete.
 * @internal
 * @version 1.8.4
 */
bool ResponseGenerator::is_response_complete(
    const std::string& content,
    const std::string& tool_calls_json) {
    if (inference_.is_response_complete == nullptr) {
        return !content.empty();
    }
    return inference_.is_response_complete(
        content.c_str(), tool_calls_json.c_str(),
        inference_.adapter_data) != 0;
}

/**
 * @brief Route and lock tier before first generation.
 * @param ctx Loop context.
 * @internal
 * @version 1.8.4
 */
void ResponseGenerator::lock_tier_if_needed(LoopContext& ctx) {
    if (!ctx.locked_tier.empty()) {
        if (callbacks_.on_tier_selected != nullptr) {
            callbacks_.on_tier_selected(ctx.locked_tier.c_str(),
                                        callbacks_.user_data);
        }
        return;
    }

    if (inference_.route == nullptr) {
        ctx.locked_tier = "default";
        return;
    }

    auto msgs_json = serialize_messages(ctx.messages);
    char* result_json = nullptr;
    int rc = inference_.route(msgs_json.c_str(), &result_json,
                              inference_.orchestrator_data);
    if (rc == 0 && result_json != nullptr) {
        ctx.locked_tier = result_json;
        if (inference_.free_fn != nullptr) {
            inference_.free_fn(result_json);
        }
    } else {
        ctx.locked_tier = "default";
        logger->warn("Routing failed (rc={}), using default tier", rc);
    }

    logger->info("Locked tier: {}", ctx.locked_tier);
    if (callbacks_.on_tier_selected != nullptr) {
        callbacks_.on_tier_selected(ctx.locked_tier.c_str(),
                                    callbacks_.user_data);
    }
}

// ── Streaming token accumulator context ──────────────────

/**
 * @brief Context passed to the streaming token callback.
 * @internal
 * @version 1.8.4
 */
struct StreamAccumulator {
    std::string content;              ///< Accumulated content
    EngineCallbacks* callbacks;       ///< Callback reference
    GenerationEvents* events;         ///< Event flags
    const HookInterface* hooks;       ///< Hook dispatch (v1.9.1)
    int token_index = 0;              ///< Token counter (v1.9.1)
    bool interrupted = false;         ///< Set when interrupt detected
};

/**
 * @brief Token callback for streaming generation.
 * @param token Token string.
 * @param len Token length.
 * @param user_data StreamAccumulator pointer.
 * @internal
 * @version 1.9.1
 */
static void stream_token_callback(
    const char* token,
    size_t len,
    void* user_data) {
    auto* acc = static_cast<StreamAccumulator*>(user_data);
    if (acc->events->interrupt != nullptr
        && acc->events->interrupt->load()) {
        acc->interrupted = true;
        return;
    }
    acc->content.append(token, len);
    if (acc->callbacks->on_stream_chunk != nullptr) {
        acc->callbacks->on_stream_chunk(token, len,
                                         acc->callbacks->user_data);
    }

    // Hook: ON_STREAM_TOKEN (v1.9.1)
    if (acc->hooks != nullptr && acc->hooks->fire_info != nullptr) {
        std::string json = "{\"token_index\":"
            + std::to_string(acc->token_index++) + "}";
        acc->hooks->fire_info(acc->hooks->registry,
            ENTROPIC_HOOK_ON_STREAM_TOKEN, json.c_str());
    }
}

/**
 * @brief Generate via streaming.
 * @param ctx Loop context.
 * @return Generation result.
 * @internal
 * @version 1.9.1
 */
GenerateResult ResponseGenerator::generate_streaming(LoopContext& ctx) {
    if (inference_.generate_stream == nullptr) {
        logger->warn("No streaming function, falling back to batch");
        return generate_batch(ctx);
    }

    auto msgs_json = serialize_messages(ctx.messages);
    auto params_json = build_params_json();

    int cancel_flag = 0;
    StreamAccumulator acc;
    acc.callbacks = &callbacks_;
    acc.events = &events_;
    acc.hooks = &hooks_;

    inference_.generate_stream(
        msgs_json.c_str(), params_json.c_str(),
        stream_token_callback, &acc,
        &cancel_flag, inference_.backend_data);

    GenerateResult result;
    result.finish_reason = acc.interrupted ? "interrupted" : "stop";
    result.content = acc.content;
    result.tool_calls_json = "[]";
    return result;
}

/**
 * @brief Generate via batch (non-streaming).
 * @param ctx Loop context.
 * @return Generation result.
 * @internal
 * @version 1.8.4
 */
GenerateResult ResponseGenerator::generate_batch(LoopContext& ctx) {
    if (inference_.generate == nullptr) {
        logger->error("No generate function available");
        return {"", "[]", "error"};
    }

    auto msgs_json = serialize_messages(ctx.messages);
    auto params_json = build_params_json();
    char* result_json = nullptr;

    int rc = inference_.generate(
        msgs_json.c_str(), params_json.c_str(),
        &result_json, inference_.backend_data);

    GenerateResult result;
    if (rc == 0 && result_json != nullptr) {
        result.content = result_json;
        result.finish_reason = "stop";
        result.tool_calls_json = "[]";
        if (inference_.free_fn != nullptr) {
            inference_.free_fn(result_json);
        }
    } else {
        result.finish_reason = "error";
        logger->error("Generate failed (rc={})", rc);
    }
    return result;
}

/**
 * @brief Handle pause during streaming generation.
 * @param ctx Loop context.
 * @param partial Content generated so far.
 * @return Updated content.
 * @internal
 * @version 1.8.4
 */
std::string ResponseGenerator::handle_pause(
    LoopContext& ctx,
    const std::string& partial) {
    ctx.state = AgentState::PAUSED;
    if (callbacks_.on_state_change != nullptr) {
        callbacks_.on_state_change(
            static_cast<int>(AgentState::PAUSED),
            callbacks_.user_data);
    }

    char* injection = nullptr;
    if (callbacks_.on_pause_prompt != nullptr) {
        callbacks_.on_pause_prompt(partial.c_str(), &injection,
                                    callbacks_.user_data);
    }

    if (injection == nullptr) {
        if (events_.interrupt != nullptr) {
            events_.interrupt->store(true);
        }
        return partial;
    }

    std::string inj(injection);
    if (inj.empty()) {
        ctx.state = AgentState::EXECUTING;
        return partial;
    }

    // Injection provided: append partial + injection to messages
    if (!partial.empty()) {
        Message partial_msg;
        partial_msg.role = "assistant";
        partial_msg.content = partial + "\n\n[Generation paused by user]";
        ctx.messages.push_back(std::move(partial_msg));
    }
    Message inject_msg;
    inject_msg.role = "user";
    inject_msg.content = "[User interjection]: " + inj
        + "\n\nPlease continue with this in mind.";
    ctx.messages.push_back(std::move(inject_msg));

    ctx.state = AgentState::EXECUTING;
    return "";
}

/**
 * @brief Serialize messages to JSON for inference interface.
 * @param messages Message list.
 * @return JSON array string (minimal format).
 * @internal
 * @version 1.8.4
 */
std::string ResponseGenerator::serialize_messages(
    const std::vector<Message>& messages) {
    // Minimal JSON serialization — no nlohmann/json dependency.
    std::string json = "[";
    for (size_t i = 0; i < messages.size(); ++i) {
        if (i > 0) { json += ","; }
        json += "{\"role\":\"" + messages[i].role
              + "\",\"content\":\"";
        // Escape content for JSON
        for (char c : messages[i].content) {
            switch (c) {
            case '"':  json += "\\\""; break;
            case '\\': json += "\\\\"; break;
            case '\n': json += "\\n";  break;
            case '\r': json += "\\r";  break;
            case '\t': json += "\\t";  break;
            default:   json += c;      break;
            }
        }
        json += "\"}";
    }
    json += "]";
    return json;
}

/**
 * @brief Build generation params JSON.
 * @return JSON string with default params.
 * @internal
 * @version 1.8.4
 */
std::string ResponseGenerator::build_params_json() {
    return "{}";
}

} // namespace entropic
