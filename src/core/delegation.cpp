// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file delegation.cpp
 * @brief DelegationManager implementation.
 *
 * Ports Python's DelegationManager. Child loop creation, worktree
 * lifecycle, pipeline execution, todo list save/restore.
 *
 * @version 1.8.6
 */

#include <entropic/core/delegation.h>
#include <entropic/core/engine_types.h>
#include <entropic/types/logging.h>

#include <algorithm>

static auto logger = entropic::log::get("core.delegation");

namespace entropic {

// ── Construction ─────────────────────────────────────────

/**
 * @brief Construct with engine loop callback and tier resolution.
 * @param run_child Callback to run child engine loop.
 * @param run_child_data Opaque pointer for run_child.
 * @param tier_resolution Tier resolution interface.
 * @param repo_dir Optional repo root for worktree isolation.
 * @internal
 * @version 1.8.6
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
        worktree_mgr_.emplace(repo_dir_);
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
 * @brief Set directory swap callback for ScopedWorktree.
 * @param swap_fn Directory swap callback.
 * @param user_data Opaque pointer for swap_fn.
 * @internal
 * @version 1.8.6
 */
void DelegationManager::set_dir_swap(
    ScopedWorktree::SwapDirFn swap_fn, void* user_data) {
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

// ── Single delegation ────────────────────────────────────

/**
 * @brief Run a child inference loop for the target tier.
 * @param parent_ctx Parent loop context.
 * @param target_tier Tier name to delegate to.
 * @param task Task description for the child.
 * @param max_turns Optional iteration limit.
 * @return DelegationResult.
 * @internal
 * @version 2.0.2
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

    // Create per-delegation worktree
    std::optional<WorktreeInfo> wt_info;
    if (worktree_mgr_) {
        worktree_mgr_->ensure_develop();
        wt_info = worktree_mgr_->create_worktree(
            "d" + std::to_string(parent_ctx.delegation_depth + 1),
            target_tier);
    }

    DelegationResult result;

    // Scoped worktree swap (RAII restores on exit)
    if (wt_info && swap_dir_fn_ != nullptr) {
        ScopedWorktree scope(swap_dir_fn_, swap_dir_data_,
                             wt_info->path, repo_dir_);
        result = run_child(child_ctx, target_tier, task, max_turns);
    } else {
        result = run_child(child_ctx, target_tier, task, max_turns);
    }

    finalize_worktree(wt_info, result);
    return result;
}

// ── Pipeline ─────────────────────────────────────────────

/**
 * @brief Build pipeline context prefix for a stage.
 * @param stage_idx Zero-based stage index.
 * @param total Total number of stages.
 * @param stages Stage tier names.
 * @return Context string prepended to the task.
 * @version 1.8.6
 * @internal
 */
static std::string pipeline_context(
    size_t stage_idx, size_t total,
    const std::vector<std::string>& stages) {
    std::string ctx = "[PIPELINE CONTEXT]\n";
    ctx += "Stage " + std::to_string(stage_idx + 1) +
           " of " + std::to_string(total) + "\n";
    ctx += "Role: " + stages[stage_idx] + "\n";
    ctx += "Stay within your role. Do not perform work "
           "outside your stage's responsibility.\n\n";
    return ctx;
}

/**
 * @brief Run a multi-stage delegation pipeline sequentially.
 * @param parent_ctx Parent loop context.
 * @param stages Ordered list of tier names.
 * @param task Task description.
 * @return DelegationResult from the final stage.
 * @internal
 * @version 2.0.2
 */
DelegationResult DelegationManager::execute_pipeline(
    LoopContext& parent_ctx,
    const std::vector<std::string>& stages,
    const std::string& task) {

    logger->info("Pipeline: {} stages, task='{}'", stages.size(), task);

    // Create shared worktree for entire pipeline
    std::optional<WorktreeInfo> shared_wt;
    if (worktree_mgr_) {
        worktree_mgr_->ensure_develop();
        shared_wt = worktree_mgr_->create_worktree("pipeline", "shared");
    }

    DelegationResult last_result;
    last_result.task = task;

    for (size_t i = 0; i < stages.size(); ++i) {
        std::string stage_task =
            pipeline_context(i, stages.size(), stages) + task;

        auto info = tier_res_.resolve_tier
            ? tier_res_.resolve_tier(stages[i], tier_res_.user_data)
            : ChildContextInfo{};

        if (!info.valid) {
            logger->error("Pipeline stage {}: tier '{}' not found",
                          i, stages[i]);
            last_result.success = false;
            last_result.summary = "Unknown tier: " + stages[i];
            break;
        }

        auto child_ctx = build_child_context(parent_ctx, info, stage_task);
        child_ctx.locked_tier = stages[i];

        if (shared_wt && swap_dir_fn_ != nullptr) {
            ScopedWorktree scope(swap_dir_fn_, swap_dir_data_,
                                 shared_wt->path, repo_dir_);
            last_result = run_child(
                child_ctx, stages[i], stage_task, std::nullopt);
        } else {
            last_result = run_child(
                child_ctx, stages[i], stage_task, std::nullopt);
        }

        logger->info("Pipeline stage {} ({}): {}",
                      i, stages[i],
                      last_result.success ? "complete" : "failed");

        if (!last_result.success) {
            break;
        }
    }

    // Pipeline always merges (even on partial failure)
    if (shared_wt && worktree_mgr_) {
        worktree_mgr_->merge_worktree(*shared_wt);
    }

    return last_result;
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
 * @version 2.0.6
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

    DelegationResult result;
    result.target_tier = target_tier;
    result.task = task;
    result.success = (child_ctx.state == AgentState::COMPLETE);
    result.turns_used = child_ctx.metrics.iterations;
    result.summary = extract_summary(child_ctx);
    result.child_messages = std::move(child_ctx.messages);

    complete_storage_record(delegation_id, result);

    logger->info("Child loop done: tier={} success={} turns={}",
                 target_tier, result.success, result.turns_used);
    return result;
}

/**
 * @brief Finalize worktree based on delegation result.
 * @param wt_info Worktree to finalize.
 * @param result Delegation result.
 * @internal
 * @version 1.8.6
 */
void DelegationManager::finalize_worktree(
    const std::optional<WorktreeInfo>& wt_info,
    const DelegationResult& result) {
    if (!wt_info || !worktree_mgr_) {
        return;
    }

    if (result.success) {
        worktree_mgr_->merge_worktree(*wt_info);
    } else {
        worktree_mgr_->discard_worktree(*wt_info);
    }
}

} // namespace entropic
