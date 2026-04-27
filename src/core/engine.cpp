// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file engine.cpp
 * @brief AgentEngine implementation — the agentic loop.
 * @version 1.8.6
 */

#include <entropic/core/engine.h>
#include <entropic/core/delegation.h>
#include <entropic/core/worktree.h>
#include <entropic/types/logging.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
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
 * @version 2.0.4
 */
AgentEngine::AgentEngine(
    const InferenceInterface& inference,
    const LoopConfig& loop_config,
    const CompactionConfig& compaction_config)
    : inference_(inference),
      loop_config_(loop_config),
      token_counter_(loop_config.context_length),
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
 * @brief Set the global stream observer.
 *
 * Delegates to ResponseGenerator which owns the choke point for every
 * generation path (streaming, batch, and child-loop delegations).
 *
 * @param observer Token callback (nullable).
 * @param user_data Forwarded to observer.
 * @internal
 * @version 2.0.6-rc16
 */
void AgentEngine::set_stream_observer(
    TokenCallback observer, void* user_data) {
    response_generator_.set_stream_observer(observer, user_data);
}

/**
 * @brief Register validation JSON provider for ON_COMPLETE context.
 * @param provider JSON builder callback (nullable).
 * @param user_data Forwarded to provider.
 * @internal
 * @version 2.0.6-rc17
 */
