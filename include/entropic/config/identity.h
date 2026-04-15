// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file identity.h
 * @brief Identity configuration -- shared by static and dynamic identities.
 *
 * Pure type header with no logic. Both static (YAML-loaded) and dynamic
 * (API-created) identities use IdentityConfig. The IdentityManager owns
 * all instances.
 *
 * @par Dependencies
 * - MCPKey, PhaseConfig from types/config.h
 *
 * @version 1.9.6
 */

#pragma once

#include <entropic/types/config.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace entropic {

/**
 * @brief Origin of an identity -- how it was created.
 * @version 1.9.6
 */
enum class IdentityOrigin : uint8_t {
    STATIC  = 0,  ///< Loaded from YAML frontmatter file at startup
    DYNAMIC = 1,  ///< Created at runtime via API
};

/**
 * @brief Full identity configuration.
 *
 * All fields from IdentityFrontmatter are present. Dynamic identities
 * set these programmatically; static identities set them from YAML parsing.
 *
 * The `origin` field distinguishes static from dynamic identities. This
 * matters for lifecycle: static identities cannot be destroyed via the
 * dynamic identity API (they are owned by the config loader).
 *
 * @version 1.9.6 (extended from v1.8.6 IdentityFrontmatter)
 */
struct IdentityConfig {
    std::string name;                           ///< Unique identity name (e.g., "eng", "npc_blacksmith")
    std::string system_prompt;                  ///< Full system prompt text (markdown body)
    std::vector<std::string> focus;             ///< Classification focus keywords (min 1)
    std::vector<std::string> examples;          ///< Few-shot classification examples
    std::string grammar_id;                     ///< Grammar registry key (empty = no grammar)
    std::string auto_chain;                     ///< Auto-chain target tier name (empty = none)
    std::vector<std::string> allowed_tools;     ///< Tool filter list (empty = all tools via identity)
    std::vector<std::string> bash_commands;     ///< Allowed bash commands
    std::vector<MCPKey> mcp_keys;               ///< MCP authorization keys (v1.9.4)
    std::string adapter_path;                   ///< LoRA adapter path (v1.9.2, empty = base model)
    int max_output_tokens = 1024;               ///< Default max output tokens
    float temperature = 0.7f;                   ///< Default temperature
    float repeat_penalty = 1.1f;                ///< Default repeat penalty
    bool enable_thinking = false;               ///< Default enable_thinking
    std::string model_preference = "primary";   ///< Model key from registry
    bool interstitial = false;                  ///< Interstitial (non-routable, triggered by tools)
    bool routable = true;                       ///< Participates in tier routing
    std::string role_type = "front_office";     ///< "front_office", "back_office", or "utility"
    bool explicit_completion = false;           ///< Requires explicit entropic.complete to finish
    std::unordered_map<std::string, PhaseConfig> phases; ///< Named inference phases
    IdentityOrigin origin = IdentityOrigin::DYNAMIC; ///< How this identity was created
};

} // namespace entropic
