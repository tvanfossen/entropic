// SPDX-License-Identifier: Apache-2.0
/**
 * @file entropic_server.cpp
 * @brief EntropicServer implementation — engine-level directive tools.
 *
 * All tools emit directives. Directive parameters are carried through the
 * ServerResponse result JSON; the engine's MCP layer reconstructs typed
 * directives from the serialized envelope. Base Directive structs in the
 * directives vector carry only the type tag for serialize_response().
 *
 * v1.9.12: Added DiagnoseTool and InspectTool for engine introspection.
 * v2.0.6-rc16: Added ContextInspectTool for context window inspection (P2-16).
 *
 * @version 2.0.6-rc16
 */

#include <entropic/mcp/servers/entropic_server.h>
#include <entropic/mcp/tool_base.h>
#include <entropic/mcp/server_base.h>
#include <entropic/mcp/utf8_sanitize.h>
#include <entropic/core/directives.h>
#include <entropic/types/logging.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

static auto logger = entropic::log::get("mcp.entropic");

namespace entropic {

// ── TodoItem ────────────────────────────────────────────────────

/**
 * @brief Single todo entry.
 * @dg_internal
 * @version 1.8.5
 */
struct TodoItem {
    std::string content; ///< Item text
    std::string status;  ///< "pending", "in_progress", "done"
};

// ── TodoTool ────────────────────────────────────────────────────

/**
 * @brief Tool for managing a persistent todo list.
 *
 * gh#158 (v2.13.0): the list lives on the TOOL, so it is one list per
 * EntropicServer — shared by every session on that server set (the
 * handle's default set serves all unbound sessions). Locked; whether it
 * should be per session is an open design question (decision #66).
 *
 * @dg_internal
 * @version 2.13.0
 */
class TodoTool : public ToolBase {
public:
    /**
     * @brief Construct from tool definition.
     * @param def Tool definition loaded from entropic/todo.json.
     * @dg_internal
     * @version 1.8.5
     */
    explicit TodoTool(ToolDefinition def)
        : ToolBase(std::move(def)) {}

    /**
     * @brief Execute todo action (add/update/remove).
     * @param args_json JSON with "action", "content", "index", "status".
     * @return ServerResponse with todo state and directives.
     * @dg_internal
     * @version 1.8.5
     */
    ServerResponse execute(const std::string& args_json) override;

    /**
     * @brief Anchor key for todo state replacement.
     * @param args_json Arguments (unused).
     * @return "todo_state" anchor key.
     * @dg_internal
     * @version 1.8.5
     */
    std::string anchor_key(
        const std::string& args_json) const override;

private:
    /**
     * @brief Apply an add/update/remove action to the todo list.
     *
     * Extracted from execute() to keep it knots-clean.
     *
     * @param action One of "add" / "update" / "remove".
     * @param args Parsed tool arguments.
     * @dg_internal
     * @version 2.3.7
     */
    void apply_todo_action(const std::string& action,
                           const nlohmann::json& args);

    /// @brief gh#158: guards items_. The default server set has ONE
    /// EntropicServer, so concurrent sessions append from several run
    /// threads; unguarded, a racing push_back corrupted the vector.
    std::mutex mutex_;
    std::vector<TodoItem> items_; ///< Todo list state

    /**
     * @brief Format the todo list as human-readable text.
     * @return Formatted todo list string.
     * @dg_internal
     * @version 1.8.5
     */
    std::string format_list() const;
};

/**
 * @brief Anchor key for context replacement.
 * @param args_json Arguments (unused).
 * @return "todo_state".
 * @dg_internal
 * @version 1.8.5
 */
std::string TodoTool::anchor_key(
    const std::string& /*args_json*/) const {
    return "todo_state";
}

/**
 * @brief Format todo list as numbered text.
 * @return Formatted string.
 * @dg_internal
 * @version 1.8.5
 */
std::string TodoTool::format_list() const {
    if (items_.empty()) {
        return "(empty)";
    }
    std::string out;
    for (size_t i = 0; i < items_.size(); ++i) {
        out += std::to_string(i) + ". [" +
               items_[i].status + "] " +
               items_[i].content + "\n";
    }
    return out;
}

/**
 * @brief Dispatch todo action and build response.
 * @param args_json JSON with action, content, index, status.
 * @return ServerResponse with formatted list and directives.
 * @dg_internal
 * @version 1.8.5
 */
/**
 * @brief Apply an add/update/remove action to the todo list.
 * @param action One of "add" / "update" / "remove".
 * @param args Parsed tool arguments.
 * @dg_internal
 * @version 2.3.7
 */
void TodoTool::apply_todo_action(const std::string& action,
                                 const nlohmann::json& args) {
    if (action == "add") {
        std::string content = args.at("content").get<std::string>();
        items_.push_back({content, "pending"});
        logger->info("[todo] add: {}", content);
    } else if (action == "update") {
        auto idx = args.at("index").get<size_t>();
        if (idx < items_.size()) {
            items_[idx].status = args.value("status", items_[idx].status);
            items_[idx].content = args.value("content", items_[idx].content);
            logger->info("[todo] update #{}: {}", idx, items_[idx].status);
        }
    } else if (action == "remove") {
        auto idx = args.at("index").get<size_t>();
        if (idx < items_.size()) {
            logger->info("[todo] remove #{}", idx);
            items_.erase(items_.begin() + static_cast<ptrdiff_t>(idx));
        }
    }
}

/**
 * @brief Execute the todo tool (add/update/remove) and emit directives.
 *
 * gh#158 (v2.13.0): the action and the render run under ONE hold of
 * `mutex_`, so the list a call returns is the list its own action
 * produced. The TSan run before this aborted with an impossible
 * allocation size inside `items_.push_back` — heap corruption, not a
 * stale read.
 *
 * @param args_json JSON with "action" plus the action's own fields.
 * @return A ServerResponse whose result is the re-rendered todo list
 *         and whose directives are context_anchor + notify_presenter.
 * @req REQ-MCP-024
 * @req REQ-LOOP-009
 * @version 2.13.0
 */
ServerResponse TodoTool::execute(const std::string& args_json) {
    auto args = nlohmann::json::parse(args_json);
    std::string action = args.at("action").get<std::string>();

    std::string rendered;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        apply_todo_action(action, args);
        rendered = format_list();
    }

    nlohmann::json result;
    result["todo_state"] = rendered;
    result["action"] = action;

    Directive anchor_d;
    anchor_d.type = ENTROPIC_DIRECTIVE_CONTEXT_ANCHOR;
    Directive notify_d;
    notify_d.type = ENTROPIC_DIRECTIVE_NOTIFY_PRESENTER;
    return {result.dump(), {anchor_d, notify_d}};
}

// ── DelegateTool ────────────────────────────────────────────────

/**
 * @brief Tool for delegating tasks to child inference loops.
 * @dg_internal
 * @version 1.8.5
 */
