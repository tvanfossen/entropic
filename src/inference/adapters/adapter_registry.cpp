// SPDX-License-Identifier: Apache-2.0
/**
 * @file adapter_registry.cpp
 * @brief Adapter factory implementation.
 *
 * gh#87 (v2.7.0): the per-family adapters (qwen35 / qwen36 / gemma4 /
 * nemotron3) were retired — llama.cpp `common_chat` now owns tool-call
 * render + parse for every bundled family (verified per-family in Phase A).
 * All tiers use GenericAdapter, which still provides identity / system-prompt
 * assembly plus a generic ChatML+JSON fallback parse for the rare path where
 * common_chat rendering is unavailable. A model-specific custom adapter is a
 * consumer concern, plumbed out-of-tree.
 *
 * @version 1.8.2
 */

#include "adapter_registry.h"
#include "generic_adapter.h"

#include <entropic/types/logging.h>

#include <algorithm>
#include <string>
#include <unordered_set>

namespace entropic {

namespace {
auto logger = entropic::log::get("inference.adapter.registry");

/**
 * @brief Lowercase a name for case-insensitive recognition.
 * @param name Adapter name as provided by config.
 * @return Lowercased copy.
 * @internal
 * @version 2.1.9
 */
std::string to_lower(const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return lower;
}
} // anonymous namespace

/**
 * @brief Create the chat adapter for a tier (gh#87: always GenericAdapter).
 *
 * The historical per-family names (qwen35 / qwen36 / gemma4 / nemotron3) and
 * "generic" are accepted silently — common_chat handles their native wire
 * format, so no family-specific adapter is needed. Any other name logs a
 * warning (likely a stale or misconfigured custom adapter) and still falls
 * back to GenericAdapter for identity assembly.
 *
 * @param name Adapter name from config (historical family name or "generic").
 * @param tier_name Identity tier name.
 * @param identity_prompt Assembled identity prompt.
 * @return Owned GenericAdapter instance.
 * @internal
 * @version 2.7.0
 */
std::unique_ptr<ChatAdapter> create_adapter(
    const std::string& name,
    const std::string& tier_name,
    const std::string& identity_prompt)
{
    static const std::unordered_set<std::string> known = {
        "generic", "qwen35", "qwen36", "gemma4", "nemotron3"};
    if (known.find(to_lower(name)) == known.end()) {
        logger->warn("Unknown adapter '{}' — using generic "
                     "(common_chat is the default tool path)", name);
    }
    logger->info("Adapter created: generic (config='{}'), tier={}",
                 name, tier_name);
    return std::make_unique<GenericAdapter>(tier_name, identity_prompt);
}

} // namespace entropic
