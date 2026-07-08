// SPDX-License-Identifier: Apache-2.0
/**
 * @file response_generator.cpp
 * @brief Response generation implementation.
 * @version 1.8.4
 */

#include <entropic/core/response_generator.h>
#include <entropic/mcp/utf8_sanitize.h>
#include <entropic/types/error.h>
#include <entropic/types/logging.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <thread>
#include <unordered_map>

static auto logger = entropic::log::get("core.response_generator");

namespace entropic {

/// Per-tier system prompt hash for diff detection across delegations.
static std::unordered_map<std::string, size_t> s_tier_system_hash;

/**
 * @brief Log the full assembled prompt (all messages, no truncation).
 *
 * Tracks system prompt hash per-tier so that consecutive delegations
 * to the same tier correctly detect content drift.
 *
 * @param messages Message list being sent to inference.
 * @param tier Locked tier name.
 * @utility
 * @version 2.0.6
 */
static void log_prompt(const std::vector<Message>& messages,
                       const std::string& tier) {
    logger->info("─── Prompt ({} messages, tier={}) ───",
                 messages.size(), tier);
    for (size_t i = 0; i < messages.size(); ++i) {
        if (messages[i].role == "system") {
            size_t h = std::hash<std::string>{}(messages[i].content);
            size_t prev = s_tier_system_hash[tier];
            s_tier_system_hash[tier] = h;
            if (h != prev || prev == 0) {
                logger->info("[{}] role=system hash={:016x} "
                             "prev={:016x}\n{}",
                             i, h, prev, messages[i].content);
            } else {
                logger->info("[{}] role=system [unchanged, {} chars, "
                             "hash={:016x}]",
                             i, messages[i].content.size(), h);
            }
        } else {
            logger->info("[{}] role={}\n{}", i, messages[i].role,
                         messages[i].content);
        }
    }
    logger->info("─── End prompt ───");
}

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
    /// @brief Pointer to the backend's cancel flag (gh#20, v2.1.5).
    ///
    /// When the interrupt event fires, the token callback writes 1
    /// here so the next iteration of the backend's decode loop stops
    /// within a single token instead of running to natural EOS.
    int* cancel_flag = nullptr;
    /// @brief Global observer — fires on every token alongside
    ///        callbacks->on_stream_chunk. (2.0.6-rc16)
    void (*observer)(const char*, size_t, void*) = nullptr;
    void* observer_data = nullptr;    ///< Observer user_data
};

/**
 * @brief Token callback for streaming generation.
 * @param token Token string.
 * @param len Token length.
 * @param user_data StreamAccumulator pointer.
 * @internal
 * @version 2.1.12
 */