class DelegateTool : public ToolBase {
public:
    /**
     * @brief Construct and patch input schema with tier names.
     * @param def Tool definition loaded from entropic/delegate.json.
     * @param tier_names Available tier names for enum patching.
     * @dg_internal
     * @version 1.8.5
     */
    DelegateTool(ToolDefinition def,
                 const std::vector<std::string>& tier_names,
                 std::vector<std::string> require_context_tiers = {});

    /**
     * @brief Execute delegation.
     * @param args_json JSON with "target", "task", "max_turns", "context".
     * @return ServerResponse with delegate + stop directives, or a typed
     *         error with NO directives when the target tier declares
     *         `requires_context` and none was supplied (gh#162).
     * @dg_internal
     * @version 2.13.0
     */
    ServerResponse execute(const std::string& args_json) override;

private:
    /// @brief gh#162: tiers that refuse a delegation carrying no context.
    std::vector<std::string> require_context_tiers_;
};

/**
 * @brief Construct and patch enum in input schema.
 *
 * The target enum is patched at construction from the tier names the
 * server was given — and those exclude source tiers, so a tier cannot
 * delegate to itself.
 *
 * @param def Tool definition.
 * @param tier_names Tier names for target enum.
 * @param require_context_tiers Tiers that refuse a contextless
 *        delegation (gh#162).
 * @req REQ-MCP-024
 * @req REQ-MCP-013
 * @req REQ-DELEG-006
 * @version 2.13.0
 */
DelegateTool::DelegateTool(
    ToolDefinition def,
    const std::vector<std::string>& tier_names,
    std::vector<std::string> require_context_tiers)
    : ToolBase(std::move(def)),
      require_context_tiers_(std::move(require_context_tiers)) {

    auto schema = nlohmann::json::parse(definition_.input_schema);
    schema["properties"]["target"]["enum"] = tier_names;
    definition_.input_schema = schema.dump();

    logger->info("[delegate] patched enum with {} tiers ({} require context)",
                 tier_names.size(), require_context_tiers_.size());
}

/**
 * @brief Copy a `context` argument into the result, dropping junk (gh#162).
 *
 * Entries without a `path` are dropped rather than forwarded: a context
 * reference with nothing to open is noise in the child's opening message,
 * and small models treat noise as instruction.
 *
 * @param args Parsed tool arguments.
 * @param[out] result Result JSON to populate with a "context" array.
 * @return Number of usable references carried through.
 * @utility
 * @version 2.13.0
 */
static size_t carry_context(const nlohmann::json& args,
                            nlohmann::json& result) {
    auto out = nlohmann::json::array();
    if (args.contains("context") && args["context"].is_array()) {
        for (const auto& ref : args["context"]) {
            if (!ref.is_object()) { continue; }
            auto path = ref.value("path", std::string{});
            if (path.empty()) { continue; }
            nlohmann::json entry;
            entry["path"] = path;
            entry["lines"] = ref.value("lines", std::string{});
            entry["note"] = ref.value("note", std::string{});
            out.push_back(std::move(entry));
        }
    }
    size_t n = out.size();
    result["context"] = std::move(out);
    return n;
}

/**
 * @brief Parse delegation args and emit directives.
 * @param args_json JSON with "target", "task", optional "max_turns"
 *                  (defaulting to -1 for "engine decides") and optional
 *                  "context" file references (gh#162).
 * @return A ServerResponse whose result echoes the delegation and whose
 *         directives are delegate + stop_processing — the pair that
 *         hands the turn to the child loop; or, when the target tier
 *         declares `requires_context` and none was supplied, an error
 *         string with NO directives (gh#162).
 * @req REQ-MCP-024
 * @req REQ-DELEG-006
 * @version 2.13.0
 */
ServerResponse DelegateTool::execute(const std::string& args_json) {
    auto args = nlohmann::json::parse(args_json);
    std::string target = args.at("target").get<std::string>();
    std::string task = args.at("task").get<std::string>();
    int max_turns = args.value("max_turns", -1);

    nlohmann::json result;
    result["action"] = "delegate";
    result["target"] = target;
    result["task"] = task;
    result["max_turns"] = max_turns;
    auto refs = carry_context(args, result);

    // gh#162: a tier that declares requires_context cannot succeed on a
    // contextless task, so refuse HERE — a tool error the lead can act on
    // in the same turn, rather than a burned child loop whose transcript
    // nobody reads.
    if (refs == 0
        && std::find(require_context_tiers_.begin(),
                     require_context_tiers_.end(), target)
           != require_context_tiers_.end()) {
        logger->warn("[delegate] refused: tier '{}' requires context", target);
        return {"Error: tier \"" + target + "\" requires context. Re-issue "
                "entropic.delegate with a `context` array naming at least one "
                "file path (optionally lines/note) the delegate should read.",
                {}};
    }

    logger->info("[delegate] target='{}' task='{}' max_turns={} context={}",
                 target, task, max_turns, refs);

    Directive delegate_d;
    delegate_d.type = ENTROPIC_DIRECTIVE_DELEGATE;

    Directive stop_d;
    stop_d.type = ENTROPIC_DIRECTIVE_STOP_PROCESSING;

    return {result.dump(), {delegate_d, stop_d}};
}

// ── PipelineTool ────────────────────────────────────────────────

/**
 * @brief Tool for multi-stage delegation pipelines.
 * @dg_internal
 * @version 1.8.5
 */
class PipelineTool : public ToolBase {
public:
    /**
     * @brief Construct and patch input schema with tier names.
     * @param def Tool definition loaded from entropic/pipeline.json.
     * @param tier_names Available tier names for enum patching.
     * @dg_internal
     * @version 1.8.5
     */
    PipelineTool(ToolDefinition def,
                 const std::vector<std::string>& tier_names,
                 std::vector<std::string> require_context_tiers = {});

    /**
     * @brief Execute pipeline setup.
     * @param args_json JSON with "stages" array and "task".
     * @return ServerResponse with pipeline + stop directives.
     * @dg_internal
     * @version 1.8.5
     */
    ServerResponse execute(const std::string& args_json) override;

private:
    std::vector<std::string> tier_names_;  ///< Known tiers for runtime validation.
    /// @brief gh#162: stages that refuse a contextless pipeline.
    std::vector<std::string> require_context_tiers_;
};

/**
 * @brief Construct and patch stage enum in input schema.
 *
 * The tier list is kept as a member as well as patched into the schema,
 * so stages are re-validated at execute time rather than trusting the
 * model to have honoured the enum.
 *
 * @param def Tool definition.
 * @param tier_names Tier names for stages enum.
 * @req REQ-MCP-024
 * @req REQ-MCP-013
 * @version 2.10.0
 */
PipelineTool::PipelineTool(
    ToolDefinition def,
    const std::vector<std::string>& tier_names,
    std::vector<std::string> require_context_tiers)
    : ToolBase(std::move(def)), tier_names_(tier_names),
      require_context_tiers_(std::move(require_context_tiers)) {

    auto schema = nlohmann::json::parse(definition_.input_schema);
    schema["properties"]["stages"]["items"]["enum"] = tier_names;
    definition_.input_schema = schema.dump();

    logger->info("[pipeline] patched enum with {} tiers",
                 tier_names.size());
}

