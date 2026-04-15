// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file classification.h
 * @brief Auto-generated classification prompt for the router model.
 *
 * Ports Python's build_classification_prompt() and _interleave_examples().
 *
 * @version 1.8.1
 */

#pragma once

#include <entropic/entropic_export.h>
#include <string>
#include <vector>

namespace entropic::prompts {

/**
 * @brief Lightweight tier descriptor for classification prompt building.
 *
 * Only the fields needed to build the prompt. Not a full identity —
 * avoids coupling the prompt builder to the full identity system.
 *
 * @version 1.8.1
 */
struct TierDescriptor {
    std::string name;                   ///< Tier name (e.g., "lead")
    std::vector<std::string> focus;     ///< Focus areas
    std::vector<std::string> examples;  ///< Few-shot examples
};

/**
 * @brief Round-robin interleave few-shot examples across tiers.
 *
 * Cycling through tiers prevents recency bias — no single tier's
 * examples dominate the tail of the prompt. The 0.6B router model
 * is sensitive to example ordering.
 *
 * @param tiers Ordered tiers (index+1 = classification digit).
 * @return Lines formatted as: "example text" -> digit
 * @version 1.8.1
 */
ENTROPIC_EXPORT std::vector<std::string> interleave_examples(
    const std::vector<TierDescriptor>& tiers);

/**
 * @brief Auto-generate classification prompt from tier focus + examples.
 *
 * The trailing " -> " (with space) constrains the router model to
 * output a single digit. Used with max_tokens=1 for classification.
 *
 * @param tiers Ordered tiers (index+1 = classification digit).
 * @param message User message to classify.
 * @param history Recent user messages for context (optional).
 * @param recent_tiers Recent tier activations for continuity (optional).
 * @return Classification prompt string.
 * @version 1.8.1
 */
ENTROPIC_EXPORT std::string build_classification_prompt(
    const std::vector<TierDescriptor>& tiers,
    const std::string& message,
    const std::vector<std::string>& history = {},
    const std::vector<std::string>& recent_tiers = {});

} // namespace entropic::prompts
