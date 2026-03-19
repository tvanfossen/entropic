/**
 * @file message.h
 * @brief Message struct for conversation history.
 *
 * Internal C++ representation. At .so boundaries, messages are serialized
 * as JSON arrays: [{"role":"user","content":"...","metadata":{...},"tool_calls":[]}]
 *
 * @version 1.8.2
 */

#pragma once

#include <string>
#include <vector>
#include <unordered_map>

namespace entropic {

/**
 * @brief A message in a conversation.
 *
 * Maps to Python Message dataclass. Roles: "user", "assistant", "system", "tool".
 * Metadata is arbitrary key-value pairs (not enforced at type level).
 *
 * @version 1.8.2
 */
struct Message {
    std::string role;                                          ///< Message role
    std::string content;                                       ///< Message text content
    std::unordered_map<std::string, std::string> metadata;     ///< Arbitrary metadata
};

} // namespace entropic
