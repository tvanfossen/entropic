/**
 * @file adapter_registry.cpp
 * @brief Adapter factory implementation.
 * @version 1.8.2
 */

#include "adapter_registry.h"
#include "generic_adapter.h"
#include "qwen35_adapter.h"

#include <entropic/types/logging.h>

#include <algorithm>

namespace entropic {

namespace {
auto logger = entropic::log::get("inference.adapter.registry");
} // anonymous namespace

/**
 * @brief Create adapter by name. Falls back to GenericAdapter.
 * @param name Adapter name (e.g. "qwen35", "generic").
 * @param tier_name Identity tier name.
 * @param identity_prompt Assembled identity prompt.
 * @return Owned adapter instance.
 * @internal
 * @version 1.8.2
 */
std::unique_ptr<ChatAdapter> create_adapter(
    const std::string& name,
    const std::string& tier_name,
    const std::string& identity_prompt)
{
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    if (lower == "qwen35") {
        return std::make_unique<Qwen35Adapter>(tier_name, identity_prompt);
    }

    if (lower != "generic") {
        logger->warn("Unknown adapter '{}', falling back to generic", name);
    }
    return std::make_unique<GenericAdapter>(tier_name, identity_prompt);
}

} // namespace entropic
