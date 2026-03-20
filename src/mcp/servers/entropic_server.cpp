/**
 * @file entropic_server.cpp
 * @brief EntropicServer implementation — engine-level directive tools.
 *
 * All tools emit directives. Directive parameters are carried through the
 * ServerResponse result JSON; the engine's MCP layer reconstructs typed
 * directives from the serialized envelope. Base Directive structs in the
 * directives vector carry only the type tag for serialize_response().
 *
 * @version 1.8.5
 */

#include <entropic/mcp/servers/entropic_server.h>
#include <entropic/mcp/tool_base.h>
#include <entropic/mcp/server_base.h>
#include <entropic/core/directives.h>
#include <entropic/types/logging.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <vector>

static auto logger = entropic::log::get("mcp.entropic");

namespace entropic {

// ── TodoItem ────────────────────────────────────────────────────

/**
 * @brief Single todo entry.
 * @internal
 * @version 1.8.5
 */
struct TodoItem {
    std::string content; ///< Item text
    std::string status;  ///< "pending", "in_progress", "done"
};

// ── TodoTool ────────────────────────────────────────────────────

/**
 * @brief Tool for managing a persistent todo list.
 * @internal
 * @version 1.8.5
 */
class TodoTool : public ToolBase {
public:
    /**
     * @brief Construct from tool definition.
     * @param def Tool definition loaded from entropic/todo.json.
     * @internal
     * @version 1.8.5
     */
    explicit TodoTool(ToolDefinition def)
        : ToolBase(std::move(def)) {}

    /**
     * @brief Execute todo action (add/update/remove).
     * @param args_json JSON with "action", "content", "index", "status".
     * @return ServerResponse with todo state and directives.
     * @internal
     * @version 1.8.5
     */
    ServerResponse execute(const std::string& args_json) override;

    /**
     * @brief Anchor key for todo state replacement.
     * @param args_json Arguments (unused).
     * @return "todo_state" anchor key.
     * @internal
     * @version 1.8.5
     */
    std::string anchor_key(
        const std::string& args_json) const override;

private:
    std::vector<TodoItem> items_; ///< Todo list state

    /**
     * @brief Format the todo list as human-readable text.
     * @return Formatted todo list string.
     * @internal
     * @version 1.8.5
     */
    std::string format_list() const;
};

/**
 * @brief Anchor key for context replacement.
 * @param args_json Arguments (unused).
 * @return "todo_state".
 * @internal
 * @version 1.8.5
 */
std::string TodoTool::anchor_key(
    const std::string& /*args_json*/) const {
    return "todo_state";
}

/**
 * @brief Format todo list as numbered text.
 * @return Formatted string.
 * @internal
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
 * @internal
 * @version 1.8.5
 */
ServerResponse TodoTool::execute(const std::string& args_json) {
    auto args = nlohmann::json::parse(args_json);
    std::string action = args.at("action").get<std::string>();

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
            items_.erase(items_.begin() +
                         static_cast<ptrdiff_t>(idx));
        }
    }

    std::string list_text = format_list();

    nlohmann::json result;
    result["todo_state"] = list_text;
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
 * @internal
 * @version 1.8.5
 */
class DelegateTool : public ToolBase {
public:
    /**
     * @brief Construct and patch input schema with tier names.
     * @param def Tool definition loaded from entropic/delegate.json.
     * @param tier_names Available tier names for enum patching.
     * @internal
     * @version 1.8.5
     */
    DelegateTool(ToolDefinition def,
                 const std::vector<std::string>& tier_names);

    /**
     * @brief Execute delegation.
     * @param args_json JSON with "target", "task", "max_turns".
     * @return ServerResponse with delegate + stop directives.
     * @internal
     * @version 1.8.5
     */
    ServerResponse execute(const std::string& args_json) override;
};

/**
 * @brief Construct and patch enum in input schema.
 * @param def Tool definition.
 * @param tier_names Tier names for target enum.
 * @internal
 * @version 1.8.5
 */
DelegateTool::DelegateTool(
    ToolDefinition def,
    const std::vector<std::string>& tier_names)
    : ToolBase(std::move(def)) {

    auto schema = nlohmann::json::parse(definition_.input_schema);
    schema["properties"]["target"]["enum"] = tier_names;
    definition_.input_schema = schema.dump();

    logger->info("[delegate] patched enum with {} tiers",
                 tier_names.size());
}

