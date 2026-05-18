// SPDX-License-Identifier: Apache-2.0
/**
 * @file adapter_registry.cpp
 * @brief Adapter factory implementation.
 * @version 1.8.2
 */

#include "adapter_registry.h"
#include "gemma4_adapter.h"
#include "generic_adapter.h"
#include "nemotron3_adapter.h"
#include "qwen35_adapter.h"
#include "qwen36_adapter.h"

#include <entropic/types/logging.h>

#include <algorithm>
#include <array>
#include <functional>
#include <string_view>

namespace entropic {

namespace {
auto logger = entropic::log::get("inference.adapter.registry");

/// @brief Factory signature shared by all adapter constructors.
using AdapterFactory = std::function<std::unique_ptr<ChatAdapter>(
    const std::string&, const std::string&)>;

/**
 * @brief Lookup-table entry: lowercase key → owning factory.
 * @internal
 * @version 2.1.9
 */
struct AdapterEntry {
    std::string_view key;
    AdapterFactory factory;
};

/**
 * @brief Build the static adapter dispatch table.
 *
 * Populated once on first lookup. New adapter families register an
 * additional row here; create_adapter() stays at three returns.
 *
 * @return Registered factories, in lookup order.
 * @internal
 * @version 2.1.9
 */
const std::array<AdapterEntry, 4>& adapter_table() {
    static const std::array<AdapterEntry, 4> table{{
        {"qwen35",    [](auto& t, auto& p) {
            return std::make_unique<Qwen35Adapter>(t, p); }},
        {"qwen36",    [](auto& t, auto& p) {
            return std::make_unique<Qwen36Adapter>(t, p); }},
        {"gemma4",    [](auto& t, auto& p) {
            return std::make_unique<Gemma4Adapter>(t, p); }},
        {"nemotron3", [](auto& t, auto& p) {
            return std::make_unique<Nemotron3Adapter>(t, p); }},
    }};
    return table;
}

/**
 * @brief Lowercase a name for case-insensitive lookup.
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
 * @brief Create adapter by name. Falls back to GenericAdapter.
 *
 * Looks up the lowercased adapter name in the static dispatch table.
 * Unknown names log a warning and fall back to GenericAdapter; the
 * "generic" name itself is silent.
 *
 * @param name Adapter name (e.g. "qwen35", "qwen36", "gemma4",
 *             "nemotron3", "generic").
 * @param tier_name Identity tier name.
 * @param identity_prompt Assembled identity prompt.
 * @return Owned adapter instance.
 * @internal
 * @version 2.1.9
 */
std::unique_ptr<ChatAdapter> create_adapter(
    const std::string& name,
    const std::string& tier_name,
    const std::string& identity_prompt)
{
    const std::string lower = to_lower(name);
    for (const auto& entry : adapter_table()) {
        if (lower == entry.key) {
            logger->info("Adapter created: type={}, tier={}",
                         entry.key, tier_name);
            return entry.factory(tier_name, identity_prompt);
        }
    }
    if (lower != "generic") {
        logger->warn("Unknown adapter '{}', falling back to generic", name);
    }
    logger->info("Adapter created: type=generic, tier={}", tier_name);
    return std::make_unique<GenericAdapter>(tier_name, identity_prompt);
}

} // namespace entropic