static void stream_token_callback(
    const char* token,
    size_t len,
    void* user_data) {
    auto* acc = static_cast<StreamAccumulator*>(user_data);

    // gh#20 (v2.1.5): two coupled bugs lived here.
    //
    // (A) The previous implementation set `acc->interrupted = true`
    //     and returned early WITHOUT propagating the interrupt to the
    //     backend. The backend's cancel_flag stayed 0, so llama_cpp
    //     ran to natural EOS — up to 60s of wasted decode after the
    //     user pressed Ctrl-C.
    //
    // (B) The early return also dropped every post-interrupt token
    //     from `acc->content`. When the backend finally finished
    //     cleanly, the response_generator built the iter result from
    //     this truncated buffer (e.g. 7 chars instead of the 107
    //     decoded tokens forming a valid tool call), throwing away
    //     fully-formed output.
    //
    // The fix raises the cancel flag for the backend AND keeps
    // appending the token so the content buffer is complete up to
    // the cancel point. The backend stops on its next loop iteration
    // (<= 1 token wall-time); whatever made it through is preserved.
    bool just_interrupted = acc->events->interrupt != nullptr
        && acc->events->interrupt->load()
        && !acc->interrupted;
    if (just_interrupted) {
        acc->interrupted = true;
        // gh#49 (v2.1.12): log the cancel-flag raise so a session
        // log can confirm the per-token interrupt poll observed the
        // engine-level flag. Pre-v2.1.12 the bissell-llm-studio
        // repro saw the "Engine interrupted" line on 0->1 transition
        // but no evidence the per-token poll ever fired — this log
        // is the first observable receipt of the propagation.
        logger->info("Stream interrupt observed at token {}; "
                     "raising backend cancel_flag",
                     acc->token_index);
        if (acc->cancel_flag != nullptr) {
            *acc->cancel_flag = 1;
        }
    }

    acc->content.append(token, len);
    if (acc->callbacks->on_stream_chunk != nullptr) {
        acc->callbacks->on_stream_chunk(token, len,
                                         acc->callbacks->user_data);
    }

    // Global observer — fires on every token regardless of whether
    // the caller registered on_stream_chunk. (2.0.6-rc16)
    if (acc->observer != nullptr) {
        acc->observer(token, len, acc->observer_data);
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
 * @brief Resolve a stream's finish_reason from rc + content size.
 *
 * gh#20 (v2.1.5) resolution order: CANCELLED → "interrupted";
 * error with partial content → "partial"; error with none → "error";
 * clean → "stop".
 *
 * @param rc Backend return code.
 * @param content_size Accumulated content length.
 * @return finish_reason string.
 * @utility
 * @version 2.3.7
 */
static std::string resolve_stream_finish_reason(int rc,
                                                size_t content_size) {
    std::string reason;
    if (rc == ENTROPIC_ERROR_CANCELLED) {
        logger->info("Stream cancelled by interrupt after {} chars",
                     content_size);
        reason = "interrupted";
    } else if (rc != 0 && content_size > 0) {
        logger->warn("Stream failed (rc={}) after {} chars — "
                     "preserving partial", rc, content_size);
        reason = "partial";
    } else if (rc != 0) {
        logger->error("Stream failed (rc={}) with no partial content", rc);
        reason = "error";
    } else {
        reason = "stop";
    }
    return reason;
}

/**
 * @brief Inject prompts + serialize messages/params for a turn.
 * @param ctx Loop context.
 * @param mode Label for the log line ("stream"/"batch").
 * @return {messages_json, params_json}.
 * @internal
 * @version 2.7.0
 */
std::pair<std::string, std::string> ResponseGenerator::prepare_prompts(
    LoopContext& ctx, const char* mode) {
    // gh#87 (v2.7.0): tool defs no longer string-injected into the system
    // message — they flow as structured JSON via build_params_json →
    // params.tools, and common_chat renders them in the model's native
    // format. Only the runtime engine-state reminder is injected here.
    auto messages = inject_engine_state_reminder(ctx.messages, ctx);
    logger->info("Generate ({}): tier={}, {} messages",
                 mode, ctx.locked_tier, messages.size());
    log_prompt(messages, ctx.locked_tier);
    return {serialize_messages(messages),
            build_params_json(ctx.locked_tier)};
}

/**
 * @brief Generate via streaming.
 * @param ctx Loop context.
 * @return Generation result.
 * @internal
 * @version 2.3.7
 */
GenerateResult ResponseGenerator::generate_streaming(LoopContext& ctx) {
    if (inference_.generate_stream == nullptr) {
        logger->warn("No streaming function, falling back to batch");
        return generate_batch(ctx);
    }

    auto [msgs_json, params_json] = prepare_prompts(ctx, "stream");

    int cancel_flag = 0;
    StreamAccumulator acc;
    acc.callbacks = &callbacks_;
    acc.events = &events_;
    acc.hooks = &hooks_;
    // gh#20 (v2.1.5): give the token callback a path to raise the
    // backend cancel flag when an interrupt is observed. Without
    // this, the previous implementation would early-return out of
    // the token callback without ever telling the backend to stop.
    acc.cancel_flag = &cancel_flag;
    // Wire the persistent stream observer so every token — including
    // batch entropic_run and delegate child-loop generations — reaches
    // any registered observer. (P0-1, 2.0.6-rc16)
    acc.observer = stream_observer_;
    acc.observer_data = stream_observer_data_;

    int rc = inference_.generate_stream(
        msgs_json.c_str(), params_json.c_str(),
        stream_token_callback, &acc,
        &cancel_flag, inference_.backend_data);

    GenerateResult result;
    result.finish_reason = resolve_stream_finish_reason(rc,
                                                        acc.content.size());
    // Issue #3 (v2.1.1): inbound boundary from llama_cpp. Models can emit
    // malformed UTF-8 mid-stream (partial multi-byte runs under XML-tool-call
    // pressure, decoder desyncs). Sanitize ONCE at message-finalization,
    // never per-token — a multi-byte codepoint may split across token
    // boundaries and per-token sanitize would corrupt valid output.
    // See include/entropic/mcp/utf8_sanitize.h for the boundary policy.
    result.content = mcp::sanitize_utf8(acc.content);
    result.tool_calls_json = "[]";
    logger->info("Generate complete (stream): finish={}, {} chars",
                 result.finish_reason, result.content.size());
    return result;
}

/**
 * @brief Generate via batch (non-streaming).
 * @param ctx Loop context.
 * @return Generation result.
 * @internal
 * @version 2.3.7
 */
/**
 * @brief Dispatch the batch backend call. See header. (gh#81, v2.4.2)
 * @internal
 * @version 2.9.6
 */
int ResponseGenerator::dispatch_batch_generate(
    const std::string& msgs_json,
    const std::string& params_json,
    char** result_json) {
    // Fall back to the no-cancel entry for backends that predate the
    // generate_cancellable ABI field.
    //
    // gh#110 (v2.9.6): also prefer the no-cancel entry whenever
    // speculative decoding is enabled. `generate_cancellable`'s
    // backing orchestrator call deliberately bypasses
    // run_generate_dispatch (speculative/MTP routing) — batch-with-
    // cancel only ever calls plain decode. The plain `generate` entry
    // point runs run_generate_dispatch and is therefore the only
    // batch path that can reach MTP. v1 tradeoff: a speculative batch
    // turn is not cancellable mid-decode (documented, not silent).
    if (inference_.generate_cancellable == nullptr
        || loop_config_.speculative_enabled) {
        return inference_.generate(
            msgs_json.c_str(), params_json.c_str(),
            result_json, inference_.backend_data);
    }

    // The C-ABI side bridges this int → atomic<bool> for the backend
    // via its own poller; this side mirrors the engine's atomic
    // interrupt_flag_ → the int. Two cheap 10ms-poll hops, but it
    // keeps the C ABI int*-only (no atomic across the .so boundary).
    int cancel_int =
        (events_.interrupt != nullptr
         && events_.interrupt->load(std::memory_order_acquire)) ? 1 : 0;

    std::atomic<bool> observer_done(false);
    std::thread observer;
    if (events_.interrupt != nullptr) {
        auto* flag = events_.interrupt;
        observer = std::thread([&cancel_int, flag, &observer_done]() {
            while (!observer_done.load(std::memory_order_acquire)) {
                if (flag->load(std::memory_order_acquire)) {
                    cancel_int = 1;
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
    }

    int rc = inference_.generate_cancellable(
        msgs_json.c_str(), params_json.c_str(),
        result_json, &cancel_int, inference_.backend_data);

    observer_done.store(true, std::memory_order_release);
    if (observer.joinable()) { observer.join(); }
    return rc;
}

/**
 * @brief Generate via batch (non-streaming). (gh#81 cancel-aware, v2.4.2)
 * @param ctx Loop context.
 * @return Generation result.
 * @internal
 * @version 2.4.2
 */
GenerateResult ResponseGenerator::generate_batch(LoopContext& ctx) {
    if (inference_.generate == nullptr
        && inference_.generate_cancellable == nullptr) {
        logger->error("No generate function available");
        return {"", "[]", "error"};
    }

    auto [msgs_json, params_json] = prepare_prompts(ctx, "batch");
    char* result_json = nullptr;

    // gh#81 (v2.4.2): prefer the cancellable dispatch so an interrupt
    // is honored mid-decode (was ~60s lag pre-fix).
    int rc = dispatch_batch_generate(msgs_json, params_json, &result_json);

    GenerateResult result;
    // gh#81 (v2.4.2): a cancelled batch is terminal, not an error —
    // map it to "interrupted" so the engine transitions to INTERRUPTED
    // and any partial content is preserved (mirrors the streaming
    // resolve_stream_finish_reason policy).
    if (rc == ENTROPIC_ERROR_CANCELLED) {
        result.finish_reason = "interrupted";
        if (result_json != nullptr) {
            result.content = mcp::sanitize_utf8(result_json);
        }
        result.tool_calls_json = "[]";
        logger->info("Generate cancelled (batch) after {} chars",
                     result.content.size());
    } else if (rc == 0 && result_json != nullptr) {
        // Issue #3 (v2.1.1): inbound boundary, batch path. See the
        // streaming branch above for rationale; same policy applies.
        result.content = mcp::sanitize_utf8(result_json);
        result.finish_reason = "stop";
        result.tool_calls_json = "[]";
        // Fire observer once with full content so the non-streaming
        // fallback still reaches registered observers. (2.0.6-rc16)
        if (stream_observer_ != nullptr && !result.content.empty()) {
            stream_observer_(result.content.data(),
                             result.content.size(),
                             stream_observer_data_);
        }
    } else {
        result.finish_reason = "error";
        logger->error("Generate failed (rc={})", rc);
    }
    if (result_json != nullptr && inference_.free_fn != nullptr) {
        inference_.free_fn(result_json);
    }
    logger->info("Generate complete (batch): finish={}, {} chars",
                 result.finish_reason, result.content.size());
    return result;
}

/**
 * @brief Handle pause during streaming generation.
 *
 * v2.1.10 (gh#40 fallout): fire the persistent state_observer_ slot
 * in addition to the legacy callbacks_.on_state_change, so consumers
 * see PAUSED during streaming runs where the legacy callbacks have
 * been overwritten by run_streaming's set_callbacks() shuffle.
 *
 * @param ctx Loop context.
 * @param partial Content generated so far.
 * @return Updated content.
 * @internal
 * @version 2.1.10
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
    // gh#40 fallout (v2.1.10): persistent slot fires alongside the
    // legacy on_state_change so consumers see PAUSED during
    // streaming runs where the legacy callbacks_ struct has been
    // overwritten by run_streaming's set_callbacks() shuffle.
    if (state_observer_ != nullptr) {
        state_observer_(static_cast<int>(AgentState::PAUSED),
                        state_observer_data_);
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
/**
 * @brief JSON-escape one string into a growing buffer.
 * @internal
 * @version 2.1.8
 */
static void json_escape_into(const std::string& s, std::string& out) {
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:   out += c;      break;
        }
    }
}

/**
 * @brief Serialize a single multimodal content_parts array (gh#37, v2.1.8).
 *
 * Emits `[{"type":"text","text":"..."}, {"type":"image","path":"..."}]`
 * directly — keeps core free of nlohmann/json (design rule #21).
 *
 * @internal
 * @version 2.1.8
 */
static void serialize_content_parts(
    const std::vector<ContentPart>& parts, std::string& out) {
    out += '[';
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) { out += ','; }
        if (parts[i].type == ContentPartType::IMAGE) {
            out += R"({"type":"image","path":")";
            json_escape_into(parts[i].image_path, out);
            out += R"(","url":")";
            json_escape_into(parts[i].image_url, out);
            out += R"("})";
        } else {
            out += R"({"type":"text","text":")";
            json_escape_into(parts[i].text, out);
            out += R"("})";
        }
    }
    out += ']';
}