/**
 * @brief Parse delegation args and emit directives.
 * @param args_json JSON with "target", "task", optional "max_turns".
 * @return ServerResponse with result text and directives.
 * @internal
 * @version 1.8.5
 */
ServerResponse DelegateTool::execute(const std::string& args_json) {
    auto args = nlohmann::json::parse(args_json);
    std::string target = args.at("target").get<std::string>();
    std::string task = args.at("task").get<std::string>();
    int max_turns = args.value("max_turns", -1);

    logger->info("[delegate] target='{}' task='{}' max_turns={}",
                 target, task, max_turns);

    nlohmann::json result;
    result["action"] = "delegate";
    result["target"] = target;
    result["task"] = task;
    result["max_turns"] = max_turns;

    Directive delegate_d;
    delegate_d.type = ENTROPIC_DIRECTIVE_DELEGATE;

    Directive stop_d;
    stop_d.type = ENTROPIC_DIRECTIVE_STOP_PROCESSING;

    return {result.dump(), {delegate_d, stop_d}};
}

// ── PipelineTool ────────────────────────────────────────────────

/**
 * @brief Tool for multi-stage delegation pipelines.
 * @internal
 * @version 1.8.5
 */
class PipelineTool : public ToolBase {
public:
    /**
     * @brief Construct and patch input schema with tier names.
     * @param def Tool definition loaded from entropic/pipeline.json.
     * @param tier_names Available tier names for enum patching.
     * @internal
     * @version 1.8.5
     */
    PipelineTool(ToolDefinition def,
                 const std::vector<std::string>& tier_names);

    /**
     * @brief Execute pipeline setup.
     * @param args_json JSON with "stages" array and "task".
     * @return ServerResponse with pipeline + stop directives.
     * @internal
     * @version 1.8.5
     */
    ServerResponse execute(const std::string& args_json) override;
};

/**
 * @brief Construct and patch stage enum in input schema.
 * @param def Tool definition.
 * @param tier_names Tier names for stages enum.
 * @internal
 * @version 1.8.5
 */
PipelineTool::PipelineTool(
    ToolDefinition def,
    const std::vector<std::string>& tier_names)
    : ToolBase(std::move(def)) {

    auto schema = nlohmann::json::parse(definition_.input_schema);
    schema["properties"]["stages"]["items"]["enum"] = tier_names;
    definition_.input_schema = schema.dump();

    logger->info("[pipeline] patched enum with {} tiers",
                 tier_names.size());
}

/**
 * @brief Parse pipeline args, validate stages, emit directives.
 * @param args_json JSON with "stages" and "task".
 * @return ServerResponse with result text and directives.
 * @internal
 * @version 1.8.5
 */
ServerResponse PipelineTool::execute(const std::string& args_json) {
    auto args = nlohmann::json::parse(args_json);
    auto stages = args.at("stages").get<std::vector<std::string>>();
    std::string task = args.at("task").get<std::string>();

    if (stages.size() < 2) {
        logger->warn("[pipeline] rejected: fewer than 2 stages");
        return {"Error: pipeline requires at least 2 stages", {}};
    }

    logger->info("[pipeline] stages={} task='{}'",
                 stages.size(), task);

    nlohmann::json result;
    result["action"] = "pipeline";
    result["stages"] = stages;
    result["task"] = task;

    Directive pipeline_d;
    pipeline_d.type = ENTROPIC_DIRECTIVE_PIPELINE;

    Directive stop_d;
    stop_d.type = ENTROPIC_DIRECTIVE_STOP_PROCESSING;

    return {result.dump(), {pipeline_d, stop_d}};
}

// ── CompleteTool ────────────────────────────────────────────────

/**
 * @brief Tool for signaling task completion.
 * @internal
 * @version 1.8.5
 */
class CompleteTool : public ToolBase {
public:
    /**
     * @brief Construct from tool definition.
     * @param def Tool definition loaded from entropic/complete.json.
     * @internal
     * @version 1.8.5
     */
    explicit CompleteTool(ToolDefinition def)
        : ToolBase(std::move(def)) {}

    /**
     * @brief Execute completion signal.
     * @param args_json JSON with "summary".
     * @return ServerResponse with complete + stop directives.
     * @internal
     * @version 1.8.5
     */
    ServerResponse execute(const std::string& args_json) override;
};

/**
 * @brief Parse summary and emit completion directives.
 * @param args_json JSON with "summary".
 * @return ServerResponse with summary and directives.
 * @internal
 * @version 1.8.5
 */
