// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file directives.cpp
 * @brief DirectiveProcessor implementation.
 * @version 1.8.4
 */

#include <entropic/core/directives.h>
#include <entropic/types/logging.h>

#include <cstdlib>
#include <string>

static auto logger = entropic::log::get("core.directives");

namespace entropic {

/**
 * @brief Register a handler for a directive type.
 * @param dtype Directive type enum value.
 * @param handler Handler function.
 * @internal
 * @version 1.8.4
 */
void DirectiveProcessor::register_handler(
    entropic_directive_type_t dtype,
    DirectiveHandler handler) {
    handlers_[static_cast<int>(dtype)] = std::move(handler);
}

/**
 * @brief Process a list of directives, dispatching to handlers.
 * @param ctx Current loop context.
 * @param directives Typed directive list.
 * @return Aggregate result.
 * @internal
 * @version 2.0.0
 */
DirectiveResult DirectiveProcessor::process(
    LoopContext& ctx,
    const std::vector<const Directive*>& directives) {
    logger->info("Processing {} directive(s)", directives.size());
    DirectiveResult result;
    int processed = 0;
    for (const auto* directive : directives) {
        if (directive == nullptr) {
            continue;
        }

        auto it = handlers_.find(static_cast<int>(directive->type));
        bool has_handler = it != handlers_.end();

        // Hook: ON_DIRECTIVE or ON_CUSTOM_DIRECTIVE (v1.9.1)
        if (!fire_directive_hook(directive, has_handler)) {
            continue; // suppressed by hook
        }

        if (has_handler) {
            logger->info("Directive type={}, handler invoked",
                         static_cast<int>(directive->type));
            it->second(ctx, *directive, result);
            ++processed;
            if (result.stop_processing) {
                break;
            }
        }
    }
    logger->info("Directives: {}/{} processed", processed,
                 directives.size());
    return result;
}

/**
 * @brief Fire ON_DIRECTIVE or ON_CUSTOM_DIRECTIVE hook.
 * @param directive The directive being processed.
 * @param has_handler Whether a handler is registered.
 * @return true to proceed, false if suppressed.
 * @internal
 * @version 1.9.1
 */
bool DirectiveProcessor::fire_directive_hook(
    const Directive* directive, bool has_handler) {
    if (hooks_.fire_pre == nullptr) {
        return true;
    }

    auto point = has_handler
        ? ENTROPIC_HOOK_ON_DIRECTIVE
        : ENTROPIC_HOOK_ON_CUSTOM_DIRECTIVE;
    std::string json = "{\"type\":"
        + std::to_string(static_cast<int>(directive->type)) + "}";
    char* mod = nullptr;
    int rc = hooks_.fire_pre(hooks_.registry,
        point, json.c_str(), &mod);
    free(mod);
    return rc == 0;
}

} // namespace entropic