/**
 * @brief Parse pipeline args, validate stages, emit directives.
 *
 * gh#129 (v2.10.0): each stage is validated against tier_names_ before
 * any directive is emitted. Unknown tier names are rejected with a plain
 * error string naming the offending stage and the valid options.
 *
 * @param args_json JSON with "stages", "task" and optional "context"
 *                  file references (gh#162).
 * @return On success, a ServerResponse echoing the stages with
 *         pipeline + stop_processing directives. On fewer than two
 *         stages, a stage naming a tier not in tier_names, or a stage
 *         that declares `requires_context` with none supplied (gh#162),
 *         an error string quoting the offending stage — with NO
 *         directives, so nothing is dispatched.
 * @req REQ-MCP-024
 * @req REQ-DELEG-006
 * @version 2.13.0
 */
/**
 * @brief Why this pipeline cannot run, or "" when it can (gh#129, gh#162).
 *
 * Extracted from `PipelineTool::execute` to keep it under the knots
 * ABC + returns gates when the context check joined the stage check.
 *
 * @param stages Requested stage tiers.
 * @param tier_names Configured tiers.
 * @param require_context_tiers Tiers that refuse a contextless run.
 * @param refs Number of context references supplied.
 * @return Error text for the model, or "" when the pipeline is valid.
 * @utility
 * @req REQ-MCP-024
 * @req REQ-DELEG-006
 * @version 2.13.0
 */
static std::string pipeline_rejection(
    const std::vector<std::string>& stages,
    const std::vector<std::string>& tier_names,
    const std::vector<std::string>& require_context_tiers,
    size_t refs) {
    std::string err;
    if (stages.size() < 2) {
        logger->warn("[pipeline] rejected: fewer than 2 stages");
        err = "Error: pipeline requires at least 2 stages";
    }
    for (const auto& s : stages) {
        if (!err.empty()) { break; }
        if (std::find(tier_names.begin(), tier_names.end(), s)
            == tier_names.end()) {
            logger->warn("[pipeline] invalid stage: '{}'", s);
            err = "Error: unknown stage \"" + s + "\". Valid stages: "
                  + nlohmann::json(tier_names).dump();
        } else if (refs == 0
                   && std::find(require_context_tiers.begin(),
                                require_context_tiers.end(), s)
                      != require_context_tiers.end()) {
            // gh#162: one context list seeds every stage, so a stage that
            // requires context refuses the whole pipeline — before any
            // stage runs, not after the first has burned its turns.
            logger->warn("[pipeline] refused: stage '{}' requires context", s);
            err = "Error: stage \"" + s + "\" requires context. Re-issue "
                  "entropic.pipeline with a `context` array naming at least "
                  "one file path the stages should read.";
        }
    }
    return err;
}

/**
 * @brief Parse pipeline args, validate stages, emit directives.
 *
 * gh#129 (v2.10.0): every stage is validated against tier_names_ before
 * any directive is emitted. gh#162 (v2.13.0): the context list is carried
 * through and a stage declaring `requires_context` refuses the run.
 *
 * @param args_json JSON with "stages", "task" and optional "context".
 * @return On success, a ServerResponse echoing the stages with
 *         pipeline + stop_processing directives; otherwise an error
 *         string with NO directives, so nothing is dispatched.
 * @req REQ-MCP-024
 * @req REQ-DELEG-006
 * @version 2.13.0
 */
ServerResponse PipelineTool::execute(const std::string& args_json) {
    auto args = nlohmann::json::parse(args_json);
    auto stages = args.at("stages").get<std::vector<std::string>>();
    std::string task = args.at("task").get<std::string>();

    nlohmann::json result;
    result["action"] = "pipeline";
    result["stages"] = stages;
    result["task"] = task;
    auto refs = carry_context(args, result);  // gh#162

    auto rejection = pipeline_rejection(
        stages, tier_names_, require_context_tiers_, refs);
    if (!rejection.empty()) {
        return {rejection, {}};
    }

    logger->info("[pipeline] stages={} task='{}' context={}",
                 stages.size(), task, refs);

    Directive pipeline_d;
    pipeline_d.type = ENTROPIC_DIRECTIVE_PIPELINE;

    Directive stop_d;
    stop_d.type = ENTROPIC_DIRECTIVE_STOP_PROCESSING;

    return {result.dump(), {pipeline_d, stop_d}};
}

// ── CompleteTool ────────────────────────────────────────────────

/**
 * @brief Tool for signaling task completion.
 * @dg_internal
 * @version 1.8.5
 */
class CompleteTool : public ToolBase {
public:
    /**
     * @brief Construct from tool definition.
     * @param def Tool definition loaded from entropic/complete.json.
     * @dg_internal
     * @version 1.8.5
     */
    explicit CompleteTool(ToolDefinition def)
        : ToolBase(std::move(def)) {}

    /**
     * @brief Execute completion signal.
     * @param args_json JSON with "summary".
     * @return ServerResponse with complete + stop directives.
     * @dg_internal
     * @version 1.8.5
     */
    ServerResponse execute(const std::string& args_json) override;
};

/**
 * @brief Parse summary and emit completion directives.
 *
 * Issue #10 (v2.1.4): also forwards `coverage_gap` and
 * `suggested_files` into the result JSON so the engine's directive
 * builder can populate the typed CompleteDirective fields. The
 * engine then suppresses auto-relay when coverage_gap is set,
 * injecting a `[COVERAGE GAP]` user message into the lead context
 * instead.
 *
 * @param args_json JSON with "summary" and optional "coverage_gap" /
 *                  "gap_description" / "suggested_files".
 * @return On success, a ServerResponse carrying the sanitized summary
 *         with complete + stop_processing directives. When
 *         coverage_gap is true and gap_description is empty, a
 *         `missing_gap_description` error with NO directives — the
 *         validation runs before any directive is emitted.
 * @req REQ-MCP-024
 * @version 2.10.0
 */
ServerResponse CompleteTool::execute(const std::string& args_json) {
    auto args = nlohmann::json::parse(args_json);
    std::string summary = args.at("summary").get<std::string>();
    bool coverage_gap = args.value("coverage_gap", false);
    std::string gap_description =
        args.value("gap_description", std::string{});
    std::vector<std::string> suggested;
    if (args.contains("suggested_files")
        && args["suggested_files"].is_array()) {
        suggested = args["suggested_files"]
            .get<std::vector<std::string>>();
    }

    // Issue #10 (v2.1.4): coverage_gap=true REQUIRES a non-empty
    // gap_description so the lead context's [COVERAGE GAP] message
    // tells the next specialist what to fill in. An empty
    // gap_description with coverage_gap=true would be a bare
    // "I don't know" with no signal — refuse it at the tool boundary.
    if (coverage_gap && gap_description.empty()) {
        nlohmann::json err;
        err["error"] = "missing_gap_description";
        err["message"] =
            "coverage_gap=true requires a non-empty gap_description "
            "(what's missing from this answer and why).";
        return {err.dump(), {}};
    }

    logger->info("[complete] summary='{}' coverage_gap={} "
                 "gap_description_len={} suggested_files={}",
                 summary, coverage_gap,
                 gap_description.size(), suggested.size());

    nlohmann::json result;
    result["action"] = "complete";
    result["summary"] = entropic::mcp::sanitize_utf8(summary);
    result["coverage_gap"] = coverage_gap;
    result["gap_description"] = entropic::mcp::sanitize_utf8(gap_description);
    result["suggested_files"] = suggested;

    Directive complete_d;
    complete_d.type = ENTROPIC_DIRECTIVE_COMPLETE;

    Directive stop_d;
    stop_d.type = ENTROPIC_DIRECTIVE_STOP_PROCESSING;

    return {result.dump(), {complete_d, stop_d}};
}