ServerResponse CompleteTool::execute(const std::string& args_json) {
    auto args = nlohmann::json::parse(args_json);
    std::string summary = args.at("summary").get<std::string>();

    logger->info("[complete] summary='{}'", summary);

    nlohmann::json result;
    result["action"] = "complete";
    result["summary"] = summary;

    Directive complete_d;
    complete_d.type = ENTROPIC_DIRECTIVE_COMPLETE;

    Directive stop_d;
    stop_d.type = ENTROPIC_DIRECTIVE_STOP_PROCESSING;

    return {result.dump(), {complete_d, stop_d}};
}

// ── PhaseChangeTool ─────────────────────────────────────────────

/**
 * @brief Tool for switching inference phase.
 * @internal
 * @version 1.8.5
 */
class PhaseChangeTool : public ToolBase {
public:
    /**
     * @brief Construct with inline tool definition.
     * @internal
     * @version 1.8.5
     */
    PhaseChangeTool();

    /**
     * @brief Execute phase change.
     * @param args_json JSON with "phase".
     * @return ServerResponse with phase_change directive.
     * @internal
     * @version 1.8.5
     */
    ServerResponse execute(const std::string& args_json) override;
};

/**
 * @brief Build inline definition for phase_change tool.
 * @internal
 * @version 1.8.5
 */
PhaseChangeTool::PhaseChangeTool()
    : ToolBase({"phase_change",
                "Switch inference phase",
                R"({"type":"object","properties":{"phase":{"type":"string"}},"required":["phase"]})"}) {}

/**
 * @brief Parse phase and emit directive.
 * @param args_json JSON with "phase".
 * @return ServerResponse with phase_change directive.
 * @internal
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
 * @internal
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
 * @param args_json JSON with optional "keep_recent".
 * @return ServerResponse with prune_messages directive.
 * @internal
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

// ── EntropicServer ──────────────────────────────────────────────

/**
 * @brief Construct with tier names and data dir, register tools.
 * @param tier_names Available tier names for delegate/pipeline schemas.
 * @param data_dir Path to bundled data directory.
 * @version 1.8.5
 */
EntropicServer::EntropicServer(
    const std::vector<std::string>& tier_names,
    const std::string& data_dir)
    : MCPServerBase("entropic") {

    std::string tools_dir = data_dir + "/tools";
    int tool_count = 0;

    auto todo_def = load_tool_definition(
        "todo", "entropic", tools_dir);
    todo_ = std::make_unique<TodoTool>(std::move(todo_def));
    register_tool(todo_.get());
    ++tool_count;

    auto complete_def = load_tool_definition(
        "complete", "entropic", tools_dir);
    complete_ = std::make_unique<CompleteTool>(
        std::move(complete_def));
    register_tool(complete_.get());
    ++tool_count;

    phase_change_ = std::make_unique<PhaseChangeTool>();
    register_tool(phase_change_.get());
    ++tool_count;

    auto prune_def = load_tool_definition(
        "prune_context", "entropic", tools_dir);
    prune_context_ = std::make_unique<PruneContextTool>(
        std::move(prune_def));
    register_tool(prune_context_.get());
    ++tool_count;

    if (tier_names.size() > 1) {
        auto delegate_def = load_tool_definition(
            "delegate", "entropic", tools_dir);
        delegate_ = std::make_unique<DelegateTool>(
            std::move(delegate_def), tier_names);
        register_tool(delegate_.get());
        ++tool_count;

        auto pipeline_def = load_tool_definition(
            "pipeline", "entropic", tools_dir);
        pipeline_ = std::make_unique<PipelineTool>(
            std::move(pipeline_def), tier_names);
        register_tool(pipeline_.get());
        ++tool_count;
    }

    logger->info("EntropicServer initialized with {} tools "
                 "({} tiers)", tool_count, tier_names.size());
}

/**
 * @brief Destructor.
 * @version 1.8.5
 */
EntropicServer::~EntropicServer() = default;

/**
 * @brief Skip duplicate check for delegate and pipeline.
 * @param tool_name Local tool name.
 * @return true for delegate and pipeline.
 * @internal
 * @version 1.8.5
 */
bool EntropicServer::skip_duplicate_check(
    const std::string& tool_name) const {
    return tool_name == "delegate" || tool_name == "pipeline";
}

} // namespace entropic
