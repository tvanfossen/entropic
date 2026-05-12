// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file delegation.cpp
 * @brief DelegationManager implementation.
 *
 * Ports Python's DelegationManager. Child loop creation, sandbox
 * lifecycle, pipeline execution, todo list save/restore.
 *
 * v2.1.5 (gh#29): sandbox replaces worktree. The engine no longer
 * touches the user's git repo. Delegation output flows back via a
 * unified-diff patch produced by SandboxManager::finalize_sandbox().
 *
 * @version 2.1.5
 */

#include <entropic/core/delegation.h>
#include <entropic/core/engine_types.h>
#include <entropic/types/logging.h>

#include <nlohmann/json.hpp>

#include <algorithm>

static auto logger = entropic::log::get("core.delegation");

namespace entropic {

// ── Construction ─────────────────────────────────────────

/**
 * @brief Construct with engine loop callback and tier resolution.
 * @param run_child Callback to run child engine loop.
 * @param run_child_data Opaque pointer for run_child.
 * @param tier_resolution Tier resolution interface.
 * @param repo_dir Optional project root for sandbox isolation (gh#29).
 * @internal
 * @version 2.1.5
 */
DelegationManager::DelegationManager(
    RunChildLoopFn run_child,
    void* run_child_data,
    const TierResolutionInterface& tier_resolution,
    const std::filesystem::path& repo_dir)
    : run_child_fn_(run_child),
      run_child_data_(run_child_data),
      tier_res_(tier_resolution),
      repo_dir_(repo_dir) {
    if (!repo_dir_.empty()) {
        sandbox_mgr_.emplace(repo_dir_);
    }
}

/**
 * @brief Set todo list save/restore callbacks.
 * @param callbacks Todo callbacks.
 * @internal
 * @version 1.8.6
 */
void DelegationManager::set_todo_callbacks(const TodoCallbacks& callbacks) {
    todo_callbacks_ = callbacks;
}

/**
 * @brief Set directory swap callback for ScopedSandbox.
 * @param swap_fn Directory swap callback.
 * @param user_data Opaque pointer for swap_fn.
 * @internal
 * @version 2.1.5
 */
void DelegationManager::set_dir_swap(
    ScopedSandbox::SwapDirFn swap_fn, void* user_data) {
    swap_dir_fn_ = swap_fn;
    swap_dir_data_ = user_data;
}

/**
 * @brief Set storage interface for delegation record persistence.
 * @param storage Storage callbacks (nullable).
 * @internal
 * @version 1.8.8
 */
void DelegationManager::set_storage(const StorageInterface* storage) {
    storage_ = storage;
}

/**
 * @brief Set delegation start/complete callbacks.
 * @param on_start Pre-delegation gate (nullable).
 * @param on_complete Post-delegation result (nullable).
 * @param user_data Forwarded to both callbacks.
 * @internal
 * @version 2.1.5
 */
void DelegationManager::set_delegation_callbacks(
    ent_decision_t (*on_start)(const ent_delegation_request_t*, void*),
    ent_decision_t (*on_complete)(const ent_delegation_result_t*, void*),
    void* user_data) {
    delegation_start_cb_ = on_start;
    delegation_complete_cb_ = on_complete;
    delegation_cb_data_ = user_data;
}

/**
 * @brief Invoke `delegation_start_cb_` if registered.
 *
 * Builds the request descriptor and dispatches. Null callback defaults
 * to ACCEPT — the engine never refuses delegations without explicit
 * consumer opt-in.
 *
 * @param delegation_id Short id.
 * @param target_tier   Tier name.
 * @param task          Task description.
 * @param depth         Delegation depth.
 * @param is_pipeline   True for pipeline stages.
 * @return Decision from callback (or ACCEPT if null).
 * @internal
 * @version 2.1.5
 */
ent_decision_t DelegationManager::fire_start_cb(
    const std::string& delegation_id,
    const std::string& target_tier,
    const std::string& task,
    int depth,
    bool is_pipeline) {
    if (delegation_start_cb_ == nullptr) {
        return ENT_DECISION_ACCEPT;
    }
    ent_delegation_request_t req{};
    req.delegation_id = delegation_id.c_str();
    req.target_tier  = target_tier.c_str();
    req.task         = task.c_str();
    req.depth        = depth;
    req.is_pipeline  = is_pipeline ? 1 : 0;
    return delegation_start_cb_(&req, delegation_cb_data_);
}

/**
 * @brief Deliver a finalized patch to consumer or pending/.
 *
 * The complete callback is null-tolerant (default-deny → pending/).
 * If the callback returns REJECT the patch is also persisted to
 * pending/ for the consumer to recover later. The sandbox directory
 * is removed by the caller (`finalize_sandbox_for`) after this call
 * returns — independent of the consumer's decision (gh#29: engine
 * never retains writable state).
 *
 * @param sb_info        Sandbox identity.
 * @param sandbox_result Patch artifact.
 * @param result         Original delegation result.
 * @internal
 * @version 2.1.5
 */
void DelegationManager::deliver_sandbox_result(
    const SandboxInfo& sb_info,
    const SandboxResult& sandbox_result,
    const DelegationResult& result) {

    if (delegation_complete_cb_ == nullptr) {
        persist_pending_patch(sb_info, sandbox_result,
                              "no complete callback registered");
        return;
    }

    std::vector<std::string> files_owned;
    files_owned.reserve(sandbox_result.files_touched.size());
    for (const auto& p : sandbox_result.files_touched) {
        files_owned.push_back(p.string());
    }
    std::vector<const char*> files_c;
    files_c.reserve(files_owned.size() + 1);
    for (const auto& s : files_owned) { files_c.push_back(s.c_str()); }
    files_c.push_back(nullptr);

    ent_delegation_result_t res{};
    res.delegation_id     = sb_info.delegation_id.c_str();
    res.target_tier       = result.target_tier.c_str();
    res.success           = result.success ? 1 : 0;
    res.summary           = result.summary.c_str();
    res.patch             = sandbox_result.patch.c_str();
    res.patch_len         = sandbox_result.patch.size();
    res.files_touched     = files_c.data();
    res.files_touched_len = files_owned.size();

    auto decision = delegation_complete_cb_(&res, delegation_cb_data_);
    if (decision == ENT_DECISION_REJECT) {
        persist_pending_patch(sb_info, sandbox_result,
                              "consumer REJECTED");
    } else {
        logger->info("Delegation {}: consumer ACCEPTED ({} files, "
                     "{} bytes)",
                     sb_info.delegation_id,
                     sandbox_result.files_touched.size(),
                     sandbox_result.patch.size());
    }
}

/**
 * @brief Persist a patch to pending/ + log WARN.
 * @internal
 * @version 2.1.5
 */
void DelegationManager::persist_pending_patch(
    const SandboxInfo& sb_info,
    const SandboxResult& sandbox_result,
    const char* reason) {
    auto path = sandbox_mgr_->write_pending_patch(
        sb_info.delegation_id, sandbox_result.patch);
    if (path) {
        logger->warn("Delegation {}: {}; patch saved to {} "
                     "({} files, {} bytes)",
                     sb_info.delegation_id, reason, path->string(),
                     sandbox_result.files_touched.size(),
                     sandbox_result.patch.size());
    }
}

// ── Single delegation ────────────────────────────────────

/**
 * @brief Run a child inference loop for the target tier.
 * @param parent_ctx Parent loop context.
 * @param target_tier Tier name to delegate to.
 * @param task Task description for the child.
 * @param max_turns Optional iteration limit.
 * @return DelegationResult.
 * @internal
 * @version 2.1.5-cb
 */
DelegationResult DelegationManager::execute_delegation(
    LoopContext& parent_ctx,
    const std::string& target_tier,
    const std::string& task,
    std::optional<int> max_turns) {

    logger->info("Delegation: target_tier='{}', task='{}', depth={}",
                 target_tier, task,
                 parent_ctx.delegation_depth + 1);
    auto info = tier_res_.resolve_tier
        ? tier_res_.resolve_tier(target_tier, tier_res_.user_data)
        : ChildContextInfo{};

    if (!info.valid) {
        logger->error("Tier '{}' not found", target_tier);
        return {"Unknown tier: " + target_tier, false, target_tier, task};
    }

    auto child_ctx = build_child_context(parent_ctx, info, task);
    child_ctx.locked_tier = target_tier;

    // gh#29 (v2.1.5): consumer gate before any child loop runs. Engine
    // never proceeds past a REJECT — even if no sandbox is configured.
    std::string del_id =
        "d" + std::to_string(parent_ctx.delegation_depth + 1);
    if (fire_start_cb(del_id, target_tier, task,
                      parent_ctx.delegation_depth + 1, false)
        == ENT_DECISION_REJECT) {
        logger->info("Delegation {} ({}) rejected by start callback",
                     del_id, target_tier);
        return {"Delegation rejected by consumer", false,
                target_tier, task};
    }

    // Create per-delegation filesystem sandbox (gh#29: was a git
    // worktree, now an isolated copy under ~/.entropic/sandbox/).
    std::optional<SandboxInfo> sb_info;
    if (sandbox_mgr_) {
        sb_info = sandbox_mgr_->create_sandbox(del_id);
    }

    DelegationResult result;

    if (sb_info && swap_dir_fn_ != nullptr) {
        ScopedSandbox scope(swap_dir_fn_, swap_dir_data_,
                            sb_info->path, repo_dir_);
        result = run_child(child_ctx, target_tier, task, max_turns);
    } else {
        result = run_child(child_ctx, target_tier, task, max_turns);
    }

    finalize_sandbox_for(sb_info, result);
    return result;
}

// ── Pipeline ─────────────────────────────────────────────

/**
 * @brief Build pipeline context prefix for a stage.
 *
 * Issue #11 (v2.1.4): the optional `prior_output` argument carries
 * the previous stage's result forward as part of the next stage's
 * task message. Pre-2.1.4 each stage ran in isolation against the
 * ORIGINAL task only — surprising for prompt authors who reasonably
 * expected `pipeline` to mean "Unix-style chained pipeline" and
 * caused the multi-stage flow (e.g. researcher → reader) to lose
 * intermediate output. The intent was always forward-carry; the
 * isolation behavior was a bug.
 *
 * @param stage_idx Zero-based stage index.
 * @param total Total number of stages.
 * @param stages Stage tier names.
 * @param prior_output Previous stage's summary/result (empty for
 *                    stage 0).
 * @return Context string prepended to the task.
 * @version 2.1.4
 * @internal
 */
static std::string pipeline_context(
    size_t stage_idx, size_t total,
    const std::vector<std::string>& stages,
    const std::string& prior_output) {
    std::string ctx = "[PIPELINE CONTEXT]\n";
    ctx += "Stage " + std::to_string(stage_idx + 1) +
           " of " + std::to_string(total) + "\n";
    ctx += "Role: " + stages[stage_idx] + "\n";
    ctx += "Stay within your role. Do not perform work "
           "outside your stage's responsibility.\n";
    if (!prior_output.empty()) {
        ctx += "\n[PRIOR STAGE OUTPUT]\n";
        ctx += prior_output;
        ctx += "\n";
    }
    ctx += "\n";
    return ctx;
}

/**
 * @brief Run a multi-stage delegation pipeline sequentially.
 *
 * Issue #11 (v2.1.4): each stage now receives the prior stage's
 * output as a `[PRIOR STAGE OUTPUT]` block in its task message
 * (forward-carry semantics — the original intent that pre-2.1.4
 * was lost to per-stage isolation).
 *
 * @param parent_ctx Parent loop context.
 * @param stages Ordered list of tier names.
 * @param task Task description.
 * @return DelegationResult from the final stage.
 * @internal
 * @version 2.1.5-cb
 */
DelegationResult DelegationManager::execute_pipeline(
    LoopContext& parent_ctx,
    const std::vector<std::string>& stages,
    const std::string& task) {

    logger->info("Pipeline: {} stages, task='{}'", stages.size(), task);

    // gh#29 (v2.1.5): single pre-flight gate for the whole pipeline.
    auto first = stages.empty() ? std::string{} : stages.front();
    if (fire_start_cb("pipeline", first, task,
                      parent_ctx.delegation_depth + 1, true)
        == ENT_DECISION_REJECT) {
        logger->info("Pipeline rejected by start callback");
        return {"Pipeline rejected by consumer", false, first, task};
    }

    // Shared sandbox for the entire pipeline (gh#29). Each stage runs
    // inside the same directory so later stages observe earlier stages'
    // file edits — preserving the v2.1.4 forward-carry behavior.
    std::optional<SandboxInfo> shared_sb;
    if (sandbox_mgr_) {
        shared_sb = sandbox_mgr_->create_sandbox("pipeline");
    }

    DelegationResult last_result;
    last_result.task = task;

    for (size_t i = 0; i < stages.size(); ++i) {
        if (!run_pipeline_stage(parent_ctx, stages, i, task,
                                shared_sb, last_result)) {
            break;
        }
    }

    finalize_sandbox_for(shared_sb, last_result);
    return last_result;
}

/**
 * @brief Run one stage of an `execute_pipeline` loop.
 *
 * Extracted to keep `execute_pipeline` under the SLOC gate. Returns
 * false on tier-not-found or stage failure so the caller can break
 * the outer loop.
 *
 * @param parent_ctx  Parent loop context.
 * @param stages      Full stage list.
 * @param stage_idx   Index of the stage to run.
 * @param task        Original task text.
 * @param shared_sb   Shared pipeline sandbox (may be empty).
 * @param last_result In/out: previous-stage result on entry,
 *                    this stage's result on return.
 * @return true to continue to the next stage, false to break.
 * @internal
 * @version 2.1.5
 */
bool DelegationManager::run_pipeline_stage(
    LoopContext& parent_ctx,
    const std::vector<std::string>& stages,
    size_t stage_idx,
    const std::string& task,
    const std::optional<SandboxInfo>& shared_sb,
    DelegationResult& last_result) {

    const auto& tier_name = stages[stage_idx];
    std::string prior = (stage_idx == 0) ? std::string{} : last_result.summary;
    std::string stage_task =
        pipeline_context(stage_idx, stages.size(), stages, prior) + task;

    auto info = tier_res_.resolve_tier
        ? tier_res_.resolve_tier(tier_name, tier_res_.user_data)
        : ChildContextInfo{};

    if (!info.valid) {
        logger->error("Pipeline stage {}: tier '{}' not found",
                      stage_idx, tier_name);
        last_result.success = false;
        last_result.summary = "Unknown tier: " + tier_name;
        return false;
    }

    auto child_ctx = build_child_context(parent_ctx, info, stage_task);
    child_ctx.locked_tier = tier_name;

    if (shared_sb && swap_dir_fn_ != nullptr) {
        ScopedSandbox scope(swap_dir_fn_, swap_dir_data_,
                            shared_sb->path, repo_dir_);
        last_result = run_child(child_ctx, tier_name, stage_task,
                                std::nullopt);
    } else {
        last_result = run_child(child_ctx, tier_name, stage_task,
                                std::nullopt);
    }

    logger->info("Pipeline stage {} ({}): {}", stage_idx, tier_name,
                 last_result.success ? "complete" : "failed");
    return last_result.success;
}

// ── Child context ────────────────────────────────────────

/**
 * @brief Build a fresh LoopContext for the child delegation.
 * @param parent_ctx Parent context.
 * @param info Resolved tier info.
 * @param task Task description.
 * @return Fresh child context.
 * @internal
 * @version 2.0.6-rc16
 */
LoopContext DelegationManager::build_child_context(
    const LoopContext& parent_ctx,
    const ChildContextInfo& info,
    const std::string& task) {

    LoopContext child;
    child.delegation_depth = parent_ctx.delegation_depth + 1;
    // P1-9: propagate ancestor chain + append parent tier so the
    // child can reject cycles (A→B→A) before executing.
    child.delegation_ancestor_tiers = parent_ctx.delegation_ancestor_tiers;
    if (!parent_ctx.locked_tier.empty()) {
        child.delegation_ancestor_tiers.push_back(parent_ctx.locked_tier);
    }
    child.locked_tier = info.system_prompt.empty()
        ? parent_ctx.locked_tier : "";
    child.all_tools = info.tools;
    child.active_phase = "default";

    // System prompt as first message
    Message sys;
    sys.role = "system";
    sys.content = info.system_prompt;
    child.messages.push_back(std::move(sys));

    // Task as user message (with completion instructions)
    std::string user_content = task;
    if (!info.completion_instructions.empty()) {
        user_content += "\n\n" + info.completion_instructions;
    }
    Message user;
    user.role = "user";
    user.content = std::move(user_content);
    child.messages.push_back(std::move(user));

    return child;
}

/**
 * @brief Extract the delegation summary from child context.
 * @param child_ctx Completed child context.
 * @return Summary text.
 * @internal
 * @version 1.8.6
 */
std::string DelegationManager::extract_summary(
    const LoopContext& child_ctx) const {
    // Prefer explicit completion summary
    auto it = child_ctx.metadata.find("explicit_completion_summary");
    if (it != child_ctx.metadata.end() && !it->second.empty()) {
        return it->second;
    }

    // Fall back to last assistant message
    for (auto rit = child_ctx.messages.rbegin();
         rit != child_ctx.messages.rend(); ++rit) {
        if (rit->role == "assistant" && !rit->content.empty()) {
            return rit->content;
        }
    }

    return "(No response from delegate)";
}

/**
 * @brief Run child loop with todo save/restore.
 * @param child_ctx Child context to execute.
 * @param target_tier Tier name.
 * @param task Task description.
 * @param max_turns Optional turn limit.
 * @return DelegationResult.
 * @internal
 * @version 1.8.6
 */
/**
 * @brief Create delegation storage record if storage is available.
 * @param child_ctx Child context (conversation_id set on success).
 * @param target_tier Target tier.
 * @param task Task description.
 * @param max_turns Turn limit.
 * @return Delegation ID (empty if no storage).
 * @internal
 * @version 1.8.8
 */
std::string DelegationManager::create_storage_record(
    LoopContext& child_ctx, const std::string& target_tier,
    const std::string& task, std::optional<int> max_turns) {
    if (!storage_ || !storage_->create_delegation) {
        return "";
    }
    std::string del_id, child_conv_id;
    auto src_tier = child_ctx.locked_tier.empty()
        ? "root" : child_ctx.locked_tier.c_str();
    storage_->create_delegation(
        child_ctx.parent_conversation_id.c_str(),
        src_tier, target_tier.c_str(), task.c_str(),
        max_turns.value_or(0), del_id, child_conv_id,
        storage_->user_data);
    child_ctx.conversation_id = child_conv_id;
    return del_id;
}

/**
 * @brief Complete delegation storage record.
 * @param delegation_id Delegation ID.
 * @param result Delegation result.
 * @internal
 * @version 1.8.8
 */
void DelegationManager::complete_storage_record(
    const std::string& delegation_id,
    const DelegationResult& result) {
    if (!storage_ || !storage_->complete_delegation
        || delegation_id.empty()) {
        return;
    }
    const char* status = result.success ? "completed" : "failed";
    storage_->complete_delegation(
        delegation_id.c_str(), status,
        result.summary.c_str(), storage_->user_data);
}

/**
 * @brief Run child loop with todo save/restore and storage records.
 * @param child_ctx Child context to execute.
 * @param target_tier Tier name.
 * @param task Task description.
 * @param max_turns Optional turn limit.
 * @return DelegationResult.
 * @internal
 * @version 2.0.6-rc18
 */
DelegationResult DelegationManager::run_child(
    LoopContext& child_ctx,
    const std::string& target_tier,
    const std::string& task,
    std::optional<int> max_turns) {

    // Save parent todo list
    std::string saved_todo;
    if (todo_callbacks_.save != nullptr) {
        saved_todo = todo_callbacks_.save(todo_callbacks_.user_data);
    }
    if (todo_callbacks_.install_fresh != nullptr) {
        todo_callbacks_.install_fresh(todo_callbacks_.user_data);
    }

    auto delegation_id = create_storage_record(
        child_ctx, target_tier, task, max_turns);

    logger->info("Running child loop: tier={} depth={} msgs={} "
                 "system_hash={:016x}",
                 target_tier, child_ctx.delegation_depth,
                 child_ctx.messages.size(),
                 std::hash<std::string>{}(
                     child_ctx.messages.empty()
                         ? std::string{}
                         : child_ctx.messages[0].content));

    if (run_child_fn_ != nullptr) {
        run_child_fn_(child_ctx, run_child_data_);
    }

    // Restore parent todo list
    if (todo_callbacks_.restore != nullptr && !saved_todo.empty()) {
        todo_callbacks_.restore(saved_todo, todo_callbacks_.user_data);
    }

    auto result = build_child_result(
        target_tier, task, child_ctx);
    complete_storage_record(delegation_id, result);
    log_child_result(result);
    return result;
}

/**
 * @brief Build DelegationResult from a terminated child loop context.
 *
 * Reads terminal_reason metadata (E7) and sets success=true only when
 * the child reached COMPLETE via a real entropic.complete — never for
 * a synthetic budget_exhausted termination. Keeps run_child under the
 * 50-SLOC complexity gate. (2.0.6-rc18)
 *
 * Issue #10 (v2.1.4): also hoists coverage_gap / gap_description /
 * suggested_files from child metadata onto the typed
 * DelegationResult so the parent's finalize_delegation_result can
 * branch on it.
 *
 * @param target_tier Tier that executed.
 * @param task Task text.
 * @param child_ctx Terminated child context (messages moved out).
 * @return Fully-populated DelegationResult.
 * @internal
 * @version 2.1.4
 */
DelegationResult DelegationManager::build_child_result(
    const std::string& target_tier,
    const std::string& task,
    LoopContext& child_ctx) {
    DelegationResult result;
    result.target_tier = target_tier;
    result.task = task;
    auto tr = child_ctx.metadata.find("terminal_reason");
    if (tr != child_ctx.metadata.end()) {
        result.terminal_reason = tr->second;
    }
    result.success = (child_ctx.state == AgentState::COMPLETE
                      && result.terminal_reason.empty());
    result.turns_used = child_ctx.metrics.iterations;
    result.summary = extract_summary(child_ctx);
    // Issue #10 (v2.1.4): hoist coverage_gap signal from child
    // metadata onto DelegationResult so the parent's
    // finalize_delegation_result can branch on it without re-parsing
    // ctx.metadata. dir_complete writes these keys when the child
    // calls entropic.complete with coverage_gap=true.
    auto cg = child_ctx.metadata.find("coverage_gap");
    if (cg != child_ctx.metadata.end() && cg->second == "true") {
        result.coverage_gap = true;
        auto gd = child_ctx.metadata.find("gap_description");
        if (gd != child_ctx.metadata.end()) {
            result.gap_description = gd->second;
        }
        auto sf = child_ctx.metadata.find("suggested_files_json");
        if (sf != child_ctx.metadata.end()) {
            auto parsed = nlohmann::json::parse(
                sf->second, nullptr, false);
            if (parsed.is_array()) {
                result.suggested_files =
                    parsed.get<std::vector<std::string>>();
            }
        }
    }
    result.child_messages = std::move(child_ctx.messages);
    return result;
}

/**
 * @brief Emit the "Child loop done:" log with branch on terminal_reason.
 * @param result Delegation result with terminal_reason populated.
 * @internal
 * @version 2.0.6-rc18
 */
void DelegationManager::log_child_result(
    const DelegationResult& result) {
    if (result.terminal_reason.empty()) {
        logger->info("Child loop done: tier={} success={} turns={}",
                     result.target_tier, result.success,
                     result.turns_used);
    } else {
        logger->warn("Child loop done: tier={} success=false "
                     "turns={} reason={}",
                     result.target_tier, result.turns_used,
                     result.terminal_reason);
    }
}

/**
 * @brief Finalize sandbox based on delegation result.
 *
 * On success, generates a unified-diff patch and routes it to the
 * registered complete callback. ACCEPT means the consumer applied the
 * patch (engine discards the sandbox); REJECT or null-callback means
 * the engine persists the patch to `<session>/pending/<id>.patch` for
 * later inspection. The sandbox directory is removed unconditionally
 * — the engine never retains writable state (gh#29).
 *
 * @param sb_info Sandbox to finalize.
 * @param result Delegation result.
 * @internal
 * @version 2.1.5-cb
 */
void DelegationManager::finalize_sandbox_for(
    const std::optional<SandboxInfo>& sb_info,
    const DelegationResult& result) {
    if (!sb_info || !sandbox_mgr_) {
        return;
    }

    if (result.success) {
        auto patch_result = sandbox_mgr_->finalize_sandbox(*sb_info);
        if (patch_result) {
            deliver_sandbox_result(*sb_info, *patch_result, result);
        } else {
            logger->error("Delegation {}: finalize_sandbox failed; "
                          "no patch delivered", sb_info->delegation_id);
        }
    } else {
        logger->info("Delegation {} failed: discarding sandbox without "
                     "generating patch", sb_info->delegation_id);
    }
    sandbox_mgr_->discard_sandbox(*sb_info);
}

} // namespace entropic