// ── PhaseChangeTool ─────────────────────────────────────────────

/**
 * @brief Tool for switching inference phase.
 * @dg_internal
 * @version 1.8.5
 */
class PhaseChangeTool : public ToolBase {
public:
    /**
     * @brief Construct with inline tool definition.
     * @dg_internal
     * @version 1.8.5
     */
    PhaseChangeTool();

    /**
     * @brief Execute phase change.
     * @param args_json JSON with "phase".
     * @return ServerResponse with phase_change directive.
     * @dg_internal
     * @version 1.8.5
     */
    ServerResponse execute(const std::string& args_json) override;
};

/**
 * @brief Build inline definition for phase_change tool.
 * @dg_internal
 * @version 1.8.5
 */
PhaseChangeTool::PhaseChangeTool()
    : ToolBase({"phase_change",
                "Switch inference phase",
                R"({"type":"object","properties":{"phase":{"type":"string"}},"required":["phase"]})"}) {}

/**
 * @brief Parse phase and emit directive.
 * @param args_json JSON with "phase".
 * @return A ServerResponse echoing the requested phase with a single
 *         phase_change directive for the DirectiveProcessor.
 * @req REQ-MCP-024
 * @version 1.8.5
 */
ServerResponse PhaseChangeTool::execute(
    const std::string& args_json) {
    auto args = nlohmann::json::parse(args_json);
    std::string phase = args.at("phase").get<std::string>();

    logger->info("[phase_change] phase='{}'", phase);

    nlohmann::json result;
    result["action"] = "phase_change";
    result["phase"] = phase;

    Directive phase_d;
    phase_d.type = ENTROPIC_DIRECTIVE_PHASE_CHANGE;

    return {result.dump(), {phase_d}};
}

// ── PruneContextTool ────────────────────────────────────────────

/**
 * @brief Tool for pruning old messages from context.
 * @dg_internal
 * @version 1.8.5
 */
class PruneContextTool : public ToolBase {
public:
    /**
     * @brief Construct from tool definition.
     * @param def Tool definition loaded from entropic/prune_context.json.
     * @version 1.8.5
     */
    explicit PruneContextTool(ToolDefinition def)
        : ToolBase(std::move(def)) {}

    /**
     * @brief Execute context pruning.
     * @param args_json JSON with optional "keep_recent".
     * @return ServerResponse with prune directive.
     * @version 1.8.5
     */
    ServerResponse execute(const std::string& args_json) override;
};

/**
 * @brief Parse keep_recent and emit prune directive.
 * @param args_json JSON with optional "keep_recent" (defaulting to 2
 *                  when omitted).
 * @return A ServerResponse echoing the retained-message count with a
 *         single prune_messages directive.
 * @req REQ-MCP-024
 * @version 1.8.5
 */
ServerResponse PruneContextTool::execute(
    const std::string& args_json) {
    auto args = nlohmann::json::parse(args_json);

    static constexpr int default_keep = 2;
    int keep_recent = args.value("keep_recent", default_keep);

    logger->info("[prune_context] keep_recent={}", keep_recent);

    nlohmann::json result;
    result["action"] = "prune_context";
    result["keep_recent"] = keep_recent;

    Directive prune_d;
    prune_d.type = ENTROPIC_DIRECTIVE_PRUNE_MESSAGES;

    return {result.dump(), {prune_d}};
}

// ── DiagnoseTool ────────────────────────────────────────────────

/**
 * @brief Tool for full engine state snapshots.
 * @dg_internal
 * @version 1.9.12
 */
class DiagnoseTool : public ToolBase {
public:
    /**
     * @brief Construct from tool definition.
     * @param def Tool definition loaded from entropic/diagnose.json.
     * @dg_internal
     * @version 1.9.12
     */
    explicit DiagnoseTool(ToolDefinition def)
        : ToolBase(std::move(def)) {}

    /**
     * @brief Execute diagnostic snapshot.
     * @param args_json JSON with optional include_docs, history_limit.
     * @return ServerResponse with JSON snapshot (no directives).
     * @dg_internal
     * @version 1.9.12
     */
    ServerResponse execute(const std::string& args_json) override;

    /**
     * @brief Read-only tool requires only READ access.
     * @return MCPAccessLevel::READ, relaxing ToolBase's WRITE default
     *         because this tool only reads engine state.
     * @req REQ-MCP-011
     * @version 1.9.12
     */
    MCPAccessLevel required_access_level() const override {
        return MCPAccessLevel::READ;
    }

    /** @brief Set state provider pointer.
     * @param p Provider pointer (must outlive tool).
     * @utility
     * @version 1.9.12
     */
    void set_provider(const entropic_state_provider_t* p) {
        provider_ = p;
    }

private:
    const entropic_state_provider_t* provider_ = nullptr;
};

/**
 * @brief Call a state provider callback and wrap result.
 *
 * Every introspection tool reads engine state through the
 * entropic_state_provider_t callback struct, and each individual
 * callback is null-checked here rather than dereferenced.
 *
 * @param fn Callback returning malloc'd string (or nullptr).
 * @param ud User data for callback.
 * @return The callback's string, copied and its buffer freed; "{}" when
 *         the callback is absent or returned null.
 * @req REQ-MCP-024
 * @version 1.9.12
 */
static std::string call_provider(
    char* (*fn)(void*), void* ud) {
    if (fn == nullptr) {
        return "{}";
    }
    char* raw = fn(ud);
    if (raw == nullptr) {
        return "{}";
    }
    std::string result(raw);
    free(raw);
    return result;
}

/**
 * @brief Call history callback with max_entries param.
 * @param fn History callback.
 * @param max_entries Max entries to return.
 * @param ud User data.
 * @return JSON string.
 * @dg_internal
 * @version 1.9.12
 */
static std::string call_history_provider(
    char* (*fn)(int, void*), int max_entries, void* ud) {
    if (fn == nullptr) {
        return "[]";
    }
    char* raw = fn(max_entries, ud);
    if (raw == nullptr) {
        return "[]";
    }
    std::string result(raw);
    free(raw);
    return result;
}

/**
 * @brief Call docs callback with section param.
 * @param fn Docs callback.
 * @param section Section name (nullptr for full doc).
 * @param ud User data.
 * @return String content.
 * @dg_internal
 * @version 1.9.12
 */
static std::string call_docs_provider(
    char* (*fn)(const char*, void*),
    const char* section, void* ud) {
    if (fn == nullptr) {
        return "";
    }
    char* raw = fn(section, ud);
    if (raw == nullptr) {
        return "";
    }
    std::string result(raw);
    free(raw);
    return result;
}

