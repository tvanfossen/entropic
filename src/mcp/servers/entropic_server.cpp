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
 *
 * @version 1.9.12
 */

#include <entropic/mcp/servers/entropic_server.h>
#include <entropic/mcp/tool_base.h>
#include <entropic/mcp/server_base.h>
#include <entropic/core/directives.h>
#include <entropic/types/logging.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
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

// ── DiagnoseTool ────────────────────────────────────────────────

/**
 * @brief Tool for full engine state snapshots.
 * @internal
 * @version 1.9.12
 */
class DiagnoseTool : public ToolBase {
public:
    /**
     * @brief Construct from tool definition.
     * @param def Tool definition loaded from entropic/diagnose.json.
     * @internal
     * @version 1.9.12
     */
    explicit DiagnoseTool(ToolDefinition def)
        : ToolBase(std::move(def)) {}

    /**
     * @brief Execute diagnostic snapshot.
     * @param args_json JSON with optional include_docs, history_limit.
     * @return ServerResponse with JSON snapshot (no directives).
     * @internal
     * @version 1.9.12
     */
    ServerResponse execute(const std::string& args_json) override;

    /**
     * @brief Read-only tool requires only READ access.
     * @return MCPAccessLevel::READ.
     * @internal
     * @version 1.9.12
     */
    MCPAccessLevel required_access_level() const override {
        return MCPAccessLevel::READ;
    }

    /** @brief Set state provider pointer.
     * @param p Provider pointer (must outlive tool).
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
 * @param fn Callback returning malloc'd string (or nullptr).
 * @param ud User data for callback.
 * @return JSON string (empty object if callback is null).
 * @internal
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
 * @internal
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
 * @internal
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
 * @internal
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
 * @param args_json JSON with optional include_docs, history_limit.
 * @return ServerResponse with snapshot JSON.
 * @internal
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
 * @internal
 * @version 1.9.12
 */
class InspectTool : public ToolBase {
public:
    /**
     * @brief Construct from tool definition.
     * @param def Tool definition loaded from entropic/inspect.json.
     * @internal
     * @version 1.9.12
     */
    explicit InspectTool(ToolDefinition def)
        : ToolBase(std::move(def)) {}

    /**
     * @brief Execute targeted inspection.
     * @param args_json JSON with "target" and optional "key".
     * @return ServerResponse with query result (no directives).
     * @internal
     * @version 1.9.12
     */
    ServerResponse execute(const std::string& args_json) override;

    /**
     * @brief Read-only tool requires only READ access.
     * @return MCPAccessLevel::READ.
     * @internal
     * @version 1.9.12
     */
    MCPAccessLevel required_access_level() const override {
        return MCPAccessLevel::READ;
    }

    /** @brief Set state provider pointer.
     * @param p Provider pointer.
     * @version 1.9.12
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
 * @internal
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
 * @internal
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
 * @internal
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
 * @internal
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
 * @internal
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
 * @internal
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
 * @internal
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
 * @internal
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
 * @internal
 * @version 1.9.12
 */
ServerResponse InspectTool::execute(const std::string& args_json) {
    if (provider_ == nullptr) {
        logger->error("[inspect] no state provider set");
        return {"Error: engine state provider not configured", {}};
    }

    auto args = nlohmann::json::parse(args_json);
    std::string target = args.at("target").get<std::string>();
    std::string key = args.value("key", "");

    logger->info("[inspect] target='{}' key='{}'", target, key);

    auto result = dispatch_inspect(*provider_, target, key);
    return {result, {}};
}

// ── EntropicServer ──────────────────────────────────────────────

/**
 * @brief Register core tools (todo, complete, phase_change, prune).
 * @param tools_dir Path to tools directory.
 * @return Number of tools registered.
 * @internal
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
 * @brief Register delegation tools if multi-tier.
 * @param tools_dir Path to tools directory.
 * @param tier_names Tier names for schema patching.
 * @return Number of tools registered.
 * @internal
 * @version 1.9.12
 */
int EntropicServer::register_delegation_tools(
    const std::string& tools_dir,
    const std::vector<std::string>& tier_names) {
    if (tier_names.size() <= 1) {
        return 0;
    }
    auto delegate_def = load_tool_definition(
        "delegate", "entropic", tools_dir);
    delegate_ = std::make_unique<DelegateTool>(
        std::move(delegate_def), tier_names);
    register_tool(delegate_.get());

    auto pipeline_def = load_tool_definition(
        "pipeline", "entropic", tools_dir);
    pipeline_ = std::make_unique<PipelineTool>(
        std::move(pipeline_def), tier_names);
    register_tool(pipeline_.get());

    return 2;
}

/**
 * @brief Register introspection tools (diagnose, inspect).
 * @param tools_dir Path to tools directory.
 * @return Number of tools registered.
 * @internal
 * @version 1.9.12
 */
int EntropicServer::register_introspection_tools(
    const std::string& tools_dir) {
    auto diagnose_def = load_tool_definition(
        "diagnose", "entropic", tools_dir);
    diagnose_ = std::make_unique<DiagnoseTool>(
        std::move(diagnose_def));
    register_tool(diagnose_.get());

    auto inspect_def = load_tool_definition(
        "inspect", "entropic", tools_dir);
    inspect_ = std::make_unique<InspectTool>(
        std::move(inspect_def));
    register_tool(inspect_.get());

    return 2;
}

/**
 * @brief Construct with tier names and data dir, register tools.
 * @param tier_names Available tier names for delegate/pipeline schemas.
 * @param data_dir Path to bundled data directory.
 * @version 1.9.12
 */
EntropicServer::EntropicServer(
    const std::vector<std::string>& tier_names,
    const std::string& data_dir)
    : MCPServerBase("entropic") {

    std::string tools_dir = data_dir + "/tools";
    int count = register_core_tools(tools_dir);
    count += register_delegation_tools(tools_dir, tier_names);
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
 * @param tool_name Local tool name.
 * @return true for delegate and pipeline.
 * @internal
 * @version 1.8.5
 */
bool EntropicServer::skip_duplicate_check(
    const std::string& tool_name) const {
    return tool_name == "delegate" || tool_name == "pipeline";
}

/**
 * @brief Set the engine state provider for introspection tools.
 * @param provider Callback struct with engine state accessors.
 * @internal
 * @version 1.9.12
 */
void EntropicServer::set_state_provider(
    const entropic_state_provider_t& provider) {
    state_provider_ = provider;
    diagnose_->set_provider(&state_provider_);
    inspect_->set_provider(&state_provider_);
    logger->info("State provider set for introspection tools");
}

} // namespace entropic
