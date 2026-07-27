// SPDX-License-Identifier: Apache-2.0
/**
 * @file adapter_registry.cpp
 * @brief Adapter factory implementation.
 *
 * gh#87 Phase D (v2.7.0): hybrid parse ownership. llama.cpp `common_chat`
 * owns tool-call parsing for families it has a DEDICATED grammar for
 * (gemma4 → PEG_GEMMA4, multi-parameter safe). Families that fall to
 * common_chat's PEG autoparser (Qwen, nemotron3) keep their hand-rolled
 * adapter, because the autoparser only extracts the first `<parameter=>`
 * of a multi-parameter call. So:
 *   - qwen35 / qwen36 / nemotron3 → their per-family adapter (multi-param).
 *   - gemma4 / anything else      → GenericAdapter (identity assembly only;
 *                                    parsing flows through common_chat).
 *
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
#include <string>
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
 * @brief Build the static adapter dispatch table (gh#87 Phase D).
 *
 * The autoparser families that need a multi-parameter parser, plus gemma4.
 *
 * gh#108 (v2.10.3): gemma4 WAS intentionally absent — it parses tool calls
 * via common_chat's PEG_GEMMA4 grammar, so it fell through to
 * GenericAdapter. But that grammar is only reachable when
 * common_chat_parse_reliable() is true, which needs a TOOLED render to have
 * captured a parser arena. A toolless generate fell to Generic, which strips
 * `<think>` — a marker gemma4 never emits — leaving `<|channel>` reasoning
 * in content on every path. Gemma4Adapter is the fallback that closes it;
 * PEG_GEMMA4 remains primary whenever an arena exists.
 *
 * @return Registered factories, in lookup order.
 * @internal
 * @version 2.10.3
 */
const std::array<AdapterEntry, 4>& adapter_table() {
    static const std::array<AdapterEntry, 4> table{{
        {"gemma4",    [](auto& t, auto& p) {
            return std::make_unique<Gemma4Adapter>(t, p); }},
        {"qwen35",    [](auto& t, auto& p) {
            return std::make_unique<Qwen35Adapter>(t, p); }},
        {"qwen36",    [](auto& t, auto& p) {
            return std::make_unique<Qwen36Adapter>(t, p); }},
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
 * @brief Create adapter by name (gh#87 Phase D hybrid).
 *
 * Autoparser families (qwen35 / qwen36 / nemotron3) resolve to their
 * per-family multi-parameter adapter. Everything else — including gemma4,
 * whose tool calls are parsed by common_chat's dedicated grammar — falls
 * back to GenericAdapter (identity assembly + generic fallback parse).
 *
 * @param name Adapter name from config.
 * @param tier_name Identity tier name.
 * @param identity_prompt Assembled identity prompt.
 * @return Owned adapter instance.
 * @internal
 * @version 2.7.0 (Phase D)
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
    logger->info("Adapter created: generic (config='{}'), tier={} "
                 "(tool parsing via common_chat)", name, tier_name);
    return std::make_unique<GenericAdapter>(tier_name, identity_prompt);
}

} // namespace entropic