void AgentEngine::set_validation_provider(
    char* (*provider)(void*), void* user_data) {
    validation_provider_ = provider;
    validation_provider_data_ = user_data;
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
 * @brief Apply per-identity max_iterations / max_tool_calls_per_turn overrides.
 *
 * Reads override values from TierResolutionInterface.get_tier_param for
 * "max_iterations" and "max_tool_calls_per_turn". Stores results in ctx so
 * should_stop and ToolExecutor::truncate_to_limit can respect them without
 * coupling to the identity layer. No-op when locked_tier is empty or the
 * tier resolver is unset.
 *
 * @param ctx Loop context to update (effective_max_* fields).
 * @internal
 * @version 2.0.6-rc16
 */
void AgentEngine::apply_identity_overrides(LoopContext& ctx) {
    if (ctx.locked_tier.empty() || tier_res_.get_tier_param == nullptr) {
        return;
    }
    auto mi = tier_res_.get_tier_param(
        ctx.locked_tier, "max_iterations", tier_res_.user_data);
    if (!mi.empty()) {
        ctx.effective_max_iterations = std::atoi(mi.c_str());
        logger->info("[override] tier={} max_iterations={}",
                     ctx.locked_tier, ctx.effective_max_iterations);
    }
    auto mt = tier_res_.get_tier_param(
        ctx.locked_tier, "max_tool_calls_per_turn", tier_res_.user_data);
    if (!mt.empty()) {
        ctx.effective_max_tool_calls_per_turn = std::atoi(mt.c_str());
        logger->info("[override] tier={} max_tool_calls_per_turn={}",
                     ctx.locked_tier, ctx.effective_max_tool_calls_per_turn);
    }
}

/**
 * @brief Resolve effective max_iterations, honouring per-identity override.
 * @param ctx Loop context.
 * @return Per-identity override if set (>=0), otherwise LoopConfig default.
 * @internal
 * @version 2.0.6-rc16
 */
int AgentEngine::resolve_max_iterations(const LoopContext& ctx) const {
    return ctx.effective_max_iterations >= 0
        ? ctx.effective_max_iterations
        : loop_config_.max_iterations;
}

/**
 * @brief Resolve effective max_tool_calls_per_turn, honouring override.
 * @param ctx Loop context.
 * @return Per-identity override if set (>=0), otherwise LoopConfig default.
 * @internal
 * @version 2.0.6-rc16
 */
int AgentEngine::resolve_max_tool_calls(const LoopContext& ctx) const {
    return ctx.effective_max_tool_calls_per_turn >= 0
        ? ctx.effective_max_tool_calls_per_turn
        : loop_config_.max_tool_calls_per_turn;
}

/**
 * @brief Run the engine loop on a pre-built context.
 * @param ctx Loop context to execute.
 * @internal
 * @version 2.0.6-rc16.2
 */
void AgentEngine::run_loop(LoopContext& ctx) {
    reset_interrupt();
    pause_flag_.store(false);
    apply_identity_overrides(ctx);
    reinject_context_anchors(ctx);
    if (ctx.metrics.start_time == 0.0) {
        ctx.metrics.start_time = now_seconds();
    }
    set_state(ctx, AgentState::PLANNING);
    loop(ctx);
    ctx.metrics.end_time = now_seconds();
    // Per-tier accumulator (P2-15 follow-up, 2.0.6-rc16.2)
    auto& tm = per_tier_metrics_[
        ctx.locked_tier.empty() ? "lead" : ctx.locked_tier];
    tm.iterations  += ctx.metrics.iterations;
    tm.tool_calls  += ctx.metrics.tool_calls;
    tm.tokens_used += ctx.metrics.tokens_used;
    tm.errors      += ctx.metrics.errors;
    tm.end_time    += (ctx.metrics.end_time - ctx.metrics.start_time);
}

/**
 * @brief Run the engine on initial messages.
 * @param messages System + user messages.
 * @return Final messages.
 * @internal
 * @version 2.0.6-rc16.2
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
    last_metrics_ = ctx.metrics;  // P2-15: snapshot for entropic_status
    // Per-tier accumulator (P2-15 follow-up, 2.0.6-rc16.2)
    auto& tm = per_tier_metrics_[
        ctx.locked_tier.empty() ? "lead" : ctx.locked_tier];
    tm.iterations  += ctx.metrics.iterations;
    tm.tool_calls  += ctx.metrics.tool_calls;
    tm.tokens_used += ctx.metrics.tokens_used;
    tm.errors      += ctx.metrics.errors;
    tm.end_time    += (ctx.metrics.end_time - ctx.metrics.start_time);
    logger->info("Loop complete: {} iterations, {}ms",
                 ctx.metrics.iterations, ctx.metrics.duration_ms());
    return ctx.messages;
}

/**
 * @brief Main loop.
 * @param ctx Loop context.
 * @internal
 * @version 2.0.6-rc18
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

    if (ctx.metrics.iterations >= resolve_max_iterations(ctx)
        && ctx.state != AgentState::COMPLETE
        && ctx.state != AgentState::ERROR
        && ctx.state != AgentState::INTERRUPTED) {
        // P3-18 follow-up (2.0.6-rc16.2): force synthetic completion
        // so the facade returns a proper result when the cap is hit.
        // E7 (2.0.6-rc18): mark ctx.metadata with the terminal reason
        // so delegate result and parent-tier relay can surface that
        // the child did not complete naturally.
        logger->warn("Loop ended due to max iterations ({}/{}) — "
                     "forcing synthetic entropic.complete",
                     ctx.metrics.iterations, resolve_max_iterations(ctx));
        Message forced;
        forced.role = "assistant";
        forced.content = "[iteration cap reached after "
            + std::to_string(ctx.metrics.iterations)
            + " iterations — returning current state]";
        ctx.messages.push_back(std::move(forced));
        ctx.metadata["terminal_reason"] = "budget_exhausted";
        set_state(ctx, AgentState::COMPLETE);
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
 * @version 2.0.7.1
 */
void AgentEngine::execute_iteration(LoopContext& ctx) {
    logger->info("[LOOP] iter {}/{} state={} msgs={}",
                 ctx.metrics.iterations,
                 resolve_max_iterations(ctx),
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
    fire_post_generate_hook(result, ctx.locked_tier, ctx.messages);
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
 * @version 2.0.6-rc16
 */
void AgentEngine::evaluate_no_tool_decision(
    LoopContext& ctx,
    const std::string& content,
    const std::string& finish_reason) {
    if (handle_terminal_finish_reasons(ctx, finish_reason)) { return; }
    if (try_auto_chain(ctx, finish_reason, content)) {
        logger->info("[DECISION] auto-chain triggered");
        return;
    }
    if (record_explicit_completion_failure(ctx, finish_reason)) { return; }
    if (response_generator_.is_response_complete(content, "[]")) {
        logger->info("[DECISION] response complete");
        set_state(ctx, AgentState::COMPLETE);
    }
}

/**
 * @brief Short-circuit evaluation for terminal finish reasons.
 *
 * Handles "interrupted" and "length" before the tier-specific logic
 * runs. Keeps evaluate_no_tool_decision under the 3-return cap.
 * (P1-6, 2.0.6-rc16)
 *
 * @param ctx Loop context.
 * @param finish_reason Generation finish reason.
 * @return true if a terminal reason was handled (caller returns).
 * @utility
 * @version 2.0.6-rc16
 */
bool AgentEngine::handle_terminal_finish_reasons(
    LoopContext& ctx, const std::string& finish_reason) {
    if (finish_reason == "interrupted") {
        logger->info("[DECISION] interrupted");
        set_state(ctx, AgentState::INTERRUPTED);
        return true;
    }
    if (finish_reason == "length") {
        logger->info("[DECISION] length, continuing");
        return true;
    }
    return false;
}

/**
 * @brief Record a structured failure when a tier requires explicit
 *        completion but returned zero tool calls. (P1-6, 2.0.6-rc16)
 *
 * @param ctx Loop context (metadata is mutated).
 * @param finish_reason Generation finish reason (for logging).
 * @return true if the failure was recorded and state transitioned.
 * @utility
 * @version 2.0.6-rc16.2
 */
bool AgentEngine::record_explicit_completion_failure(
    LoopContext& ctx, const std::string& finish_reason) {
    if (!tier_requires_explicit_completion(ctx.locked_tier)) {
        return false;
    }
    auto& retries = ctx.metadata["zero_tool_call_retries"];
    int n = retries.empty() ? 0 : std::atoi(retries.c_str());
    logger->warn("[DECISION] tier '{}' requires explicit completion "
                 "but iteration emitted zero tool calls "
                 "(finish_reason={}) retry={}/2",
                 ctx.locked_tier, finish_reason, n);
    ctx.metadata["failure_reason"] =
        "zero_tool_calls_with_explicit_completion";
    ctx.metadata["failure_tier"] = ctx.locked_tier;
    if (n >= 2) {
        logger->error("[DECISION] zero-tool-call retries exhausted "
                      "(tier={}), failing turn", ctx.locked_tier);
        set_state(ctx, AgentState::ERROR);
        return true;
    }
    Message correction;
    correction.role = "user";
    correction.content =
        "[SYSTEM] Your previous response contained no tool call. "
        "You must end every turn with exactly one tool call "
        "(entropic.complete, entropic.delegate, or entropic.inspect). "
        "Retry.";
    ctx.messages.push_back(std::move(correction));
    retries = std::to_string(n + 1);
    set_state(ctx, AgentState::EXECUTING);
    return true;
}

/**
 * @brief Check if a tier contractually requires explicit completion.
 *
 * Delegates to the TierResolutionInterface, falling back to false
 * when no resolver is wired (preserves legacy behavior in tests that
 * omit tier metadata).
 *
 * @param tier Tier name.
 * @return true if the tier's explicit_completion flag is set.
 * @utility
 * @version 2.0.6-rc16.1
 */
bool AgentEngine::tier_requires_explicit_completion(
    const std::string& tier) const {
    if (tier_res_.get_tier_param == nullptr) { return false; }
    auto val = tier_res_.get_tier_param(
        tier, "explicit_completion", tier_res_.user_data);
    return val == "true" || val == "1";
}

/**
 * @brief Detect delegation cycles via ancestor chain check.
 *
 * Targets already present in delegation_ancestor_tiers or matching
 * the active locked_tier would re-enter an ancestor loop, which the
 * depth cap only catches after repeated waste. (P1-9, 2.0.6-rc16)
 *
 * @param ctx Loop context.
 * @param target Pending delegation target tier name.
 * @return true if target would close a cycle.
 * @utility
 * @version 2.0.6-rc16
 */
bool AgentEngine::is_delegation_cycle(
    const LoopContext& ctx, const std::string& target) const {
    if (target == ctx.locked_tier) { return true; }
    for (const auto& anc : ctx.delegation_ancestor_tiers) {
        if (anc == target) { return true; }
    }
    return false;
}

/**
 * @brief Check if loop should stop.
 * @param ctx Loop context.
 * @return true if termination condition met.
 * @internal
 * @version 2.0.6-rc16
 */
bool AgentEngine::should_stop(const LoopContext& ctx) const {
    bool terminal = ctx.state == AgentState::COMPLETE
                 || ctx.state == AgentState::ERROR
                 || ctx.state == AgentState::INTERRUPTED;
    bool at_limit = ctx.metrics.iterations >= resolve_max_iterations(ctx)
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
 *
 * Callable from any thread and polled per-token on the generation
 * path, so interrupt() runs many times for a single Ctrl+C. Log only
 * on the 0→1 transition to avoid flooding the session log.
 * (P1-4, 2.0.6-rc16)
 *
 * @internal
 * @version 2.0.6-rc16.1
 */
void AgentEngine::interrupt() {
    if (!interrupt_flag_.exchange(true)) {
        logger->info("Engine interrupted");
        // P1-10: propagate to external MCP transports so in-flight
        // tool calls abort alongside the generation loop.
        if (external_interrupt_cb_ != nullptr) {
            external_interrupt_cb_(external_interrupt_data_);
        }
    }
    pause_flag_.store(false);
}

/**
 * @brief Register external transport interrupt callback.
 *
 * Facade wires this to ServerManager::interrupt_external_tools so
 * AgentEngine::interrupt() unwinds tool dispatches within ~100ms.
 * (P1-10, 2.0.6-rc16)
 *
 * @param cb Callback (nullable).
 * @param user_data Forwarded to cb.
 * @internal
 * @version 2.0.6-rc16
 */
void AgentEngine::set_external_interrupt(void (*cb)(void*),
                                         void* user_data) {
    external_interrupt_cb_ = cb;
    external_interrupt_data_ = user_data;
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
 * @version 2.0.6-rc16.2
 */
void AgentEngine::dir_complete(
    LoopContext& ctx, const Directive& d, DirectiveResult& r) {
    const auto& cd = static_cast<const CompleteDirective&>(d);

    // P1-5 follow-up (2.0.6-rc16.2): signal validating phase so async
    // subscribers can progress queued/running/running:<tier> → validating.
    auto prior_state = ctx.state;
    set_state(ctx, AgentState::VERIFYING);

    // Fire ON_COMPLETE pre-hook — application can validate/reject
    if (fire_complete_hook(cd.summary, ctx)) {
        logger->info("[DIRECTIVE] complete REJECTED by hook → revising");
        // Revision path: inform subscribers the validator is rewriting.
        ctx.metadata["validator_phase"] = "revising";
        set_state(ctx, prior_state);
        r.stop_processing = false;  // don't stop — model should retry
        return;
    }

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
/**
 * @brief Build one ToolCall from a parsed JSON object.
 * @param obj JSON object with "name" and optional "arguments".
 * @param tc_str Original JSON string (used in id hash).
 * @return Populated ToolCall.
 * @internal
 * @version 2.0.2
 */
static ToolCall build_tool_call_from_json(
    const nlohmann::json& obj, const std::string& tc_str) {
    ToolCall tc;
    tc.name = obj.value("name", "");
    tc.id = "tc-" + std::to_string(
        std::hash<std::string>{}(tc.name + tc_str) & 0xFFFF);
    if (obj.contains("arguments") && obj["arguments"].is_object()) {
        tc.arguments_json = obj["arguments"].dump();
        for (auto& [k, v] : obj["arguments"].items()) {
            tc.arguments[k] = v.is_string()
                ? v.get<std::string>() : v.dump();
        }
    }
    return tc;
}

/**
 * @brief Decode a JSON tool-calls array string into ToolCall vector.
 * @param tc_str JSON array string ("[]" or "" yields empty result).
 * @return Parsed ToolCall vector (empty on parse failure or empty input).
 * @internal
 * @version 2.0.2
 */
static std::vector<ToolCall> decode_tool_calls_json(
    const std::string& tc_str) {
    std::vector<ToolCall> calls;
    if (tc_str == "[]" || tc_str.empty()) { return calls; }
    auto arr = nlohmann::json::parse(tc_str, nullptr, false);
    if (!arr.is_array()) { return calls; }
    for (const auto& obj : arr) {
        calls.push_back(build_tool_call_from_json(obj, tc_str));
    }
    return calls;
}

/**
 * @brief Parse tool calls from raw model output via adapter.
 *
 * Delegates raw extraction to the adapter's parse_tool_calls callback,
 * then fully parses the resulting JSON array into individual ToolCall
 * objects with populated name, id, and arguments fields.
 *
 * @param raw_content Raw model output string.
 * @return Pair of (cleaned content, fully-parsed tool call vector).
 * @utility
 * @version 2.0.2
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

    if (rc != 0) { return {cleaned_str, {}}; }
    auto calls = decode_tool_calls_json(tc_str);
    if (!calls.empty()) {
        logger->info("Parsed tool calls from model output");
    }
    return {cleaned_str, std::move(calls)};
}

/**
 * @brief Process tool call results and directives.
 * @param ctx Loop context.
 * @param tool_calls Parsed tool calls.
 * @internal
 * @version 2.0.2
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
    // Don't overwrite terminal states set by directive handlers
    if (ctx.state != AgentState::COMPLETE
        && ctx.state != AgentState::ERROR
        && ctx.state != AgentState::INTERRUPTED) {
        set_state(ctx, AgentState::EXECUTING);
    }
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
 * @brief 256-entry table mapping bytes to JSON escape sequences.
 *
 * Non-null entries replace the input byte with the escape string.
 * Null entries pass the byte through unchanged.
 *
 * @return Reference to static lookup table.
 * @utility
 * @version 2.0.7
 */
static const std::array<const char*, 256>& json_escape_table() {
    static const auto tbl = []() {
        std::array<const char*, 256> t{};
        t[static_cast<unsigned char>('"')]  = "\\\"";
        t[static_cast<unsigned char>('\\')] = "\\\\";
        t[static_cast<unsigned char>('\n')] = "\\n";
        t[static_cast<unsigned char>('\r')] = "\\r";
        t[static_cast<unsigned char>('\t')] = "\\t";
        return t;
    }();
    return tbl;
}

/**
 * @brief JSON-escape a string (no surrounding quotes).
 * @param s Raw string.
 * @return Escaped string safe for JSON embedding.
 * @utility
 * @version 2.0.7
 */
static std::string json_escape_engine(const std::string& s) {
    const auto& tbl = json_escape_table();
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        const char* esc = tbl[static_cast<unsigned char>(c)];
        if (esc) { out += esc; }
        else     { out += c; }
    }
    return out;
}

/**
 * @brief Build tool call manifest from conversation messages.
 *
 * Scans for messages with metadata["tool_name"] (tool results stored
 * by ToolExecutor) and builds a human-readable summary line per call.
 *
 * @param messages Conversation messages.
 * @return Manifest string, empty if no tool calls.
 * @utility
 * @version 2.0.7
 */
static std::string build_tool_manifest(
    const std::vector<Message>& messages) {
    std::string manifest;
    for (const auto& msg : messages) {
        auto it = msg.metadata.find("tool_name");
        if (it == msg.metadata.end() || it->second.empty()) {
            continue;
        }
        manifest += "- " + it->second
            + " \xe2\x86\x92 " + std::to_string(msg.content.size())
            + " chars\n";
    }
    return manifest;
}

/**
 * @brief Append executor history (if any) to the tool manifest.
 *
 * Enriches the validator revision prompt with iteration numbers and
 * elapsed times pulled from ToolExecutor's ToolCallHistory ring
 * buffer via the ToolExecutionInterface::history_json callback.
 * (P1-11, 2.0.6-rc16)
 *
 * @param base Existing manifest from messages.
 * @param tool_exec Tool execution interface.
 * @return Enriched manifest (base + history lines if available).
 * @utility
 * @version 2.0.6-rc16
 */
static std::string enrich_manifest_with_history(
    const std::string& base,
    const ToolExecutionInterface& tool_exec) {
    if (tool_exec.history_json == nullptr) { return base; }
    char* js = tool_exec.history_json(20, tool_exec.user_data);
    if (js == nullptr) { return base; }
    std::string out = base + "prior-iteration history: " + js + "\n";
    if (tool_exec.free_fn != nullptr) { tool_exec.free_fn(js); }
    return out;
}

/**
 * @brief Extract the first system message content from messages.
 * @param messages Conversation messages.
 * @return System prompt text, or empty string if none found.
 * @utility
 * @version 2.0.7
 */
static std::string extract_system_prompt(
    const std::vector<Message>& messages) {
    for (const auto& msg : messages) {
        if (msg.role == "system") { return msg.content; }
    }
    return {};
}

/**
 * @brief Fire POST_GENERATE hook with full context.
 *
 * Builds context JSON including content, tier, tool call manifest
 * (tool names + result sizes from messages), and the tier's system
 * prompt. The constitutional validator uses these to distinguish
 * grounded responses from ungrounded ones and to preserve identity
 * during revision.
 *
 * @param result Generation result (mutable — hook may revise content).
 * @param tier Active tier name at the time of generation.
 * @param messages Current conversation messages.
 * @internal
 * @version 2.0.6-rc16
 */
void AgentEngine::fire_post_generate_hook(
    GenerateResult& result,
    const std::string& tier,
    const std::vector<Message>& messages) {
    if (hooks_.fire_post == nullptr) {
        return;
    }
    auto manifest = build_tool_manifest(messages);
    manifest = enrich_manifest_with_history(manifest, tool_exec_);
    auto sys = extract_system_prompt(messages);
    std::string json =
        "{\"finish_reason\":\"" + result.finish_reason
        + "\",\"content\":\""    + json_escape_engine(result.content)
        + "\",\"tier\":\""       + tier
        + "\",\"tool_context\":\"" + json_escape_engine(manifest)
        + "\",\"system_prompt\":\"" + json_escape_engine(sys) + "\"}";
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
 * @brief Build a JSON array of tool results from context messages.
 *
 * Extracts messages with metadata["tool_name"] and serializes them
 * as [{"name":"...", "content":"..."}] for the ON_COMPLETE hook.
 *
 * @param messages Conversation messages.
 * @return JSON array string.
 * @utility
 * @version 2.0.10
 */
static std::string build_tool_results_json(
    const std::vector<Message>& messages) {
    std::string arr = "[";
    bool first = true;
    for (const auto& msg : messages) {
        auto it = msg.metadata.find("tool_name");
        if (it == msg.metadata.end() || it->second.empty()) {
            continue;
        }
        if (!first) { arr += ","; }
        arr += "{\"name\":\"" + json_escape_engine(it->second)
            + "\",\"content\":\"" + json_escape_engine(msg.content)
            + "\"}";
        first = false;
    }
    arr += "]";
    return arr;
}

/**
 * @brief Fire ON_COMPLETE pre-hook for citation/summary validation.
 *
 * Builds context JSON with all guaranteed ON_COMPLETE schema fields:
 *   summary      — entropic.complete summary text
 *   tier         — active tier that called complete
 *   tool_results — [{name, content}] from conversation messages
 *   iteration    — loop iteration count at completion time
 *   validation   — null, or {ran, verdict, violations, revisions_applied}
 *                  populated from the registered validation provider
 *
 * The application's hook handler can validate citations, check format,
 * etc. Returns true if the hook rejected the completion. (P2-12, E3)
 *
 * @param summary The entropic.complete summary text.
 * @param ctx Loop context with tool results in messages.
 * @return true if hook cancelled (completion rejected).
 * @internal
 * @version 2.0.6-rc17
 */
bool AgentEngine::fire_complete_hook(
    const std::string& summary,
    const LoopContext& ctx) {
    if (hooks_.fire_pre == nullptr) { return false; }

    auto tool_results = build_tool_results_json(ctx.messages);
    // E3 (2.0.6-rc17): splice validator verdict/violations so
    // ON_COMPLETE consumers can distinguish clean pass from
    // reverted-for-length / max-revisions-exhausted.
    std::string validation_block = ",\"validation\":null";
    if (validation_provider_ != nullptr) {
        char* v = validation_provider_(validation_provider_data_);
        if (v != nullptr) {
            validation_block = ",\"validation\":" + std::string(v);
            free(v);
        }
    }
    std::string json =
        "{\"summary\":\"" + json_escape_engine(summary)
        + "\",\"tier\":\"" + ctx.locked_tier
        + "\",\"tool_results\":" + tool_results
        + ",\"iteration\":" + std::to_string(ctx.metrics.iterations)
        + validation_block
        + "}";
    char* modified = nullptr;
    int rc = hooks_.fire_pre(hooks_.registry,
        ENTROPIC_HOOK_ON_COMPLETE, json.c_str(), &modified);
    if (modified != nullptr) {
        // Hook provided rejection feedback — inject as user message
        Message feedback;
        feedback.role = "user";
        feedback.content = std::string("[CITATION VALIDATION] ") + modified;
        const_cast<LoopContext&>(ctx).messages.push_back(
            std::move(feedback));
        free(modified);
    }
    return rc != 0;
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
 * @brief Append a delegation rejection message to the loop context.
 * @param ctx Loop context.
 * @internal
 * @version 2.0.2
 */
static void push_delegation_rejected(LoopContext& ctx) {
    Message reject;
    reject.role = "user";
    reject.content = "[DELEGATION REJECTED] Maximum delegation "
                     "depth (" + std::to_string(
                         AgentEngine::MAX_DELEGATION_DEPTH) +
                     ") reached.";
    ctx.messages.push_back(std::move(reject));
}

/**
 * @brief Append a structured cycle-rejection message to the loop
 *        context so the model can recover. Names the offending tier
 *        and the ancestor chain. (P1-9, 2.0.6-rc16)
 *
 * @param ctx Loop context.
 * @param target Target tier that would have closed the cycle.
 * @internal
 * @version 2.0.6-rc16
 */
static void push_delegation_cycle_rejected(
    LoopContext& ctx, const std::string& target) {
    std::string chain;
    for (const auto& anc : ctx.delegation_ancestor_tiers) {
        chain += anc + " -> ";
    }
    chain += ctx.locked_tier + " -> " + target;

    Message reject;
    reject.role = "user";
    reject.content = "[DELEGATION REJECTED] Circular delegation "
                     "detected: tier '" + target +
                     "' already in the active delegation chain "
                     "(" + chain + "). Choose a different target.";
    ctx.metadata["failure_reason"] = "delegation_cycle";
    ctx.metadata["failure_target"] = target;
    ctx.messages.push_back(std::move(reject));
}

/**
 * @brief Append a delegation result message to the loop context.
 * @param ctx Loop context.
 * @param target Target tier name.
 * @param result Delegation result.
 * @internal
 * @version 2.0.2
 */
static void push_delegation_result(LoopContext& ctx,
    const std::string& target, const DelegationResult& result) {
    std::string tag = result.success ? "COMPLETE" : "FAILED";
    Message msg;
    msg.role = "user";
    msg.content = "[DELEGATION " + tag + ": " + target + "] " + result.summary;
    ctx.messages.push_back(std::move(msg));
}

/**
 * @brief Execute a pending delegation from the tool result directives.
 *
 * On successful delegation the state transitions to COMPLETE unless the
 * tier's "explicit_completion" parameter is set, in which case the state
 * returns to EXECUTING so the parent loop continues.  On failure or
 * rejection the state is always EXECUTING.
 *
 * @param ctx Loop context with pending delegation.
 * @utility
 * @version 2.0.6-rc16.1
 */
void AgentEngine::execute_pending_delegation(LoopContext& ctx) {
    auto pending = std::move(*ctx.pending_delegation);
    ctx.pending_delegation.reset();

    if (ctx.delegation_depth >= MAX_DELEGATION_DEPTH) {
        logger->warn("Delegation rejected: depth {} >= max {}",
                     ctx.delegation_depth, MAX_DELEGATION_DEPTH);
        push_delegation_rejected(ctx);
        return;
    }

    // P1-9: reject A→B→A and longer cycles before running a child.
    if (is_delegation_cycle(ctx, pending.target)) {
        logger->warn("Delegation rejected: cycle on target tier '{}'",
                     pending.target);
        push_delegation_cycle_rejected(ctx, pending.target);
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

    std::optional<int> max_turns;
    if (pending.max_turns > 0) { max_turns = pending.max_turns; }

    DelegationManager mgr(run_child_loop_trampoline, this,
                          tier_res_, get_repo_dir());
    if (storage_.create_delegation != nullptr) {
        mgr.set_storage(&storage_);
    }

    auto result = mgr.execute_delegation(
        ctx, pending.target, pending.task, max_turns);
    push_delegation_result(ctx, pending.target, result);

    fire_delegation_complete(ctx, pending.target, result);
    fire_delegate_complete_hook(pending.target, result.success);

    finalize_delegation_result(ctx, result);
}

/**
 * @brief Shared relay machinery: fire hook, write summary, set COMPLETE.
 *
 * Called by both the normal and budget_exhausted relay paths in
 * finalize_delegation_result. Caller applies any content prefix before
 * passing summary.
 *
 * @param ctx Loop context.
 * @param summary Content to relay.
 * @internal
 * @version 2.1.0
 */
void AgentEngine::relay_partial_result(
    LoopContext& ctx, const std::string& summary) {
    GenerateResult relay_result;
    relay_result.content = summary;
    relay_result.finish_reason = "stop";
    relay_result.tool_calls_json = "[]";
    fire_post_generate_hook(
        relay_result, ctx.locked_tier, ctx.messages);
    ctx.metadata["explicit_completion_summary"] = relay_result.content;
    set_state(ctx, AgentState::COMPLETE);
}

/**
 * @brief Promote relayed delegate output or set next engine state.
 *
 * Handles the relay_single_delegate path (validate + promote), the
 * budget_exhausted relay path (partial tagged relay), and the standard
 * explicit_completion transition. Extracted from execute_pending_delegation
 * to keep that function under the 50-SLOC / 3-return quality gates.
 * (P0-3, P1-9, 2.0.6-rc16; budget_exhausted relay: E2, 2.1.0)
 *
 * @param ctx Loop context.
 * @param result Delegation result from DelegationManager.
 * @utility
 * @version 2.1.0
 */
void AgentEngine::finalize_delegation_result(
    LoopContext& ctx, const DelegationResult& result) {
    // delegation.cpp sets success=false when terminal_reason is non-empty,
    // so the two relay branches are disjoint — check them independently.
    const bool in_relay_tier =
        relay_single_delegate_tiers_.count(ctx.locked_tier) > 0;
    if (result.success && in_relay_tier) {
        relay_partial_result(ctx, result.summary);
        log_relay_status(ctx);
        return;
    }
    if (!result.terminal_reason.empty() && in_relay_tier) {
        relay_partial_result(ctx,
            "[partial — budget_exhausted] " + result.summary);
        log_relay_status(ctx, result.terminal_reason);
        return;
    }
    bool needs_explicit = tier_requires_explicit_completion(
        ctx.locked_tier);
    set_state(ctx, (result.success && !needs_explicit)
        ? AgentState::COMPLETE : AgentState::EXECUTING);
}

/**
 * @brief Emit a disambiguating log for the relay-took-delegate path.
 *
 * Distinguishes four cases: budget_exhausted partial relay, validation
 * ran and passed, validation was skipped (e.g. lead in skip_tiers), or
 * no validator is configured. Writes ctx.metadata["relay_status"] so
 * downstream (ON_COMPLETE hook context, ask_complete) can render it.
 * (E8, 2.0.6-rc18; budget_exhausted path: E2, 2.1.0)
 *
 * @param ctx Loop context (metadata mutated).
 * @param terminal_reason Non-empty when relaying a budget_exhausted child.
 * @internal
 * @version 2.1.0
 */
void AgentEngine::log_relay_status(LoopContext& ctx,
                                   const std::string& terminal_reason) {
    std::string verdict;
    if (validation_provider_ != nullptr) {
        char* v = validation_provider_(validation_provider_data_);
        if (v != nullptr) {
            std::string jv(v);
            free(v);
            auto pos = jv.find("\"verdict\":\"");
            if (pos != std::string::npos) {
                pos += 11;
                auto end = jv.find('"', pos);
                if (end != std::string::npos) {
                    verdict = jv.substr(pos, end - pos);
                }
            }
        }
    }
    if (!terminal_reason.empty()) {
        ctx.metadata["relay_status"] = "budget_exhausted_relayed";
        logger->warn(
            "Relay: single-delegate result used "
            "(partial — terminal_reason={}, verdict={})",
            terminal_reason, verdict.empty() ? "none" : verdict);
        return;
    }
    if (verdict == "skipped" || verdict.empty()) {
        ctx.metadata["relay_status"] = "validation_skipped";
        logger->info(
            "Relay: single-delegate result used "
            "(lead validation skipped per config)");
    } else {
        ctx.metadata["relay_status"] = "validated";
        logger->info(
            "Relay: single-delegate result used "
            "(passed lead validation)");
    }
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
 * @version 2.0.2
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

// ── Conversation state (v2.0.2) ─────────────────────────────

/**
 * @brief Set the system prompt for conversation state.
 * @param prompt Assembled system prompt.
 * @internal
 * @version 2.0.2
 */
void AgentEngine::set_system_prompt(const std::string& prompt) {
    system_prompt_ = prompt;
}

/**
 * @brief Set session logger for model transcript logging.
 * @param log Non-owning pointer (nullable).
 * @internal
 * @version 2.0.2
 */
void AgentEngine::set_session_logger(SessionLogger* log) {
    session_logger_ = log;
}

/**
 * @brief Run a single conversation turn (stateful).
 * @param input User input string.
 * @return Result messages from engine.
 * @internal
 * @version 2.0.2
 */
std::vector<Message> AgentEngine::run_turn(const std::string& input) {
    if (conversation_.empty() && !system_prompt_.empty()) {
        Message sys;
        sys.role = "system";
        sys.content = system_prompt_;
        conversation_.push_back(std::move(sys));
    }
    Message usr;
    usr.role = "user";
    usr.content = input;
    conversation_.push_back(std::move(usr));

    size_t sent_len = conversation_.size();
    auto result = run(conversation_);  // copy — run() may mutate
    for (size_t i = sent_len; i < result.size(); ++i) {
        conversation_.push_back(result[i]);
    }
    return result;
}

/**
 * @brief Run a streaming conversation turn (stateful).
 * @param input User input.
 * @param on_token Consumer callback (filtered, UTF-8 aligned).
 * @param user_data Consumer context.
 * @param cancel_flag Polled per-token, nullable.
 * @return 0=OK, 1=cancelled, 2=error.
 * @internal
 * @version 2.0.2
 */
int AgentEngine::run_streaming(
    const std::string& input,
    TokenCallback on_token,
    void* user_data,
    int* cancel_flag)
{
    if (session_logger_) {
        session_logger_->log_user_input(input);
    }

    StreamThinkFilter filter(on_token, user_data);
    if (session_logger_ && session_logger_->is_open()) {
        filter.set_raw_callback(
            SessionLogger::raw_token_callback,
            session_logger_);
    }

    struct Ctx {
        StreamThinkFilter* filter;
        int* cancel;
        AgentEngine* engine;
    };
    Ctx sctx{&filter, cancel_flag, this};

    EngineCallbacks cbs{};
    cbs.on_stream_chunk = [](const char* t, size_t l, void* ud) {
        auto* c = static_cast<Ctx*>(ud);
        if (c->cancel && *c->cancel) {
            c->engine->interrupt();
            return;
        }
        c->filter->on_token(t, l);
    };
    cbs.user_data = &sctx;
    set_callbacks(cbs);

    auto result = run_turn(input);
    filter.flush();

    if (session_logger_) {
        session_logger_->end_turn();
    }
    if (cancel_flag && *cancel_flag) { return 1; }
    return 0;
}

/**
 * @brief Clear conversation history.
 * @internal
 * @version 2.0.2
 */
void AgentEngine::clear_conversation() {
    conversation_.clear();
    logger->info("conversation cleared");
}

/**
 * @brief Get conversation message count.
 * @return Number of messages.
 * @internal
 * @version 2.0.2
 */
size_t AgentEngine::message_count() const {
    return conversation_.size();
}

/**
 * @brief Get conversation messages.
 * @return Const reference to messages.
 * @internal
 * @version 2.0.2
 */
const std::vector<Message>& AgentEngine::get_messages() const {
    return conversation_;
}

// ── Directive hooks (v2.0.2) ────────────────────────────────

/**
 * @brief Build ToolExecutorHooks wired to this engine's DirectiveProcessor.
 * @return Configured hooks.
 * @internal
 * @version 2.0.2
 */
ToolExecutorHooks AgentEngine::build_directive_hooks() {
    ToolExecutorHooks hooks;
    hooks.process_directives = [](
        LoopContext& ctx,
        const std::vector<const Directive*>& dirs,
        void* ud) -> DirectiveResult {
        return static_cast<AgentEngine*>(ud)
            ->directive_processor().process(ctx, dirs);
    };
    hooks.user_data = this;
    return hooks;
}

// ── Tier resolution (v2.0.2) ────────────────────────────────

/**
 * @brief Store pre-resolved tier context info.
 * @param name Tier name.
 * @param info Context info.
 * @internal
 * @version 2.0.2
 */
void AgentEngine::set_tier_info(
    const std::string& name,
    const ChildContextInfo& info)
{
    tier_info_[name] = info;
}

/**
 * @brief Mark a tier as relay-on-single-delegate.
 * @param name Tier name.
 * @internal
 * @version 2.0.11
 */
void AgentEngine::set_relay_single_delegate(const std::string& name) {
    relay_single_delegate_tiers_.insert(name);
}

/**
 * @brief Store handoff rules.
 * @param rules Source tier → valid targets.
 * @internal
 * @version 2.0.2
 */
void AgentEngine::set_handoff_rules(
    const std::unordered_map<std::string,
        std::vector<std::string>>& rules)
{
    handoff_rules_ = rules;
    wire_internal_tier_resolution();
}

/**
 * @brief Wire internal TierResolutionInterface from stored data.
 * @internal
 * @version 2.0.2
 */
ChildContextInfo AgentEngine::tri_resolve_tier(
    const std::string& name, void* ud) {
    auto* self = static_cast<AgentEngine*>(ud);
    auto it = self->tier_info_.find(name);
    ChildContextInfo info;
    if (it == self->tier_info_.end()) {
        info.valid = false;
    } else {
        info = it->second;
    }
    return info;
}

/**
 * @brief Check if a tier exists in the tier info map.
 * @param name Tier name.
 * @param ud AgentEngine pointer.
 * @return true if tier exists.
 * @utility
 * @version 2.0.2
 */
bool AgentEngine::tri_tier_exists(const std::string& name, void* ud) {
    auto* self = static_cast<AgentEngine*>(ud);
    return self->tier_info_.count(name) > 0;
}

/**
 * @brief Get handoff targets for a tier.
 * @param name Source tier name.
 * @param ud AgentEngine pointer.
 * @return List of target tier names.
 * @utility
 * @version 2.0.2
 */
std::vector<std::string> AgentEngine::tri_get_handoff_targets(
    const std::string& name, void* ud) {
    auto* self = static_cast<AgentEngine*>(ud);
    auto it = self->handoff_rules_.find(name);
    std::vector<std::string> result;
    if (it != self->handoff_rules_.end()) { result = it->second; }
    return result;
}

/**
 * @brief Get a named parameter for a tier.
 * @param name Tier name.
 * @param param Parameter name.
 * @param ud AgentEngine pointer.
 * @return Parameter value string.
 * @utility
 * @version 2.0.2
 */
/**
 * @brief Look up named tier parameter (TierResolutionInterface trampoline).
 *
 * Surfaces explicit_completion, max_iterations, and max_tool_calls_per_turn
 * from pre-resolved ChildContextInfo. Returns empty string when tier or
 * param is not found, or when a numeric override is unset (-1).
 *
 * @param name Tier name.
 * @param param Parameter key.
 * @param ud Untyped AgentEngine* pointer.
 * @return String value, empty if tier or param unknown.
 * @internal
 * @version 2.0.6-rc16
 */
std::string AgentEngine::tri_get_tier_param(const std::string& name,
    const std::string& param, void* ud) {
    auto* self = static_cast<AgentEngine*>(ud);
    auto it = self->tier_info_.find(name);
    if (it == self->tier_info_.end()) { return ""; }
    const auto& info = it->second;
    std::string result;
    if (param == "explicit_completion") {
        result = info.explicit_completion ? "true" : "false";
    } else if (param == "max_iterations"
               && info.max_iterations_override >= 0) {
        result = std::to_string(info.max_iterations_override);
    } else if (param == "max_tool_calls_per_turn"
               && info.max_tool_calls_per_turn_override >= 0) {
        result = std::to_string(info.max_tool_calls_per_turn_override);
    }
    return result;
}

/**
 * @brief Wire internal tier resolution callbacks into the delegation manager.
 * @utility
 * @version 2.0.2
 */
void AgentEngine::wire_internal_tier_resolution() {
    TierResolutionInterface tri;
    tri.resolve_tier = &AgentEngine::tri_resolve_tier;
    tri.tier_exists = &AgentEngine::tri_tier_exists;
    tri.get_handoff_targets = &AgentEngine::tri_get_handoff_targets;
    tri.get_tier_param = &AgentEngine::tri_get_tier_param;
    tri.user_data = this;
    set_tier_resolution(tri);
}

} // namespace entropic