/**
 * @brief Serialize a message list to the engine-canonical JSON wire shape.
 *
 * Hand-rolled — core has no nlohmann/json dependency (design rule
 * #21). For messages with non-empty `content_parts`, emits an
 * OpenAI-style content array so image parts survive the
 * engine→backend hop (gh#37, v2.1.8). Otherwise emits a plain
 * `"content":"..."` string.
 *
 * @param messages Message vector to serialize.
 * @return JSON array string ready for entropic_inference_generate.
 * @internal
 * @version 2.1.8
 */
std::string ResponseGenerator::serialize_messages(
    const std::vector<Message>& messages) {
    std::string json = "[";
    for (size_t i = 0; i < messages.size(); ++i) {
        if (i > 0) { json += ','; }
        json += "{\"role\":\"" + messages[i].role + "\",\"content\":";
        if (messages[i].content_parts.empty()) {
            json += '"';
            json_escape_into(messages[i].content, json);
            json += '"';
        } else {
            serialize_content_parts(messages[i].content_parts, json);
        }
        json += '}';
    }
    json += ']';
    return json;
}

/**
 * @brief Build generation params JSON, including per-tier tool defs.
 *
 * gh#87 (v2.7.0): embeds the tier's structured tool definitions (from the
 * repurposed get_tool_prompt callback — now an MCP JSON array) under
 * `params.tools`. The backend stages them for common_chat rendering, which
 * emits the model's native tool-call format. Replaces the old system-message
 * string injection.
 *
 * @param tier Locked tier name.
 * @return JSON string {tier, tools?}.
 * @internal
 * @version 2.7.0
 */
