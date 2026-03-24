/**
 * @file directives.h
 * @brief Directive processing for tool-to-engine communication.
 *
 * Tools return results with embedded directives. The MCP layer extracts
 * these into typed Directive structs (v1.8.5). Core's DirectiveProcessor
 * dispatches each directive to a registered handler.
 *
 * Core never touches JSON — it receives typed C++ objects only.
 *
 * @version 1.8.4
 */

#pragma once

#include <entropic/types/enums.h>
#include <entropic/types/message.h>
#include <entropic/interfaces/i_hook_handler.h>

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace entropic {

struct LoopContext; // forward declaration

// ── Directive structs ────────────────────────────────────

/**
 * @brief Base directive — all directives carry a type tag.
 * @version 1.8.4
 */
struct Directive {
    entropic_directive_type_t type; ///< Discriminant for dispatch
};

/**
 * @brief Stop processing remaining tool calls.
 * @version 1.8.4
 */
struct StopProcessingDirective : Directive {
    /**
     * @brief Construct a StopProcessing directive.
     * @version 1.8.4
     */
    StopProcessingDirective() {
        type = ENTROPIC_DIRECTIVE_STOP_PROCESSING;
    }
};

/**
 * @brief Request a tier change.
 * @version 1.8.4
 */
struct TierChangeDirective : Directive {
    std::string tier;   ///< Target tier name
    std::string reason; ///< Why the change was requested

    /**
     * @brief Construct a TierChange directive.
     * @param t Target tier.
     * @param r Reason.
     * @version 1.8.4
     */
    TierChangeDirective(std::string t = "", std::string r = "")
        : tier(std::move(t)), reason(std::move(r)) {
        type = ENTROPIC_DIRECTIVE_TIER_CHANGE;
    }
};

/**
 * @brief Delegate a task to a child inference loop.
 * @version 1.8.4
 */
struct DelegateDirective : Directive {
    std::string target; ///< Target tier for delegation
    std::string task;   ///< Task description
    int max_turns = -1; ///< Max turns (-1 = default)

    /**
     * @brief Construct a Delegate directive.
     * @param tgt Target tier.
     * @param tsk Task description.
     * @param mt Max turns.
     * @version 1.8.4
     */
    DelegateDirective(std::string tgt = "",
                      std::string tsk = "",
                      int mt = -1)
        : target(std::move(tgt)),
          task(std::move(tsk)),
          max_turns(mt) {
        type = ENTROPIC_DIRECTIVE_DELEGATE;
    }
};

/**
 * @brief Execute a multi-stage delegation pipeline.
 * @version 1.8.4
 */
struct PipelineDirective : Directive {
    std::vector<std::string> stages; ///< Tier names in order
    std::string task;                ///< Task description

    /**
     * @brief Construct a Pipeline directive.
     * @param s Stages.
     * @param t Task.
     * @version 1.8.4
     */
    PipelineDirective(std::vector<std::string> s = {},
                      std::string t = "")
        : stages(std::move(s)), task(std::move(t)) {
        type = ENTROPIC_DIRECTIVE_PIPELINE;
    }
};

/**
 * @brief Signal explicit completion.
 * @version 1.8.4
 */
struct CompleteDirective : Directive {
    std::string summary; ///< Completion summary

    /**
     * @brief Construct a Complete directive.
     * @param s Summary.
     * @version 1.8.4
     */
    explicit CompleteDirective(std::string s = "")
        : summary(std::move(s)) {
        type = ENTROPIC_DIRECTIVE_COMPLETE;
    }
};

/**
 * @brief Clear self-directed todos (engine no-op).
 * @version 1.8.4
 */
struct ClearSelfTodosDirective : Directive {
    /**
     * @brief Construct a ClearSelfTodos directive.
     * @version 1.8.4
     */
    ClearSelfTodosDirective() {
        type = ENTROPIC_DIRECTIVE_CLEAR_SELF_TODOS;
    }
};

/**
 * @brief Inject a message into the conversation context.
 * @version 1.8.4
 */
struct InjectContextDirective : Directive {
    std::string content; ///< Content to inject
    std::string role = "user"; ///< Message role

    /**
     * @brief Construct an InjectContext directive.
     * @param c Content.
     * @param r Role.
     * @version 1.8.4
     */
    InjectContextDirective(std::string c = "",
                           std::string r = "user")
        : content(std::move(c)), role(std::move(r)) {
        type = ENTROPIC_DIRECTIVE_INJECT_CONTEXT;
    }
};

/**
 * @brief Prune old tool results from context.
 * @version 1.8.4
 */
struct PruneMessagesDirective : Directive {
    int keep_recent = 2; ///< How many recent results to keep

    /**
     * @brief Construct a PruneMessages directive.
     * @param k Keep recent count.
     * @version 1.8.4
     */
    explicit PruneMessagesDirective(int k = 2)
        : keep_recent(k) {
        type = ENTROPIC_DIRECTIVE_PRUNE_MESSAGES;
    }
};

/**
 * @brief Update a keyed persistent context anchor.
 * @version 1.8.4
 */
struct ContextAnchorDirective : Directive {
    std::string key;     ///< Anchor key (unique, update-in-place)
    std::string content; ///< Anchor content (empty = remove)

    /**
     * @brief Construct a ContextAnchor directive.
     * @param k Key.
     * @param c Content.
     * @version 1.8.4
     */
    ContextAnchorDirective(std::string k = "",
                           std::string c = "")
        : key(std::move(k)), content(std::move(c)) {
        type = ENTROPIC_DIRECTIVE_CONTEXT_ANCHOR;
    }
};

/**
 * @brief Switch the active inference phase.
 * @version 1.8.4
 */
struct PhaseChangeDirective : Directive {
    std::string phase; ///< Target phase name

    /**
     * @brief Construct a PhaseChange directive.
     * @param p Phase name.
     * @version 1.8.4
     */
    explicit PhaseChangeDirective(std::string p = "")
        : phase(std::move(p)) {
        type = ENTROPIC_DIRECTIVE_PHASE_CHANGE;
    }
};

/**
 * @brief Generic UI notification passthrough.
 * @version 1.8.4
 */
struct NotifyPresenterDirective : Directive {
    std::string key;       ///< Notification key
    std::string data_json; ///< Arbitrary JSON payload

    /**
     * @brief Construct a NotifyPresenter directive.
     * @param k Key.
     * @param d Data JSON.
     * @version 1.8.4
     */
    NotifyPresenterDirective(std::string k = "",
                             std::string d = "")
        : key(std::move(k)), data_json(std::move(d)) {
        type = ENTROPIC_DIRECTIVE_NOTIFY_PRESENTER;
    }
};

// ── DirectiveResult ──────────────────────────────────────

/**
 * @brief Aggregate result of processing a batch of directives.
 * @version 1.8.4
 */
struct DirectiveResult {
    bool stop_processing = false;           ///< Halt further processing
    bool tier_changed = false;              ///< Tier was switched
    std::vector<Message> injected_messages; ///< Messages to inject
};

// ── DirectiveHandler type ────────────────────────────────

/**
 * @brief Directive handler function type.
 *
 * @param ctx Current loop context (mutable).
 * @param directive The directive to process.
 * @param result Aggregate result (accumulator).
 * @version 1.8.4
 */
using DirectiveHandler = std::function<void(
    LoopContext& ctx,
    const Directive& directive,
    DirectiveResult& result)>;

// ── DirectiveProcessor ───────────────────────────────────

/**
 * @brief Processes tool directives via registry-based dispatch.
 *
 * The engine registers handlers for each directive type. The processor
 * iterates a directive list and dispatches each to the registered handler.
 *
 * @version 1.8.4
 */
class DirectiveProcessor {
public:
    /**
     * @brief Register a handler for a directive type.
     * @param dtype Directive type enum value.
     * @param handler Handler function.
     * @version 1.8.4
     */
    void register_handler(entropic_directive_type_t dtype,
                          DirectiveHandler handler);

    /**
     * @brief Process a list of directives.
     * @param ctx Current loop context.
     * @param directives Typed directive list.
     * @return Aggregate result.
     * @version 1.8.4
     */
    DirectiveResult process(
        LoopContext& ctx,
        const std::vector<const Directive*>& directives);

    /**
     * @brief Set the hook dispatch interface.
     * @param hooks Hook dispatch interface.
     * @version 1.9.1
     */
    void set_hooks(const HookInterface& hooks) { hooks_ = hooks; }

private:
    /**
     * @brief Fire ON_DIRECTIVE or ON_CUSTOM_DIRECTIVE hook.
     * @param directive Directive being processed.
     * @param has_handler Whether a handler exists.
     * @return true to proceed, false if suppressed.
     * @version 1.9.1
     */
    bool fire_directive_hook(const Directive* directive,
                             bool has_handler);          ///< @internal

    std::unordered_map<int, DirectiveHandler> handlers_; ///< Type → handler
    HookInterface hooks_;                                ///< Hook dispatch (v1.9.1)
};

} // namespace entropic
