/**
 * @file classification.cpp
 * @brief Classification prompt builder implementation.
 * @version 1.8.1
 */

#include <entropic/prompts/classification.h>
#include <algorithm>
#include <cctype>

namespace entropic::prompts {

/**
 * @brief Convert string to uppercase.
 * @param s Input string.
 * @return Uppercase version.
 * @version 1.8.1
 * @internal
 */
static std::string to_upper(const std::string& s)
{
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return result;
}

/**
 * @brief Round-robin interleave few-shot examples across tiers.
 * @param tiers Ordered tiers (index+1 = classification digit).
 * @return Lines formatted as: "example text" -> digit
 * @version 1.8.1
 * @internal
 */
std::vector<std::string> interleave_examples(
    const std::vector<TierDescriptor>& tiers)
{
    struct Queue {
        int digit;
        const std::vector<std::string>* examples;
    };

    std::vector<Queue> queues;
    for (size_t i = 0; i < tiers.size(); ++i) {
        if (!tiers[i].examples.empty()) {
            queues.push_back(
                {static_cast<int>(i + 1), &tiers[i].examples});
        }
    }

    std::vector<std::string> lines;
    size_t round_idx = 0;
    while (true) {
        bool added = false;
        for (const auto& q : queues) {
            if (round_idx < q.examples->size()) {
                lines.push_back("\"" + (*q.examples)[round_idx]
                                + "\" -> "
                                + std::to_string(q.digit));
                added = true;
            }
        }
        if (!added) {
            break;
        }
        ++round_idx;
    }
    return lines;
}

/**
 * @brief Join strings with a separator.
 * @param items Strings to join.
 * @param sep Separator between items.
 * @return Joined string.
 * @version 1.8.2
 * @internal
 */
static std::string join(
    const std::vector<std::string>& items, const std::string& sep)
{
    std::string result;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) result += sep;
        result += items[i];
    }
    return result;
}

/**
 * @brief Build tier definition lines for classification prompt.
 * @param tiers Ordered tiers.
 * @return Formatted tier definitions.
 * @version 1.8.2
 * @internal
 */
static std::string format_tier_definitions(
    const std::vector<TierDescriptor>& tiers)
{
    std::string result;
    for (size_t i = 0; i < tiers.size(); ++i) {
        result += std::to_string(i + 1) + " = "
                  + to_upper(tiers[i].name) + ": "
                  + join(tiers[i].focus, ", ") + "\n";
    }
    return result;
}

/**
 * @brief Auto-generate classification prompt from tier focus + examples.
 * @param tiers Ordered tiers (index+1 = classification digit).
 * @param message User message to classify.
 * @param history Recent user messages for context.
 * @param recent_tiers Recent tier activations for continuity.
 * @return Classification prompt string.
 * @version 1.8.2
 * @internal
 */
std::string build_classification_prompt(
    const std::vector<TierDescriptor>& tiers,
    const std::string& message,
    const std::vector<std::string>& history,
    const std::vector<std::string>& recent_tiers)
{
    std::string result = "Classify the message. Reply with the number only.\n\n";
    result += format_tier_definitions(tiers) + "\n";

    if (!recent_tiers.empty()) {
        result += "Recent tiers: " + join(recent_tiers, " -> ") + "\n\n";
    }

    if (!history.empty()) {
        size_t start = history.size() > 5 ? history.size() - 5 : 0;
        std::vector<std::string> recent(history.begin() + static_cast<long>(start), history.end());
        result += "Recent messages: " + join(recent, " | ") + "\n\n";
    }

    for (const auto& line : interleave_examples(tiers)) {
        result += line + "\n";
    }

    result += "\"" + message + "\" -> ";
    return result;
}

} // namespace entropic::prompts
