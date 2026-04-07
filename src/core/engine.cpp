/**
 * @file engine.cpp
 * @brief AgentEngine implementation — the agentic loop.
 * @version 1.8.6
 */

#include <entropic/core/engine.h>
#include <entropic/core/delegation.h>
#include <entropic/core/worktree.h>
#include <entropic/types/logging.h>

#include <algorithm>
#include <chrono>
#include <sstream>

static auto logger = entropic::log::get("core.engine");

namespace entropic {

// ── Hook dispatch helpers ────────────────────────────────

/**
 * @brief Fire an informational hook if the interface is wired.
 * @param hooks Hook interface.
 * @param point Hook point.
 * @param json Context JSON.
 * @internal
 * @version 1.9.1
 */
static void fire_hook_info(const HookInterface& hooks,
                           entropic_hook_point_t point,
                           const char* json) {
    if (hooks.fire_info != nullptr) {
        hooks.fire_info(hooks.registry, point, json);
    }
}

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
 * @brief Set tier resolution interface for delegation.
 * @param tier_res Tier resolution callbacks.
 * @internal
 * @version 1.8.6
 */
void AgentEngine::set_tier_resolution(
    const TierResolutionInterface& tier_res) {
    tier_res_ = tier_res;
}

/**
 * @brief Set the storage interface for persistence.
 * @param storage Storage callbacks (nullable).
 * @internal
 * @version 1.8.8
 */
void AgentEngine::set_storage(const StorageInterface& storage) {
    storage_ = storage;
    compaction_manager_.set_storage(&storage_);
}

/**
 * @brief Set the hook dispatch interface.
 * @param hooks Hook dispatch interface.
 * @internal
 * @version 1.9.1
 */
void AgentEngine::set_hooks(const HookInterface& hooks) {
    hooks_ = hooks;
    response_generator_.set_hooks(hooks);
    context_manager_.set_hooks(hooks);
    directive_processor_.set_hooks(hooks);
}

/**
 * @brief Get the tier resolution interface.
 * @return Tier resolution interface.
 * @internal
 * @version 1.8.6
 */
const TierResolutionInterface& AgentEngine::tier_resolution() const {
    return tier_res_;
}

/**
 * @brief Run the engine loop on a pre-built context.
 * @param ctx Loop context to execute.
 * @internal
 * @version 1.8.6
 */
void AgentEngine::run_loop(LoopContext& ctx) {
    reset_interrupt();
    pause_flag_.store(false);
    reinject_context_anchors(ctx);
    set_state(ctx, AgentState::PLANNING);
    loop(ctx);
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
 * @version 1.9.1
 */
void AgentEngine::loop(LoopContext& ctx) {
    // Hook: ON_LOOP_START (v1.9.1)
    {
        std::string json = "{\"message_count\":"
            + std::to_string(ctx.messages.size())
            + ",\"delegation_depth\":"
            + std::to_string(ctx.delegation_depth) + "}";
        fire_hook_info(hooks_, ENTROPIC_HOOK_ON_LOOP_START, json.c_str());
    }

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

    // Hook: ON_LOOP_END (v1.9.1)
    {
        std::string json = "{\"final_state\":\""
            + std::string(agent_state_name(ctx.state))
            + "\",\"iterations\":"
            + std::to_string(ctx.metrics.iterations) + "}";
        fire_hook_info(hooks_, ENTROPIC_HOOK_ON_LOOP_END, json.c_str());
    }
}

/**
 * @brief Execute a single loop iteration.
 * @param ctx Loop context.
 * @internal
 * @version 2.0.0
 */
void AgentEngine::execute_iteration(LoopContext& ctx) {
    logger->info("[LOOP] iter {}/{} state={} msgs={}",
                 ctx.metrics.iterations,
                 loop_config_.max_iterations,
                 agent_state_name(ctx.state),
                 ctx.messages.size());

    // Hook: ON_LOOP_ITERATION (v1.9.1)
    {
        std::string json = "{\"iteration\":"
            + std::to_string(ctx.metrics.iterations)
            + ",\"state\":\"" + agent_state_name(ctx.state)
            + "\",\"consecutive_errors\":"
            + std::to_string(ctx.consecutive_errors) + "}";
        fire_hook_info(hooks_, ENTROPIC_HOOK_ON_LOOP_ITERATION, json.c_str());
    }

    context_manager_.refresh_context_limit(ctx, 0);
    context_manager_.prune_old_tool_results(ctx);
    context_manager_.check_compaction(ctx);

    // Hook: ON_CONTEXT_ASSEMBLE (v1.9.1)
    {
        std::string json = "{\"message_count\":"
            + std::to_string(ctx.messages.size()) + "}";
        fire_hook_info(hooks_, ENTROPIC_HOOK_ON_CONTEXT_ASSEMBLE,
                       json.c_str());
    }

    set_state(ctx, AgentState::EXECUTING);

    // Hook: PRE_GENERATE — can cancel (v1.9.1)
    if (fire_pre_hook(ENTROPIC_HOOK_PRE_GENERATE, ctx.metrics.iterations)) {
        set_state(ctx, AgentState::COMPLETE);
        return;
    }

    auto result = response_generator_.generate_response(ctx);
    fire_post_generate_hook(result, ctx.locked_tier);
    auto [cleaned, tool_calls] = parse_tool_calls(result.content);
    logger->info("[ITER] finish={}, {} tool call(s), {} chars",
                 result.finish_reason, tool_calls.size(),
                 cleaned.size());
    Message assistant_msg{"assistant", cleaned};
    ctx.messages.push_back(std::move(assistant_msg));

    if (!tool_calls.empty() && tool_exec_.process_tool_calls != nullptr) {
        process_tool_results(ctx, tool_calls);
    } else {
        evaluate_no_tool_decision(ctx, cleaned, result.finish_reason);
    }

    // Execute pending delegation/pipeline (v1.8.6)
    if (ctx.pending_delegation.has_value()) {
        ctx.metrics.iterations--;
        execute_pending_delegation(ctx);
    } else if (ctx.pending_pipeline.has_value()) {
        ctx.metrics.iterations--;
        execute_pending_pipeline(ctx);
    }
}

/**
 * @brief Evaluate what to do when no tool calls were effective.
 * @param ctx Loop context.
 * @param content Response content.
 * @param finish_reason Finish reason from generation.
 * @internal
 * @version 1.8.6
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

    // Auto-chain check (v1.8.6)
    if (try_auto_chain(ctx, finish_reason, content)) {
        logger->info("[DECISION] auto-chain triggered");
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
 * @version 1.9.1
 */
void AgentEngine::set_state(LoopContext& ctx, AgentState state) {
    auto prev = ctx.state;
    ctx.state = state;
    logger->info("State: {}", agent_state_name(state));
    if (callbacks_.on_state_change != nullptr) {
        callbacks_.on_state_change(
            static_cast<int>(state), callbacks_.user_data);
    }

    // Hook: ON_STATE_CHANGE (v1.9.1)
    {
        std::string json = "{\"previous\":\""
            + std::string(agent_state_name(prev))
            + "\",\"current\":\""
            + std::string(agent_state_name(state)) + "\"}";
        fire_hook_info(hooks_, ENTROPIC_HOOK_ON_STATE_CHANGE, json.c_str());
    }

    // Hook: ON_ERROR when entering ERROR state (v1.9.1)
    if (state == AgentState::ERROR) {
        std::string json = "{\"error_code\":\"STATE_ERROR\""
            ",\"iteration\":"
            + std::to_string(ctx.metrics.iterations)
            + ",\"consecutive\":" + std::to_string(ctx.consecutive_errors)
            + "}";
        fire_hook_info(hooks_, ENTROPIC_HOOK_ON_ERROR, json.c_str());
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
 * @version 1.8.6
 */
void AgentEngine::dir_delegate(
    LoopContext& ctx, const Directive& d, DirectiveResult& r) {
    const auto& dl = static_cast<const DelegateDirective&>(d);
    ctx.pending_delegation = PendingDelegation{
        dl.target, dl.task, dl.max_turns};
    r.stop_processing = true;
    logger->info("[DIRECTIVE] delegate: target={} task='{}'",
                 dl.target, dl.task);
}

/**
 * @brief Handle pipeline directive (store pending).
 * @internal
 * @version 1.8.6
 */
void AgentEngine::dir_pipeline(
    LoopContext& ctx, const Directive& d, DirectiveResult& r) {
    const auto& pl = static_cast<const PipelineDirective&>(d);
    ctx.pending_pipeline = PendingPipeline{pl.stages, pl.task};
    r.stop_processing = true;
    logger->info("[DIRECTIVE] pipeline: {} stages", pl.stages.size());
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
 * @version 2.0.0
 */
void AgentEngine::process_tool_results(
    LoopContext& ctx,
    const std::vector<ToolCall>& tool_calls) {
    set_state(ctx, AgentState::WAITING_TOOL);

    auto results = tool_exec_.process_tool_calls(
        ctx, tool_calls, tool_exec_.user_data);

    logger->info("[TOOLS] {} call(s) -> {} result message(s)",
                 tool_calls.size(), results.size());
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

// ── Hook helpers (v1.9.1) ────────────────────────────────

/**
 * @brief Fire a pre-hook, returning true if cancelled.
 * @param point Hook point.
 * @param iteration Current iteration.
 * @return true if cancelled.
 * @internal
 * @version 1.9.1
 */
bool AgentEngine::fire_pre_hook(
    entropic_hook_point_t point, int iteration) {
    if (hooks_.fire_pre == nullptr) {
        return false;
    }
    std::string json = "{\"iteration\":"
        + std::to_string(iteration) + "}";
    char* modified = nullptr;
    int rc = hooks_.fire_pre(hooks_.registry,
        point, json.c_str(), &modified);
    free(modified);
    return rc != 0;
}

/**
 * @brief Fire POST_GENERATE hook.
 * @param result Generation result.
 * @internal
 * @version 2.0.0
 */
void AgentEngine::fire_post_generate_hook(
    GenerateResult& result, const std::string& tier) {
    if (hooks_.fire_post == nullptr) {
        return;
    }
    std::string escaped;
    escaped.reserve(result.content.size());
    for (char c : result.content) {
        if (c == '"') { escaped += "\\\""; }
        else if (c == '\\') { escaped += "\\\\"; }
        else if (c == '\n') { escaped += "\\n"; }
        else { escaped += c; }
    }
    std::string json = "{\"finish_reason\":\""
        + result.finish_reason
        + "\",\"content\":\"" + escaped
        + "\",\"tier\":\"" + tier + "\"}";
    char* out = nullptr;
    hooks_.fire_post(hooks_.registry,
        ENTROPIC_HOOK_POST_GENERATE, json.c_str(), &out);
    if (out != nullptr) {
        result.content = out;
        logger->info("POST_GENERATE hook revised content");
        free(out);
    }
}

/**
 * @brief Fire ON_DELEGATE pre-hook.
 * @param pending Delegation info.
 * @param depth Delegation depth.
 * @return true if cancelled.
 * @internal
 * @version 1.9.1
 */
bool AgentEngine::fire_delegate_pre_hook(
    const PendingDelegation& pending, int depth) {
    if (hooks_.fire_pre == nullptr) {
        return false;
    }
    std::string json = "{\"target_tier\":\""
        + pending.target + "\",\"task\":\""
        + pending.task + "\",\"depth\":"
        + std::to_string(depth) + "}";
    char* modified = nullptr;
    int rc = hooks_.fire_pre(hooks_.registry,
        ENTROPIC_HOOK_ON_DELEGATE, json.c_str(), &modified);
    free(modified);
    return rc != 0;
}

/**
 * @brief Fire ON_DELEGATE_COMPLETE post-hook.
 * @param target Target tier.
 * @param success Whether delegation succeeded.
 * @internal
 * @version 1.9.1
 */
void AgentEngine::fire_delegate_complete_hook(
    const std::string& target, bool success) {
    if (hooks_.fire_post == nullptr) {
        return;
    }
    std::string json = "{\"target_tier\":\""
        + target + "\",\"success\":"
        + (success ? "true" : "false") + "}";
    char* out = nullptr;
    hooks_.fire_post(hooks_.registry,
        ENTROPIC_HOOK_ON_DELEGATE_COMPLETE, json.c_str(), &out);
    free(out);
}

// ── Delegation execution (v1.8.6) ────────────────────────

/**
 * @brief Trampoline for DelegationManager to call engine loop.
 * @param ctx Child loop context.
 * @param user_data AgentEngine pointer.
 * @internal
 * @version 1.8.6
 */
static void run_child_loop_trampoline(LoopContext& ctx, void* user_data) {
    auto* engine = static_cast<AgentEngine*>(user_data);
    engine->run_loop(ctx);
}

/**
 * @brief Execute a pending delegation after tool processing.
 * @param ctx Loop context with pending_delegation set.
 * @internal
 * @version 1.9.1
 */
void AgentEngine::execute_pending_delegation(LoopContext& ctx) {
    auto pending = std::move(*ctx.pending_delegation);
    ctx.pending_delegation.reset();

    if (ctx.delegation_depth >= MAX_DELEGATION_DEPTH) {
        logger->warn("Delegation rejected: depth {} >= max {}",
                     ctx.delegation_depth, MAX_DELEGATION_DEPTH);
        Message reject;
        reject.role = "user";
        reject.content = "[DELEGATION REJECTED] Maximum delegation "
                         "depth (" + std::to_string(MAX_DELEGATION_DEPTH) +
                         ") reached.";
        ctx.messages.push_back(std::move(reject));
        return;
    }

    set_state(ctx, AgentState::DELEGATING);

    // Hook: ON_DELEGATE pre-hook — can cancel (v1.9.1)
    if (fire_delegate_pre_hook(pending, ctx.delegation_depth)) {
        logger->info("ON_DELEGATE hook cancelled delegation");
        set_state(ctx, AgentState::EXECUTING);
        return;
    }

    fire_delegation_start(ctx, pending.target, pending.task);

    auto repo_dir = get_repo_dir();
    std::optional<int> max_turns;
    if (pending.max_turns > 0) {
        max_turns = pending.max_turns;
    }

    DelegationManager mgr(run_child_loop_trampoline, this,
                          tier_res_, repo_dir);
    if (storage_.create_delegation != nullptr) {
        mgr.set_storage(&storage_);
    }

    auto result = mgr.execute_delegation(
        ctx, pending.target, pending.task, max_turns);

    std::string tag = result.success ? "COMPLETE" : "FAILED";
    Message result_msg;
    result_msg.role = "user";
    result_msg.content = "[DELEGATION " + tag + ": " +
                         pending.target + "] " + result.summary;
    ctx.messages.push_back(std::move(result_msg));

    fire_delegation_complete(ctx, pending.target, result);
    fire_delegate_complete_hook(pending.target, result.success);
    set_state(ctx, AgentState::EXECUTING);
}

/**
 * @brief Execute a pending pipeline after tool processing.
 * @param ctx Loop context with pending_pipeline set.
 * @internal
 * @version 1.8.8
 */
void AgentEngine::execute_pending_pipeline(LoopContext& ctx) {
    auto pending = std::move(*ctx.pending_pipeline);
    ctx.pending_pipeline.reset();

    if (ctx.delegation_depth >= MAX_DELEGATION_DEPTH) {
        logger->warn("Pipeline rejected: depth {} >= max {}",
                     ctx.delegation_depth, MAX_DELEGATION_DEPTH);
        Message reject;
        reject.role = "user";
        reject.content = "[PIPELINE REJECTED] Maximum delegation "
                         "depth reached.";
        ctx.messages.push_back(std::move(reject));
        return;
    }

    set_state(ctx, AgentState::DELEGATING);

    auto repo_dir = get_repo_dir();
    DelegationManager mgr(run_child_loop_trampoline, this,
                          tier_res_, repo_dir);
    if (storage_.create_delegation != nullptr) {
        mgr.set_storage(&storage_);
    }
    auto result = mgr.execute_pipeline(
        ctx, pending.stages, pending.task);

    std::string tag = result.success ? "COMPLETE" : "FAILED";
    Message result_msg;
    result_msg.role = "user";
    result_msg.content = "[PIPELINE " + tag + "] " + result.summary;
    ctx.messages.push_back(std::move(result_msg));

    set_state(ctx, AgentState::EXECUTING);
}

/**
 * @brief Fire on_delegation_start callback if set.
 * @param ctx Loop context.
 * @param tier Target tier.
 * @param task Task description.
 * @internal
 * @version 1.8.6
 */
void AgentEngine::fire_delegation_start(
    const LoopContext& /*ctx*/,
    const std::string& tier,
    const std::string& task) {
    if (callbacks_.on_delegation_start != nullptr) {
        callbacks_.on_delegation_start(
            "", tier.c_str(), task.c_str(),
            callbacks_.user_data);
    }
}

/**
 * @brief Fire on_delegation_complete callback if set.
 * @param ctx Loop context.
 * @param tier Target tier.
 * @param result Delegation result.
 * @internal
 * @version 1.8.6
 */
void AgentEngine::fire_delegation_complete(
    const LoopContext& /*ctx*/,
    const std::string& tier,
    const DelegationResult& result) {
    if (callbacks_.on_delegation_complete != nullptr) {
        callbacks_.on_delegation_complete(
            "", tier.c_str(), result.summary.c_str(),
            result.success ? 1 : 0,
            callbacks_.user_data);
    }
}

// ── Auto-chain (v1.8.6) ─────────────────────────────────

/**
 * @brief Check if auto-chain should fire.
 * @param ctx Loop context.
 * @param finish_reason Generation finish reason.
 * @param content Response content.
 * @return true if auto-chain conditions met.
 * @internal
 * @version 1.8.6
 */
bool AgentEngine::should_auto_chain(
    const LoopContext& ctx,
    const std::string& finish_reason,
    const std::string& content) {
    if (ctx.locked_tier.empty() || tier_res_.get_tier_param == nullptr) {
        return false;
    }

    std::string auto_chain = tier_res_.get_tier_param(
        ctx.locked_tier, "auto_chain", tier_res_.user_data);
    if (auto_chain.empty()) {
        return false;
    }

    bool triggered = (finish_reason == "length") ||
        (finish_reason == "stop" &&
         response_generator_.is_response_complete(content, "[]"));
    return triggered;
}

/**
 * @brief Attempt auto-chain: child→COMPLETE, root→TierChange.
 * @param ctx Loop context.
 * @param finish_reason Generation finish reason.
 * @param content Response content.
 * @return true if auto-chain was triggered.
 * @internal
 * @version 1.8.6
 */
bool AgentEngine::try_auto_chain(
    LoopContext& ctx,
    const std::string& finish_reason,
    const std::string& content) {
    if (!should_auto_chain(ctx, finish_reason, content)) {
        return false;
    }

    if (ctx.delegation_depth > 0) {
        logger->info("[AUTO-CHAIN] child depth={}, completing",
                     ctx.delegation_depth);
        set_state(ctx, AgentState::COMPLETE);
        return true;
    }

    // Root: tier change to auto_chain target
    std::string target = tier_res_.get_tier_param(
        ctx.locked_tier, "auto_chain", tier_res_.user_data);

    if (!target.empty()) {
        logger->info("[AUTO-CHAIN] root, tier change to '{}'", target);
        TierChangeDirective tc(target, "auto_chain");
        DirectiveResult r;
        dir_tier_change(ctx, tc, r);
    }
    return !target.empty();
}

// ── Repo init (v1.8.6) ──────────────────────────────────

/**
 * @brief Get or discover the project git repository.
 * @return Repo directory, or empty if not found.
 * @internal
 * @version 1.8.6
 */
std::filesystem::path AgentEngine::get_repo_dir() {
    if (repo_dir_checked_) {
        return cached_repo_dir_.value_or(std::filesystem::path{});
    }
    repo_dir_checked_ = true;

    auto cwd = std::filesystem::current_path();
    bool found = std::filesystem::exists(cwd / ".git");
    bool inited = !found && init_project_repo(cwd);

    if (found || inited) {
        cached_repo_dir_ = cwd;
        logger->info("Git repo at: {}", cwd.string());
    } else {
        logger->warn("No git repo found or initialized");
    }
    return cached_repo_dir_.value_or(std::filesystem::path{});
}

/**
 * @brief Initialize a git repo if none exists.
 * @param project_dir Directory to init.
 * @return true if repo now exists.
 * @internal
 * @version 1.8.6
 */
bool AgentEngine::init_project_repo(
    const std::filesystem::path& project_dir) {
    auto r1 = run_git(project_dir, "init");
    if (!r1.success) {
        return false;
    }
    auto r2 = run_git(project_dir, "add -A");
    if (!r2.success) {
        return false;
    }
    run_git(project_dir, "commit --allow-empty -m 'Initial commit'");
    logger->info("Initialized git repo at: {}", project_dir.string());
    return true;
}

} // namespace entropic