std::string ResponseGenerator::build_params_json(
    const std::string& tier) {
    nlohmann::json j = nlohmann::json::object();
    if (!tier.empty()) { j["tier"] = tier; }

    if (inference_.get_tool_prompt != nullptr) {
        char* tools = nullptr;
        int rc = inference_.get_tool_prompt(
            tier.c_str(), &tools, inference_.tool_prompt_data);
        if (rc == 0 && tools != nullptr) {
            j["tools"] = std::string(tools);
            if (inference_.free_fn) { inference_.free_fn(tools); }
        }
    }
    return j.dump();
}

/**
 * @brief Append "[engine] iteration N/MAX, tool calls so far K." as a
 *        NEW user message at the end of the prompt (per-turn reminder).
 *
 * Resolves effective max_iterations from ctx.effective_max_iterations
 * (per-identity override) or falls back to loop_config_.max_iterations.
 *
 * Issue #16 (v2.1.4): the reminder is injected as a fresh user message
 * rather than appended to the first system message's content. This keeps
 * the system message stable across turns so PromptCache hits actually
 * fire — pre-2.1.4 the per-turn reminder mutated the system message,
 * driving 100% miss rate (the cache key AND prefix_tokens both varied
 * each turn). The reminder lands at the END of the message list — the
 * most-recent context the model sees before generating — and is not
 * persisted into ctx.messages. (#16)
 *
 * @internal
 * @version 2.1.4
 */
