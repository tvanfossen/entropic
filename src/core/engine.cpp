/**
 * @file engine.cpp
 * @brief AgentEngine implementation — the agentic loop.
 * @version 1.8.4
 */

#include <entropic/core/engine.h>
#include <entropic/types/logging.h>

#include <algorithm>
#include <chrono>
#include <sstream>

static auto logger = entropic::log::get("core.engine");

namespace entropic {

// Forward declaration
static void remove_anchor_messages(LoopContext& ctx,
                                    const std::string& key);

/**
 * @brief Get current time as seconds since epoch.
 * @return Time in seconds (double).
 * @internal
 * @version 1.8.4
 */
static double now_seconds() {
    auto now = std::chrono::steady_clock::now();
    auto dur = now.time_since_epoch();
    auto ms = std::chrono::duration_cast<
        std::chrono::milliseconds>(dur);
    return static_cast<double>(ms.count()) / 1000.0;
}

/**
 * @brief Construct an agent engine.
 * @param inference Inference interface.
 * @param loop_config Loop configuration.
 * @param compaction_config Compaction configuration.
 * @internal
 * @version 1.8.4
 */
AgentEngine::AgentEngine(
    const InferenceInterface& inference,
    const LoopConfig& loop_config,
    const CompactionConfig& compaction_config)
    : inference_(inference),
      loop_config_(loop_config),
      token_counter_(16384),
      compaction_manager_(compaction_config, token_counter_),
      context_manager_(
          compaction_manager_, callbacks_,
          ContextManagerHooks{
              [this](LoopContext& ctx) { reinject_context_anchors(ctx); }
          }),
      response_generator_(
          inference, loop_config, callbacks_,
          GenerationEvents{&interrupt_flag_, &pause_flag_}) {
    register_directive_handlers();
}

/**
 * @brief Set callback functions for loop events.
 * @param callbacks Callback configuration.
 * @internal
 * @version 1.8.4
 */
void AgentEngine::set_callbacks(const EngineCallbacks& callbacks) {
    callbacks_ = callbacks;
}

/**
 * @brief Set the tool execution interface.
 * @param tool_exec Tool execution interface.
 * @internal
 * @version 1.8.5
 */
void AgentEngine::set_tool_executor(
    const ToolExecutionInterface& tool_exec) {
    tool_exec_ = tool_exec;
}

/**
 * @brief Run the engine on initial messages.
 * @param messages System + user messages.
 * @return Final messages.
 * @internal
 * @version 1.8.4
 */
std::vector<Message> AgentEngine::run(std::vector<Message> messages) {
    LoopContext ctx;
    ctx.messages = std::move(messages);
    ctx.metrics.start_time = now_seconds();

    reset_interrupt();
    pause_flag_.store(false);

    reinject_context_anchors(ctx);
    set_state(ctx, AgentState::PLANNING);

    loop(ctx);

    ctx.metrics.end_time = now_seconds();
    logger->info("Loop complete: {} iterations, {}ms",
                 ctx.metrics.iterations, ctx.metrics.duration_ms());
    return ctx.messages;
}

/**
 * @brief Main loop.
 * @param ctx Loop context.
 * @internal
 * @version 1.8.4
 */
void AgentEngine::loop(LoopContext& ctx) {
    while (!should_stop(ctx)) {
        ctx.metrics.iterations++;

        if (interrupt_flag_.load()) {
            set_state(ctx, AgentState::INTERRUPTED);
            break;
        }

        execute_iteration(ctx);

        if (ctx.state == AgentState::ERROR) {
            break;
        }
    }

    if (ctx.metrics.iterations >= loop_config_.max_iterations
        && ctx.state != AgentState::COMPLETE
        && ctx.state != AgentState::ERROR
        && ctx.state != AgentState::INTERRUPTED) {
        logger->warn("Loop ended due to max iterations");
    }
}

/**
 * @brief Execute a single loop iteration.
 * @param ctx Loop context.
 * @internal
 * @version 1.8.5
 */
void AgentEngine::execute_iteration(LoopContext& ctx) {
    logger->info("[LOOP] iter {}/{} state={} msgs={}",
                 ctx.metrics.iterations,
                 loop_config_.max_iterations,
                 agent_state_name(ctx.state),
                 ctx.messages.size());

    context_manager_.refresh_context_limit(ctx, 0);
    context_manager_.prune_old_tool_results(ctx);
    context_manager_.check_compaction(ctx);

    set_state(ctx, AgentState::EXECUTING);
    auto result = response_generator_.generate_response(ctx);

    // Parse tool calls from model output (v1.8.5)
    auto [cleaned, tool_calls] = parse_tool_calls(result.content);

    Message assistant_msg;
    assistant_msg.role = "assistant";
    assistant_msg.content = cleaned;
    ctx.messages.push_back(std::move(assistant_msg));

    if (!tool_calls.empty() && tool_exec_.process_tool_calls != nullptr) {
        process_tool_results(ctx, tool_calls);
    } else {
        evaluate_no_tool_decision(ctx, cleaned, result.finish_reason);
    }
}

/**
 * @brief Evaluate what to do when no tool calls were effective.
 * @param ctx Loop context.
 * @param content Response content.
 * @param finish_reason Finish reason from generation.
 * @internal
 * @version 1.8.4
 */
void AgentEngine::evaluate_no_tool_decision(
    LoopContext& ctx,
    const std::string& content,
    const std::string& finish_reason) {
    if (finish_reason == "interrupted") {
        logger->info("[DECISION] interrupted");
        set_state(ctx, AgentState::INTERRUPTED);
        return;
    }

    if (finish_reason == "length") {
        logger->info("[DECISION] length, continuing");
        return;
    }

    if (response_generator_.is_response_complete(content, "[]")) {
        logger->info("[DECISION] response complete");
        set_state(ctx, AgentState::COMPLETE);
    }
}

/**
 * @brief Check if loop should stop.
 * @param ctx Loop context.
 * @return true if termination condition met.
 * @internal
 * @version 1.8.4
 */
bool AgentEngine::should_stop(const LoopContext& ctx) const {
    bool terminal = ctx.state == AgentState::COMPLETE
                 || ctx.state == AgentState::ERROR
                 || ctx.state == AgentState::INTERRUPTED;
    bool at_limit = ctx.metrics.iterations >= loop_config_.max_iterations
                 || ctx.consecutive_duplicate_attempts >= 3;
    return terminal || at_limit;
}

/**
 * @brief Set agent state and fire callback.
 * @param ctx Loop context.
 * @param state New state.
 * @internal
 * @version 1.8.4
 */
void AgentEngine::set_state(LoopContext& ctx, AgentState state) {
    ctx.state = state;
    logger->info("State: {}", agent_state_name(state));
    if (callbacks_.on_state_change != nullptr) {
        callbacks_.on_state_change(
            static_cast<int>(state), callbacks_.user_data);
    }
}

/**
 * @brief Interrupt the running loop.
 * @internal
 * @version 1.8.4
 */
void AgentEngine::interrupt() {
    logger->info("Engine interrupted");
    interrupt_flag_.store(true);
    pause_flag_.store(false);
}

/**
 * @brief Reset interrupt flag.
 * @internal
 * @version 1.8.4
 */
void AgentEngine::reset_interrupt() {
    interrupt_flag_.store(false);
}

/**
 * @brief Pause generation.
 * @internal
 * @version 1.8.4
 */
void AgentEngine::pause() {
    logger->info("Engine paused");
    pause_flag_.store(true);
}

/**
 * @brief Cancel pause and interrupt.
 * @internal
 * @version 1.8.4
 */
void AgentEngine::cancel_pause() {
    logger->info("Pause cancelled, interrupting");
    pause_flag_.store(false);
    interrupt_flag_.store(true);
}

/**
 * @brief Get context usage.
 * @param messages Message list.
 * @return (tokens_used, max_tokens).
 * @internal
 * @version 1.8.4
 */
std::pair<int, int> AgentEngine::context_usage(
    const std::vector<Message>& messages) const {
    int used = token_counter_.count_messages(messages);
    return {used, token_counter_.max_tokens};
}

/**
 * @brief Reinject all cached context anchors.
 * @param ctx Loop context.
 * @internal
 * @version 1.8.4
 */
void AgentEngine::reinject_context_anchors(LoopContext& ctx) {
    for (const auto& [key, content] : context_anchors_) {
        ContextAnchorDirective d(key, content);
        DirectiveResult r;
        dir_anchor(ctx, d, r);
    }
}

// ── Directive handler registration ───────────────────────

/**
 * @brief Register all 11 directive handlers.
 * @internal
 * @version 1.8.4
 */
void AgentEngine::register_directive_handlers() {
    auto reg = [this](entropic_directive_type_t t, auto fn) {
        directive_processor_.register_handler(t,
            [this, fn](LoopContext& c, const Directive& d,
                       DirectiveResult& r) {
                (this->*fn)(c, d, r);
            });
    };
    reg(ENTROPIC_DIRECTIVE_STOP_PROCESSING, &AgentEngine::dir_stop);
    reg(ENTROPIC_DIRECTIVE_TIER_CHANGE,     &AgentEngine::dir_tier_change);
    reg(ENTROPIC_DIRECTIVE_DELEGATE,        &AgentEngine::dir_delegate);
    reg(ENTROPIC_DIRECTIVE_PIPELINE,        &AgentEngine::dir_pipeline);
    reg(ENTROPIC_DIRECTIVE_COMPLETE,        &AgentEngine::dir_complete);
    reg(ENTROPIC_DIRECTIVE_CLEAR_SELF_TODOS,&AgentEngine::dir_clear_todos);
    reg(ENTROPIC_DIRECTIVE_INJECT_CONTEXT,  &AgentEngine::dir_inject);
    reg(ENTROPIC_DIRECTIVE_PRUNE_MESSAGES,  &AgentEngine::dir_prune);
    reg(ENTROPIC_DIRECTIVE_CONTEXT_ANCHOR,  &AgentEngine::dir_anchor);
    reg(ENTROPIC_DIRECTIVE_PHASE_CHANGE,    &AgentEngine::dir_phase);
    reg(ENTROPIC_DIRECTIVE_NOTIFY_PRESENTER,&AgentEngine::dir_notify);
}

// ── Directive handlers ───────────────────────────────────

/**
 * @brief Handle stop_processing directive.
 * @internal
 * @version 1.8.4
 */
void AgentEngine::dir_stop(
    LoopContext&, const Directive&, DirectiveResult& r) {
    logger->info("[DIRECTIVE] stop_processing");
    r.stop_processing = true;
}

/**
 * @brief Handle tier_change directive.
 * @internal
 * @version 1.8.4
 */
void AgentEngine::dir_tier_change(
    LoopContext& ctx, const Directive& d, DirectiveResult& r) {
    const auto& tc = static_cast<const TierChangeDirective&>(d);
    ctx.locked_tier = tc.tier;
    r.tier_changed = true;
    logger->info("[DIRECTIVE] tier_change: {}", tc.tier);
}

/**
 * @brief Handle delegate directive (store pending).
 * @internal
 * @version 1.8.4
 */
void AgentEngine::dir_delegate(
    LoopContext& ctx, const Directive& d, DirectiveResult& r) {
    const auto& dl = static_cast<const DelegateDirective&>(d);
    ctx.metadata["pending_delegation"] = dl.target;
    r.stop_processing = true;
    logger->info("[DIRECTIVE] delegate: {}", dl.target);
}

/**
 * @brief Handle pipeline directive (store pending).
 * @internal
 * @version 1.8.4
 */
void AgentEngine::dir_pipeline(
    LoopContext& ctx, const Directive& d, DirectiveResult& r) {
    const auto& pl = static_cast<const PipelineDirective&>(d);
    ctx.metadata["pending_pipeline"] = pl.task;
    r.stop_processing = true;
    logger->info("[DIRECTIVE] pipeline: {}", pl.task);
}

/**
 * @brief Handle complete directive.
 * @internal
 * @version 1.8.4
 */
void AgentEngine::dir_complete(
    LoopContext& ctx, const Directive& d, DirectiveResult& r) {
    const auto& cd = static_cast<const CompleteDirective&>(d);
    ctx.metadata["explicit_completion_summary"] = cd.summary;
    set_state(ctx, AgentState::COMPLETE);
    r.stop_processing = true;
    logger->info("[DIRECTIVE] complete");
}

/**
 * @brief Handle clear_self_todos directive (no-op).
 * @internal
 * @version 1.8.4
 */
void AgentEngine::dir_clear_todos(
    LoopContext&, const Directive&, DirectiveResult&) {
    logger->debug("[DIRECTIVE] clear_self_todos (no-op)");
}

/**
 * @brief Handle inject_context directive.
 * @internal
 * @version 1.8.4
 */
void AgentEngine::dir_inject(
    LoopContext&, const Directive& d, DirectiveResult& r) {
    const auto& ic = static_cast<const InjectContextDirective&>(d);
    if (!ic.content.empty()) {
        Message msg;
        msg.role = ic.role;
        msg.content = ic.content;
        r.injected_messages.push_back(std::move(msg));
        logger->info("[DIRECTIVE] inject_context");
    }
}

/**
 * @brief Handle prune_messages directive.
 * @internal
 * @version 1.8.4
 */
void AgentEngine::dir_prune(
    LoopContext& ctx, const Directive& d, DirectiveResult&) {
    const auto& pm = static_cast<const PruneMessagesDirective&>(d);
    auto [pruned, freed] = context_manager_.prune_tool_results(
        ctx, pm.keep_recent);
    logger->info("[DIRECTIVE] prune: {} results, {} chars", pruned, freed);
}

/**
 * @brief Handle context_anchor directive.
 * @internal
 * @version 1.8.4
 */
void AgentEngine::dir_anchor(
    LoopContext& ctx, const Directive& d, DirectiveResult&) {
    const auto& ca = static_cast<const ContextAnchorDirective&>(d);
    if (ca.content.empty()) {
        context_anchors_.erase(ca.key);
        remove_anchor_messages(ctx, ca.key);
        logger->info("Removed anchor: {}", ca.key);
        return;
    }
    context_anchors_[ca.key] = ca.content;
    remove_anchor_messages(ctx, ca.key);
    Message anchor;
    anchor.role = "user";
    anchor.content = ca.content;
    anchor.metadata["is_context_anchor"] = "true";
    anchor.metadata["anchor_key"] = ca.key;
    ctx.messages.push_back(std::move(anchor));
    logger->info("Updated anchor: {}", ca.key);
}

/**
 * @brief Handle phase_change directive.
 * @internal
 * @version 1.8.4
 */
void AgentEngine::dir_phase(
    LoopContext& ctx, const Directive& d, DirectiveResult&) {
    const auto& pc = static_cast<const PhaseChangeDirective&>(d);
    ctx.active_phase = pc.phase;
    logger->info("[DIRECTIVE] phase_change: {}", pc.phase);
}

/**
 * @brief Handle notify_presenter directive.
 * @internal
 * @version 1.8.4
 */
void AgentEngine::dir_notify(
    LoopContext&, const Directive& d, DirectiveResult&) {
    const auto& np = static_cast<const NotifyPresenterDirective&>(d);
    if (callbacks_.on_presenter_notify != nullptr) {
        callbacks_.on_presenter_notify(
            np.key.c_str(), np.data_json.c_str(),
            callbacks_.user_data);
    }
}

// ── Tool call parsing (v1.8.5) ───────────────────────────

/**
 * @brief Parse tool calls from raw model output.
 * @param raw_content Raw model output.
 * @return (cleaned content, parsed tool calls).
 * @internal
 * @version 1.8.5
 */
std::pair<std::string, std::vector<ToolCall>>
AgentEngine::parse_tool_calls(const std::string& raw_content) {
    if (inference_.parse_tool_calls == nullptr) {
        return {raw_content, {}};
    }

    char* cleaned = nullptr;
    char* tc_json = nullptr;
    int rc = inference_.parse_tool_calls(
        raw_content.c_str(), &cleaned, &tc_json,
        inference_.adapter_data);

    std::string cleaned_str = cleaned ? cleaned : raw_content;
    std::string tc_str = tc_json ? tc_json : "[]";

    if (inference_.free_fn != nullptr) {
        if (cleaned != nullptr) { inference_.free_fn(cleaned); }
        if (tc_json != nullptr) { inference_.free_fn(tc_json); }
    }

    if (rc != 0 || tc_str == "[]" || tc_str.empty()) {
        return {cleaned_str, {}};
    }

    // Minimal JSON parse: tool_calls is a JSON array of objects
    // with "name" and "arguments" fields. This avoids pulling
    // nlohmann/json into core.so.
    // For now, store raw JSON — the ToolExecutor (in mcp.so)
    // will parse it via the ToolExecutionFn callback.
    ToolCall call;
    call.id = "tc-" + std::to_string(
        std::hash<std::string>{}(tc_str) & 0xFFFF);
    call.name = ""; // Parsed by the facade's callback
    // Store raw JSON in arguments for facade to parse
    call.arguments["_raw_json"] = tc_str;

    logger->info("Parsed tool calls from model output");
    return {cleaned_str, {call}};
}

/**
 * @brief Process tool call results and directives.
 * @param ctx Loop context.
 * @param tool_calls Parsed tool calls.
 * @internal
 * @version 1.8.5
 */
void AgentEngine::process_tool_results(
    LoopContext& ctx,
    const std::vector<ToolCall>& tool_calls) {
    set_state(ctx, AgentState::WAITING_TOOL);

    auto results = tool_exec_.process_tool_calls(
        ctx, tool_calls, tool_exec_.user_data);

    for (auto& msg : results) {
        ctx.messages.push_back(std::move(msg));
    }

    ctx.has_pending_tool_results = true;
    set_state(ctx, AgentState::EXECUTING);
}

// ── Anchor helper ────────────────────────────────────────

/**
 * @brief Remove messages with a specific anchor key.
 * @param ctx Loop context.
 * @param key Anchor key to remove.
 * @internal
 * @version 1.8.4
 */
static void remove_anchor_messages(LoopContext& ctx,
                                    const std::string& key) {
    auto& msgs = ctx.messages;
    msgs.erase(
        std::remove_if(msgs.begin(), msgs.end(),
            [&key](const Message& m) {
                auto it = m.metadata.find("anchor_key");
                return it != m.metadata.end() && it->second == key;
            }),
        msgs.end());
}

} // namespace entropic