/**
 * @brief Build the diagnose snapshot JSON.
 * @param provider State provider callbacks.
 * @param include_docs Whether to include documentation.
 * @param history_limit Max history entries.
 * @return Snapshot as JSON object.
 * @dg_internal
 * @version 1.9.12
 */
static nlohmann::json build_snapshot(
    const entropic_state_provider_t& p,
    bool include_docs, int history_limit) {

    nlohmann::json snap;

    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<
        std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    snap["snapshot_timestamp_ms"] = ms;

    snap["engine"] = nlohmann::json::parse(
        call_provider(p.get_state, p.user_data));
    snap["config"] = nlohmann::json::parse(
        call_provider(p.get_config, p.user_data));
    snap["identities"] = nlohmann::json::parse(
        call_provider(p.get_identities, p.user_data));
    snap["tools"] = nlohmann::json::parse(
        call_provider(p.get_tools, p.user_data));
    snap["history"] = nlohmann::json::parse(
        call_history_provider(
            p.get_history, history_limit, p.user_data));
    snap["metrics"] = nlohmann::json::parse(
        call_provider(p.get_metrics, p.user_data));

    if (include_docs) {
        snap["docs"] = call_docs_provider(
            p.get_docs, nullptr, p.user_data);
    } else {
        snap["docs"] = nullptr;
    }
    return snap;
}

/**
 * @brief Execute diagnostic snapshot.
 *
 * Introspection, not control: the response carries data and NO
 * directives.
 *
 * @param args_json JSON with optional include_docs (default false) and
 *                  history_limit (default 20).
 * @return A ServerResponse with an empty directives array whose result
 *         is the snapshot JSON; when no state provider is configured,
 *         the "engine state provider not configured" error instead of a
 *         null callback dereference.
 * @req REQ-MCP-024
 * @version 1.9.12
 */
ServerResponse DiagnoseTool::execute(const std::string& args_json) {
    if (provider_ == nullptr) {
        logger->error("[diagnose] no state provider set");
        return {"Error: engine state provider not configured", {}};
    }

    auto args = nlohmann::json::parse(args_json);
    bool include_docs = args.value("include_docs", false);
    int history_limit = args.value("history_limit", 20);

    logger->info("[diagnose] include_docs={} history_limit={}",
                 include_docs, history_limit);

    auto snap = build_snapshot(
        *provider_, include_docs, history_limit);
    return {snap.dump(), {}};
}

// ── InspectTool ─────────────────────────────────────────────────

/**
 * @brief Tool for targeted engine state queries.
 * @dg_internal
 * @version 1.9.12
 */
class InspectTool : public ToolBase {
public:
    /**
     * @brief Construct from tool definition.
     * @param def Tool definition loaded from entropic/inspect.json.
     * @dg_internal
     * @version 1.9.12
     */
    explicit InspectTool(ToolDefinition def)
        : ToolBase(std::move(def)) {}

    /**
     * @brief Execute targeted inspection.
     * @param args_json JSON with "target" and optional "key".
     * @return ServerResponse with query result (no directives).
     * @dg_internal
     * @version 1.9.12
     */
    ServerResponse execute(const std::string& args_json) override;

    /**
     * @brief Read-only tool requires only READ access.
     * @return MCPAccessLevel::READ, relaxing ToolBase's WRITE default
     *         because this tool only reads engine state.
     * @req REQ-MCP-011
     * @version 1.9.12
     */
    MCPAccessLevel required_access_level() const override {
        return MCPAccessLevel::READ;
    }

    /** @brief Set state provider pointer.
     * @param p Provider pointer.
     * @utility
     * @version 1.9.12
     */
    void set_provider(const entropic_state_provider_t* p) {
        provider_ = p;
    }

private:
    const entropic_state_provider_t* provider_ = nullptr;
};

// ── ContextInspectTool ──────────────────────────────────────────

/**
 * @brief Tool for inspecting the current context window contents.
 *
 * Returns each message with role, content preview, and estimated
 * token count so the model can assess its own context saturation.
 *
 * @dg_internal
 * @version 2.0.6-rc16
 */
class ContextInspectTool : public ToolBase {
public:
    /**
     * @brief Construct from tool definition.
     * @param def Tool definition loaded from entropic/context_inspect.json.
     * @dg_internal
     * @version 2.0.6-rc16
     */
    explicit ContextInspectTool(ToolDefinition def)
        : ToolBase(std::move(def)) {}

    /**
     * @brief Return context window contents as a message array.
     * @param args_json JSON with optional "max_messages" (0 = all).
     * @return ServerResponse with [{role, content_preview, token_count_est}].
     * @dg_internal
     * @version 2.0.6-rc16
     */
    ServerResponse execute(const std::string& args_json) override;

    /**
     * @brief Read-only tool requires only READ access.
     * @return MCPAccessLevel::READ, relaxing ToolBase's WRITE default
     *         because this tool only reads engine state.
     * @req REQ-MCP-011
     * @version 2.0.6-rc16
     */
    MCPAccessLevel required_access_level() const override {
        return MCPAccessLevel::READ;
    }

    /** @brief Set state provider pointer.
     * @param p Provider pointer.
     * @utility
     * @version 2.0.6-rc16
     */
    void set_provider(const entropic_state_provider_t* p) {
        provider_ = p;
    }

private:
    const entropic_state_provider_t* provider_ = nullptr;
};

/**
 * @brief Collect keys from a JSON object into a vector.
 * @param j JSON object.
 * @return Vector of key strings.
 * @dg_internal
 * @version 1.9.12
 */
static std::vector<std::string> collect_object_keys(
    const nlohmann::json& j) {
    std::vector<std::string> keys;
    for (auto it = j.begin(); it != j.end(); ++it) {
        keys.push_back(it.key());
    }
    return keys;
}

/**
 * @brief Collect "name" fields from a JSON array of objects.
 * @param j JSON array.
 * @return Vector of name strings.
 * @dg_internal
 * @version 1.9.12
 */
static std::vector<std::string> collect_array_names(
    const nlohmann::json& j) {
    std::vector<std::string> names;
    for (const auto& item : j) {
        if (item.is_object() && item.contains("name")) {
            names.push_back(item["name"].get<std::string>());
        }
    }
    return names;
}

/**
 * @brief List available keys from a JSON value for error messages.
 * @param j JSON object or array with "name" fields.
 * @return Comma-separated key list.
 * @dg_internal
 * @version 1.9.12
 */
static std::string list_available_keys(const nlohmann::json& j) {
    auto keys = j.is_object() ? collect_object_keys(j)
                               : collect_array_names(j);
    std::string result;
    for (const auto& k : keys) {
        if (!result.empty()) { result += ", "; }
        result += k;
    }
    return result;
}

/**
 * @brief Filter a JSON value by key (object key or array name).
 * @param json_str Raw JSON string.
 * @param key Key to extract.
 * @param label Label for error messages.
 * @return Filtered JSON string or error.
 * @dg_internal
 * @version 1.9.12
 */
