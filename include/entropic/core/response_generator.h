// SPDX-License-Identifier: Apache-2.0
/**
 * @file response_generator.h
 * @brief Response generation subsystem for the agentic loop.
 *
 * Handles tier routing via inference interface, system prompt assembly,
 * streaming with interrupt/pause, and tool call parsing.
 *
 * @version 1.8.4
 */

#pragma once

#include <entropic/core/engine_types.h>
#include <entropic/core/stream_think_filter.h>
#include <entropic/interfaces/i_hook_handler.h>
#include <entropic/interfaces/i_inference_callbacks.h>

#include <string>
#include <vector>

namespace entropic {

/**
 * @brief Result of a generate_response call.
 * @version 1.8.4
 */
struct GenerateResult {
    std::string content;        ///< Cleaned response content
    std::string tool_calls_json; ///< Tool calls as JSON (empty if none)
    std::string finish_reason;  ///< "stop", "length", "interrupted"
};

/**
 * @brief Handles model response generation, tier routing, pause/injection.
 *
 * Subsystem of AgentEngine. Uses InferenceInterface function pointers
 * to communicate with the inference layer without compile-time dependency.
 *
 * @version 1.8.4
 */
class ResponseGenerator {
public:
    /**
     * @brief Construct a response generator.
     * @param inference Inference interface (function pointers).
     * @param loop_config Loop configuration.
     * @param callbacks Shared callbacks (reference).
     * @param events Interrupt/pause signal flags.
     * @version 1.8.4
     */
    ResponseGenerator(const InferenceInterface& inference,
                      const LoopConfig& loop_config,
                      EngineCallbacks& callbacks,
                      GenerationEvents events);

    /**
     * @brief Generate model response, routing tier first if needed.
     * @param ctx Loop context (mutated: tier locked, system prompt rebuilt).
     * @return Generation result with content and tool calls.
     * @version 1.8.4
     */
    GenerateResult generate_response(LoopContext& ctx);

    /**
     * @brief Check if the last response indicates completion.
     * @param content Response content.
     * @param tool_calls_json Tool calls JSON (may be "[]").
     * @return true if response is complete.
     * @version 1.8.4
     */
    bool is_response_complete(const std::string& content,
                              const std::string& tool_calls_json);

private:
    /**
     * @brief Route and lock tier before first generation.
     * @param ctx Loop context.
     * @version 1.8.4
     */
    void lock_tier_if_needed(LoopContext& ctx);

    /**
     * @brief Generate via streaming with interrupt/pause.
     * @param ctx Loop context.
     * @return Generation result.
     * @version 1.8.4
     */
    GenerateResult generate_streaming(LoopContext& ctx);

    /**
     * @brief Generate via batch (non-streaming).
     * @param ctx Loop context.
     * @return Generation result.
     * @version 1.8.4
     */
    GenerateResult generate_batch(LoopContext& ctx);

    /**
     * @brief Dispatch the batch backend call, honoring interrupts. (gh#81, v2.4.2)
     *
     * Prefers `inference_.generate_cancellable` when wired: spawns a
     * short-lived observer thread mirroring the engine's
     * `interrupt_flag_` into the C-ABI int cancel flag for the
     * duration of the call. Falls back to plain `inference_.generate`
     * for backends without the cancellable field.
     *
     * @param msgs_json Serialized messages.
     * @param params_json Serialized params.
     * @param[out] result_json Backend-allocated result (caller frees).
     * @return Backend return code (0 ok, ENTROPIC_ERROR_CANCELLED on
     *         interrupt, other error codes otherwise).
     * @internal
     * @version 2.4.2
     */
    int dispatch_batch_generate(
        const std::string& msgs_json,
        const std::string& params_json,
        char** result_json);

    /**
     * @brief Inject prompts + serialize messages/params for a turn.
     *
     * Shared prep extracted from generate_streaming/generate_batch to
     * keep both knots-clean. Injects tool prompt + engine-state
     * reminder, logs, and serializes.
     *
     * @param ctx Loop context.
     * @param mode Label for the log line ("stream"/"batch").
     * @return {messages_json, params_json}.
     * @internal
     * @version 2.3.7
     */
    std::pair<std::string, std::string> prepare_prompts(
        LoopContext& ctx, const char* mode);

    /**
     * @brief Handle pause during streaming generation.
     * @param ctx Loop context.
     * @param partial Content generated so far.
     * @return Updated content after pause handling.
     * @version 1.8.4
     */
    std::string handle_pause(LoopContext& ctx,
                             const std::string& partial);

    /**
     * @brief Serialize messages to JSON for inference interface.
     * @param messages Message list.
     * @return JSON string.
     * @version 1.8.4
     */
    static std::string serialize_messages(
        const std::vector<Message>& messages);