std::vector<Message> ResponseGenerator::inject_engine_state_reminder(
    const std::vector<Message>& messages,
    const LoopContext& ctx) {
    int max_iter = ctx.effective_max_iterations >= 0
        ? ctx.effective_max_iterations
        : loop_config_.max_iterations;
    std::string reminder = "[engine] iteration "
        + std::to_string(ctx.metrics.iterations)
        + "/" + std::to_string(max_iter)
        + ", tool calls so far: "
        + std::to_string(ctx.metrics.tool_calls) + ".";

    // Demo ask #2 (v2.1.0): if the previous turn was validator-rejected,
    // surface the reason so the model knows WHY it's being asked again.
    // Engine clears pending_validation_feedback after this turn — the
    // line is one-shot.
    if (!ctx.pending_validation_feedback.empty()) {
        reminder += "\n[engine] previous turn rejected: "
                  + ctx.pending_validation_feedback;
    }
    // Demo ask #5 (v2.1.0): anti-spiral primitive. ToolExecutor
    // populated this when consecutive_same_tool_calls hit
    // max_consecutive_same_tool. Same one-shot lifecycle as the
    // validation feedback above; engine clears after this turn.
    if (!ctx.pending_anti_spiral_warning.empty()) {
        reminder += "\n[engine] anti-spiral: "
                  + ctx.pending_anti_spiral_warning;
    }

    auto result = messages;
    Message reminder_msg;
    reminder_msg.role = "user";
    reminder_msg.content = std::move(reminder);
    result.push_back(std::move(reminder_msg));
    return result;
}

} // namespace entropic