static std::string filter_json_by_key(
    const std::string& json_str,
    const std::string& key,
    const std::string& label) {
    auto j = nlohmann::json::parse(json_str);

    if (j.is_object() && j.contains(key)) {
        return j[key].dump();
    }
    if (j.is_array()) {
        for (const auto& item : j) {
            if (item.value("name", "") == key) {
                return item.dump();
            }
        }
    }
    return "Error: " + label + " '" + key
        + "' not found. Available: "
        + list_available_keys(j);
}

/**
 * @brief Inspect a filterable target (config/identity/tool).
 * @param fn Provider callback returning JSON.
 * @param ud Provider user_data.
 * @param key Filter key (empty = full).
 * @param label Error message label.
 * @return JSON string or error.
 * @dg_internal
 * @version 1.9.12
 */
static std::string inspect_filterable(
    char* (*fn)(void*), void* ud,
    const std::string& key, const std::string& label) {
    auto raw = call_provider(fn, ud);
    if (key.empty()) {
        return raw;
    }
    return filter_json_by_key(raw, key, label);
}

/**
 * @brief Dispatch inspect for simple (non-filterable) targets.
 * @param p State provider.
 * @param target Target name.
 * @param key Optional key.
 * @param result Output result string.
 * @return true if target was handled.
 * @dg_internal
 * @version 1.9.12
 */
static bool dispatch_simple_target(
    const entropic_state_provider_t& p,
    const std::string& target,
    const std::string& key,
    std::string& result) {

    if (target == "state") {
        result = call_provider(p.get_state, p.user_data);
    } else if (target == "metrics") {
        result = call_provider(p.get_metrics, p.user_data);
    } else if (target == "history") {
        int limit = key.empty() ? 10 : std::atoi(key.c_str());
        result = call_history_provider(
            p.get_history, limit, p.user_data);
    } else if (target == "docs") {
        const char* sec = key.empty() ? nullptr : key.c_str();
        result = call_docs_provider(p.get_docs, sec, p.user_data);
    } else {
        return false;
    }
    return true;
}

/**
 * @brief Try filterable targets (config/identity/tool).
 * @param p State provider.
 * @param target Target name.
 * @param key Filter key.
 * @param result Output.
 * @return true if target was handled.
 * @dg_internal
 * @version 1.9.12
 */
static bool dispatch_filterable_target(
    const entropic_state_provider_t& p,
    const std::string& target,
    const std::string& key,
    std::string& result) {
    if (target == "config") {
        result = inspect_filterable(
            p.get_config, p.user_data, key, "config section");
    } else if (target == "identity") {
        result = inspect_filterable(
            p.get_identities, p.user_data, key, "identity");
    } else if (target == "tool") {
        result = inspect_filterable(
            p.get_tools, p.user_data, key, "tool");
    } else {
        return false;
    }
    return true;
}

/**
 * @brief Dispatch an inspect query to the appropriate provider.
 * @param p State provider callbacks.
 * @param target Query target.
 * @param key Optional filter key.
 * @return Result string.
 * @dg_internal
 * @version 1.9.12
 */
static std::string dispatch_inspect(
    const entropic_state_provider_t& p,
    const std::string& target,
    const std::string& key) {

    std::string result;
    if (dispatch_filterable_target(p, target, key, result)) {
        return result;
    }
    if (dispatch_simple_target(p, target, key, result)) {
        return result;
    }
    return "Error: unknown target '" + target
        + "'. Supported: config, identity, tool, state, "
          "metrics, history, docs";
}

/**
 * @brief Execute targeted inspection query.
 * @param args_json JSON with "target" and optional "key".
 * @return ServerResponse with query result.
 * @dg_internal
 * @version 1.9.12
 */
/**
 * @brief Execute targeted inspection query.
 *
 * When called with no target (or empty target), returns the full
 * runtime state dump so the model can self-orient without knowing
 * which specific target to query.
 *
 * gh#33 bug 3 (v2.1.6): guard non-throwing parse against discarded /
 * non-object results so a malformed or no-arg call falls through to
 * the full-state dump rather than crashing the engine with
 * `nlohmann::json::type_error.306`.
 *
 * @param args_json JSON with optional "target" and "key".
 * @return A ServerResponse with an empty directives array whose result
 *         is the full state dump for an absent/empty target, the
 *         target's data otherwise, an unknown-target error listing the
 *         supported targets, or the provider-not-configured error when
 *         no state provider is wired.
 * @req REQ-MCP-024
 * @version 2.1.6
 */
ServerResponse InspectTool::execute(const std::string& args_json) {
    if (provider_ == nullptr) {
        logger->error("[inspect] no state provider set");
        return {"Error: engine state provider not configured", {}};
    }

    auto args = nlohmann::json::parse(args_json, nullptr, false);
    // gh#33 (v2.1.6): non-throwing parse returns a discarded value on
    // invalid/empty/null input; calling .value() on it throws
    // type_error.306 and crashes the engine. Coerce to an empty object
    // so a no-arg `entropic.inspect()` call falls through to the
    // full-state dump path.
    if (args.is_discarded() || !args.is_object()) {
        args = nlohmann::json::object();
    }
    std::string target = args.value("target", "");
    std::string key = args.value("key", "");

    if (target.empty()) {
        logger->info("[inspect] full state dump (no target)");
        auto state = call_provider(provider_->get_state,
                                   provider_->user_data);
        return {state, {}};
    }

    logger->info("[inspect] target='{}' key='{}'", target, key);
    auto result = dispatch_inspect(*provider_, target, key);
    return {result, {}};
}

/**
 * @brief Return context window contents as a message array.
 *
 * Delegates to the state provider's get_history callback with the
 * caller-supplied max_messages limit (0 = all messages).
 *
 * @param args_json JSON with optional "max_messages" (0 = all).
 * @return A ServerResponse with an empty directives array whose result
 *         is the JSON array of message entries; the
 *         provider-not-configured error when no state provider is
 *         wired.
 * @req REQ-MCP-024
 * @version 2.12.0
 */
ServerResponse ContextInspectTool::execute(
    const std::string& args_json) {
    if (provider_ == nullptr) {
        logger->error("[context_inspect] no state provider set");
        return {"Error: engine state provider not configured", {}};
    }

    auto args = nlohmann::json::parse(args_json, nullptr, false);
    // gh#143 (v2.12.0): the gh#33 coercion at InspectTool::execute was
    // never applied to this sibling, so an argument-free
    // `entropic.context_inspect()` threw type_error.306 here even after
    // gh#33 was closed. Note it is the `!is_object()` half that does the
    // work: a non-throwing parse of the four characters `null` returns a
    // NULL value, not a discarded one, so the is_discarded() half alone
    // would not have caught it.
    if (args.is_discarded() || !args.is_object()) {
        args = nlohmann::json::object();
    }
    int max_messages = args.value("max_messages", 0);

    logger->info("[context_inspect] max_messages={}", max_messages);
    auto result = call_history_provider(
        provider_->get_history, max_messages, provider_->user_data);
    return {result, {}};
}

