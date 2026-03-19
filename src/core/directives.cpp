/**
 * @file directives.cpp
 * @brief DirectiveProcessor implementation.
 * @version 1.8.4
 */

#include <entropic/core/directives.h>
#include <entropic/types/logging.h>

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
 * @version 1.8.4
 */
DirectiveResult DirectiveProcessor::process(
    LoopContext& ctx,
    const std::vector<const Directive*>& directives) {
    DirectiveResult result;
    for (const auto* directive : directives) {
        if (directive == nullptr) {
            continue;
        }
        auto it = handlers_.find(static_cast<int>(directive->type));
        if (it != handlers_.end()) {
            logger->debug("Processing directive type {}",
                          static_cast<int>(directive->type));
            it->second(ctx, *directive, result);
            if (result.stop_processing) {
                break;
            }
        } else {
            logger->warn("No handler for directive type {}",
                         static_cast<int>(directive->type));
        }
    }
    return result;
}

} // namespace entropic
