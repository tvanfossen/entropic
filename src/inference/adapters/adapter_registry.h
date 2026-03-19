/**
 * @file adapter_registry.h
 * @brief Adapter factory — create adapters by name.
 *
 * @version 1.8.2
 */

#pragma once

#include <entropic/inference/adapters/adapter_base.h>

#include <memory>
#include <string>

namespace entropic {

/**
 * @brief Create an adapter instance by name.
 *
 * @param name Adapter name from config (e.g. "qwen35", "generic").
 * @param tier_name Identity tier name.
 * @param identity_prompt Assembled prompt from PromptManager.
 * @return Owned adapter. Falls back to GenericAdapter if unknown.
 * @version 1.8.2
 */
std::unique_ptr<ChatAdapter> create_adapter(
    const std::string& name,
    const std::string& tier_name,
    const std::string& identity_prompt);

} // namespace entropic