// ── FollowupTool (gh#32, v2.1.6) ────────────────────────────────

/**
 * @brief Recall prior delegation results via storage-backed search.
 *
 * Backs `entropic.followup`. Read-only; no directives. Uses the state
 * provider's `search_delegations` callback to surface candidate
 * matches and returns the JSON verbatim to the model.
 *
 * @dg_internal
 * @version 2.1.6
 */
class FollowupTool : public ToolBase {
public:
    /**
     * @brief Construct from loaded tool definition.
     * @param def Tool definition (entropic/followup.json).
     * @dg_internal
     * @version 2.1.6
     */
    explicit FollowupTool(ToolDefinition def)
        : ToolBase(std::move(def)) {}

    /**
     * @brief Execute a followup query.
     * @param args_json JSON with required "query" and optional "max_results".
     * @return ServerResponse with results JSON; no directives.
     * @dg_internal
     * @version 2.1.6
     */
    ServerResponse execute(const std::string& args_json) override;

    /**
     * @brief Read-only — requires only READ access.
     * @return MCPAccessLevel::READ, relaxing ToolBase's WRITE default
     *         because this tool only reads prior delegation records.
     * @req REQ-MCP-011
     * @version 2.1.6
     */
    MCPAccessLevel required_access_level() const override {
        return MCPAccessLevel::READ;
    }

    /** @brief Store provider pointer (non-owning).
     * @param p Provider pointer.
     * @utility
     * @version 2.1.6
     */
    void set_provider(const entropic_state_provider_t* p) {
        provider_ = p;
    }

private:
    const entropic_state_provider_t* provider_ = nullptr;
};

/**
 * @brief Execute a followup query against prior delegations.
 * @param args_json JSON with "query" and optional "max_results"
 *                  (default 3).
 * @return A ServerResponse with NO directives whose result is the
 *         search results JSON, or one of the typed errors: invalid args
 *         for non-object/malformed JSON, a required-query error for an
 *         empty query, or storage-not-available when no search provider
 *         is wired.
 * @req REQ-MCP-024
 * @version 2.1.6
 */
ServerResponse FollowupTool::execute(const std::string& args_json) {
    auto args = nlohmann::json::parse(args_json, nullptr, false);
    std::string body;
    if (args.is_discarded() || !args.is_object()) {
        body = R"({"error":"invalid args: object with 'query' required"})";
    } else {
        std::string query = args.value("query", "");
        int max_results = args.value("max_results", 3);
        if (query.empty()) {
            body = R"({"error":"'query' is required and must be non-empty"})";
        } else if (provider_ == nullptr
                   || provider_->search_delegations == nullptr) {
            logger->warn("[followup] no search_delegations provider "
                         "configured");
            body = R"({"error":"delegation storage not available"})";
        } else {
            logger->info("[followup] query='{}' max_results={}",
                         query, max_results);
            char* raw = provider_->search_delegations(
                query.c_str(), max_results, provider_->user_data);
            if (raw == nullptr) {
                body = R"({"results":[]})";
            } else {
                body = raw;
                std::free(raw);
            }
        }
    }
    return {body, {}};
}

// ── ResumeDelegationTool (gh#32, v2.1.6) ────────────────────────

/**
 * @brief Resume a prior delegation with its conversation seeded.
 *
 * Backs `entropic.resume_delegation`. Emits a delegate directive
 * with `resume_from_delegation_id` set; the engine's delegation
 * handler loads the prior child conversation from storage and
 * builds the child context with that history before running the
 * new task.
 *
 * @dg_internal
 * @version 2.1.6
 */
class ResumeDelegationTool : public ToolBase {
public:
    /**
     * @brief Construct from loaded tool definition.
     * @param def Tool definition (entropic/resume_delegation.json).
     * @dg_internal
     * @version 2.1.6
     */
    /**
     * @brief Construct and patch the `target` enum with the tier names.
     * @param def Tool definition (entropic/resume_delegation.json).
     * @param tier_names Tiers a resume may address (gh#162).
     * @dg_internal
     * @version 2.13.0
     */
    ResumeDelegationTool(ToolDefinition def,
                         const std::vector<std::string>& tier_names)
        : ToolBase(std::move(def)) {
        auto schema = nlohmann::json::parse(definition_.input_schema);
        schema["properties"]["target"]["enum"] = tier_names;
        definition_.input_schema = schema.dump();
    }

    /**
     * @brief Emit a resume-flavored delegate directive.
     * @param args_json JSON {delegation_id, task, max_turns?}.
     * @return ServerResponse with delegate + stop directives.
     * @dg_internal
     * @version 2.1.6
     */
    ServerResponse execute(const std::string& args_json) override;
};

/**
 * @brief Parse resume args and emit a resume-flavored DelegateDirective.
 * @param args_json JSON {delegation_id?, target?, task, max_turns?}.
 * @return On valid arguments, a ServerResponse with delegate +
 *         stop_processing directives. On non-object args, a missing
 *         task, or neither delegation_id nor target (gh#162), a typed
 *         error with NO directives — validation precedes any directive
 *         emission.
 * @req REQ-MCP-024
 * @version 2.13.0
 */
ServerResponse ResumeDelegationTool::execute(const std::string& args_json) {
    auto args = nlohmann::json::parse(args_json, nullptr, false);
    if (args.is_discarded() || !args.is_object()) {
        return {R"({"error":"invalid args: object required"})", {}};
    }
    std::string delegation_id = args.value("delegation_id", "");
    std::string target = args.value("target", "");
    std::string task = args.value("task", "");
    int max_turns = args.value("max_turns", -1);
    // gh#162: an id OR a tier. Reaching this tool used to cost a followup
    // round trip on a slow local model before any work started.
    if (task.empty() || (delegation_id.empty() && target.empty())) {
        return {R"({"error":"'task' and one of 'delegation_id' or )"
                R"('target' are required"})",
                {}};
    }
    bool by_target = delegation_id.empty();
    logger->info("[resume_delegation] id='{}' target='{}' task='{}' "
                 "max_turns={}", delegation_id, target, task, max_turns);

    nlohmann::json result;
    result["action"] = "resume_delegation";
    result["delegation_id"] = delegation_id;
    result["target"] = target;
    result["resume_by_target"] = by_target;
    result["task"] = task;
    result["max_turns"] = max_turns;

    Directive delegate_d;
    delegate_d.type = ENTROPIC_DIRECTIVE_DELEGATE;
    Directive stop_d;
    stop_d.type = ENTROPIC_DIRECTIVE_STOP_PROCESSING;
    return {result.dump(), {delegate_d, stop_d}};
}

// ── EntropicServer ──────────────────────────────────────────────

/**
 * @brief Register core tools (todo, complete, phase_change, prune).
 *
 * The control half of the engine's own surface — every one of these
 * returns typed directives the DirectiveProcessor acts on.
 *
 * @param tools_dir Path to tools directory.
 * @return 4 — todo, complete, phase_change and prune_context are
 *         always registered regardless of tier configuration.
 * @req REQ-MCP-024
 * @req REQ-MCP-001
 * @version 1.9.12
 */