    /**
     * @brief Build generation params JSON with tier routing.
     * @param tier Locked tier name (embedded in params for orchestrator).
     * @return JSON string.
     * @version 2.0.1
     */
    static std::string build_params_json(const std::string& tier);

    /**
     * @brief Inject tool definitions into system message.
     * @param messages Original message list.
     * @param tier Locked tier name for tool filtering.
     * @return Copy of messages with tool prompt appended to system message.
     * @version 2.0.4
     */
    std::vector<Message> inject_tool_prompt(
        const std::vector<Message>& messages,
        const std::string& tier);

    /**
     * @brief Append an "[engine] iteration N/MAX..." reminder to the
     *        first system message.
     *
     * Demo ask #1 (v2.1.0): models in deep delegations have no view of
     * their own iteration count or remaining budget. Identity prompts
     * encode anti-spiral rules ("after 3 calls, pivot") that the model
     * cannot enforce because it doesn't know its own count. This
     * helper surfaces the engine-side counters as a system-prompt
     * augmentation visible to every generation step. No new persistent
     * messages are appended — the line is added to the system content
     * for THIS turn only.
     *
     * @param messages Caller's message vector (returned augmented copy).
     * @param ctx Loop context — pulls metrics.iterations and
     *            metrics.tool_calls; max_iterations is resolved against
     *            the per-tier override (effective_max_iterations) or
     *            falls back to LoopConfig.max_iterations.
     * @return Messages with the reminder line appended to the system
     *         message; original returned unchanged when no system
     *         message exists.
     * @internal
     * @version 2.1.0
     */
    std::vector<Message> inject_engine_state_reminder(
        const std::vector<Message>& messages,
        const LoopContext& ctx);

    InferenceInterface inference_;     ///< Inference function pointers
    LoopConfig loop_config_;           ///< Loop configuration
    EngineCallbacks& callbacks_;       ///< Shared callbacks
    GenerationEvents events_;          ///< Interrupt/pause flags
    HookInterface hooks_;              ///< Hook dispatch (v1.9.1)
    /// @brief Global stream observer fires on every token from every
    ///        generation path — batch entropic_run, entropic_run_streaming,
    ///        and delegate child loops. Persists across EngineCallbacks
    ///        reassignment in run_streaming (stored separately). (2.0.6-rc16)
    TokenCallback stream_observer_ = nullptr;
    void* stream_observer_data_ = nullptr;

    /// @brief Persistent state-transition observer, parallel to
    /// stream_observer_. Survives the EngineCallbacks reassignment
    /// done by run_streaming so the PAUSED transition emitted from
    /// handle_pause still reaches the consumer. (gh#40 fallout, v2.1.10)
    void (*state_observer_)(int, void*) = nullptr;
    void* state_observer_data_ = nullptr;

public:
    /**
     * @brief Set the hook dispatch interface.
     * @param hooks Hook dispatch interface.
     * @utility
     * @version 1.9.1
     */
    void set_hooks(const HookInterface& hooks) { hooks_ = hooks; }

    /**
     * @brief Set the global stream observer.
     *
     * Fires for every token from every generation path: batch
     * entropic_run, entropic_run_streaming, and delegate child loops.
     * Stored separately from EngineCallbacks so it survives the
     * set_callbacks() reassignment done by run_streaming.
     *
     * @param observer Callback invoked with each token (nullable).
     * @param user_data Forwarded to observer unchanged.
     * @utility
     * @version 2.0.6-rc16
     */
    void set_stream_observer(TokenCallback observer, void* user_data) {
        stream_observer_ = observer;
        stream_observer_data_ = user_data;
    }

    /**
     * @brief Get the registered stream observer callback.
     * @return Function pointer (nullptr if unset).
     * @utility
     * @version 2.0.6-rc16
     */
    TokenCallback stream_observer() const { return stream_observer_; }

    /**
     * @brief Set the persistent state-transition observer.
     *
     * Mirrors set_stream_observer's persistent-slot pattern so the
     * PAUSED transition emitted from handle_pause reaches the
     * consumer even after run_streaming overwrites EngineCallbacks.
     *
     * @param observer State callback (nullable).
     * @param user_data Forwarded to observer.
     * @utility
     * @version 2.1.10
     */
    void set_state_observer(
        void (*observer)(int, void*), void* user_data) {
        state_observer_ = observer;
        state_observer_data_ = user_data;
    }

    /**
     * @brief Get the observer's user_data pointer.
     * @return Opaque pointer passed to observer callbacks.
     * @utility
     * @version 2.0.6-rc16
     */
    void* stream_observer_data() const { return stream_observer_data_; }
};

} // namespace entropic