int EntropicServer::register_core_tools(
    const std::string& tools_dir) {
    auto todo_def = load_tool_definition(
        "todo", "entropic", tools_dir);
    todo_ = std::make_unique<TodoTool>(std::move(todo_def));
    register_tool(todo_.get());

    auto complete_def = load_tool_definition(
        "complete", "entropic", tools_dir);
    complete_ = std::make_unique<CompleteTool>(
        std::move(complete_def));
    register_tool(complete_.get());

    phase_change_ = std::make_unique<PhaseChangeTool>();
    register_tool(phase_change_.get());

    auto prune_def = load_tool_definition(
        "prune_context", "entropic", tools_dir);
    prune_context_ = std::make_unique<PruneContextTool>(
        std::move(prune_def));
    register_tool(prune_context_.get());

    return 4;
}

/**
 * @brief Register delegation tools when there is somewhere to delegate TO.
 *
 * `tier_names` holds the delegation TARGETS, not every configured tier:
 * `collect_delegatable_tiers` (src/facade/entropic.cpp) and
 * `build_workspace` (src/facade/entropic_mcp.cpp) both drop the source /
 * default tier before calling here, so a tier can never delegate to
 * itself. One target is therefore a perfectly ordinary multi-tier
 * config — a lead plus one worker — and the enum it patches has one
 * legal value.
 *
 * gh#160/gh#162 (v2.13.0): the guard read `size() <= 1`, which was
 * correct at v1.8.5 when the caller passed EVERY tier and a list of one
 * meant "only the source exists". v2.0.4 changed the caller to pass
 * targets only and this guard was not re-read, so the canonical two-tier
 * deployment shipped with NO entropic.delegate, entropic.pipeline or
 * entropic.resume_delegation on the model's menu at all — silently, since
 * an absent tool looks exactly like a model that chose not to call it.
 *
 * @param tools_dir Path to tools directory.
 * @param tier_names Delegation targets for schema patching (source tier
 *                   already excluded by the caller).
 * @param require_context_tiers Tiers that refuse a contextless
 *        delegation (gh#162).
 * @return 3 (delegate, pipeline, resume_delegation) when at least one
 *         target exists; 0 when there is none, which skips registering
 *         them entirely so they never appear in the model's tool list.
 * @req REQ-MCP-024
 * @version 2.13.0
 */
int EntropicServer::register_delegation_tools(
    const std::string& tools_dir,
    const std::vector<std::string>& tier_names,
    const std::vector<std::string>& require_context_tiers) {
    if (tier_names.empty()) {
        return 0;
    }
    auto delegate_def = load_tool_definition(
        "delegate", "entropic", tools_dir);
    delegate_ = std::make_unique<DelegateTool>(
        std::move(delegate_def), tier_names, require_context_tiers);
    register_tool(delegate_.get());

    auto pipeline_def = load_tool_definition(
        "pipeline", "entropic", tools_dir);
    pipeline_ = std::make_unique<PipelineTool>(
        std::move(pipeline_def), tier_names, require_context_tiers);
    register_tool(pipeline_.get());

    // gh#32 (v2.1.6): resume_delegation lives alongside delegate
    // because it produces the same directive kind (resume-flavored).
    auto resume_def = load_tool_definition(
        "resume_delegation", "entropic", tools_dir);
    resume_delegation_ = std::make_unique<ResumeDelegationTool>(
        std::move(resume_def), tier_names);
    register_tool(resume_delegation_.get());

    return 3;
}

/**
 * @brief Register introspection tools (diagnose, inspect, context_inspect).
 *
 * The read-only half of the surface: these return data with NO
 * directives, and all four read engine state through the same
 * state_provider plumbing.
 *
 * @param tools_dir Path to tools directory.
 * @return 4 — diagnose, inspect, context_inspect and followup.
 * @req REQ-MCP-024
 * @req REQ-MCP-011
 * @version 2.3.7
 */
int EntropicServer::register_introspection_tools(
    const std::string& tools_dir) {
    diagnose_ = std::make_unique<DiagnoseTool>(
        load_tool_definition("diagnose", "entropic", tools_dir));
    register_tool(diagnose_.get());

    inspect_ = std::make_unique<InspectTool>(
        load_tool_definition("inspect", "entropic", tools_dir));
    register_tool(inspect_.get());

    context_inspect_ = std::make_unique<ContextInspectTool>(
        load_tool_definition("context_inspect", "entropic", tools_dir));
    register_tool(context_inspect_.get());

    // gh#32 (v2.1.6): followup is read-only and consumes the same
    // state_provider plumbing as diagnose/inspect.
    followup_ = std::make_unique<FollowupTool>(
        load_tool_definition("followup", "entropic", tools_dir));
    register_tool(followup_.get());

    return 4;
}

/**
 * @brief Construct with tier names and data dir, register tools.
 * @param tier_names Available tier names for delegate/pipeline schemas.
 * @param data_dir Path to bundled data directory.
 * @param require_context_tiers Tiers that refuse a contextless
 *        delegation (gh#162).
 * @version 2.13.0
 */
EntropicServer::EntropicServer(
    const std::vector<std::string>& tier_names,
    const std::string& data_dir,
    const std::vector<std::string>& require_context_tiers)
    : MCPServerBase("entropic") {

    std::string tools_dir = data_dir + "/tools";
    int count = register_core_tools(tools_dir);
    count += register_delegation_tools(tools_dir, tier_names,
                                       require_context_tiers);
    count += register_introspection_tools(tools_dir);

    logger->info("EntropicServer initialized with {} tools "
                 "({} tiers)", count, tier_names.size());
}

/**
 * @brief Destructor.
 * @version 1.9.12
 */
EntropicServer::~EntropicServer() = default;

/**
 * @brief Skip duplicate check for delegate and pipeline.
 *
 * Both are legitimately repeatable side effects — the same delegation
 * issued twice is two delegations, not a stuck model.
 *
 * @param tool_name Local tool name.
 * @return true for "delegate" and "pipeline"; false for every other
 *         entropic tool, which stay duplicate-checked.
 * @req REQ-MCP-015
 * @version 1.8.5
 */
bool EntropicServer::skip_duplicate_check(
    const std::string& tool_name) const {
    return tool_name == "delegate" || tool_name == "pipeline";
}

/**
 * @brief Set the engine state provider for introspection tools.
 *
 * Until this is called every introspection tool answers with the
 * "engine state provider not configured" error rather than
 * dereferencing a null callback.
 *
 * @param provider Callback struct with engine state accessors; copied
 *                 into the server, and the copy's address handed to
 *                 diagnose, inspect, context_inspect and followup.
 * @req REQ-MCP-024
 * @version 2.1.6
 */
void EntropicServer::set_state_provider(
    const entropic_state_provider_t& provider) {
    state_provider_ = provider;
    diagnose_->set_provider(&state_provider_);
    inspect_->set_provider(&state_provider_);
    context_inspect_->set_provider(&state_provider_);
    if (followup_) {
        followup_->set_provider(&state_provider_);
    }
    logger->info("State provider set for introspection tools");
}

} // namespace entropic
