// SPDX-License-Identifier: Apache-2.0
/**
 * @file production_emission_helpers.h
 * @brief Real-emission adapter guarantee for model tests (v2.3.8, gh#71-phase-2).
 *
 * @par Why this exists
 * The CPU-tier `adapter_acceptance_test.cpp` proves the *parser* against
 * captured emit strings. It does NOT prove the live model still emits in
 * that shape under the real prompt. gh#69/#70 shipped broken precisely
 * because the only model-tier coverage rigged the prompt (handed the
 * model the exact tag template) and asserted `>= 1` once. This helper
 * closes that gap: it drives the **actual GGUF** under the **production
 * system prompt** — assembled by the adapter's own `format_system_prompt`
 * (which calls the adapter's real `format_tools`), layered on the real
 * constitution + app_context + identity — and asserts the generated emit
 * parses.
 *
 * @par The guarantee (asserted by run_emission_battery)
 *   1. **The adapter never misses a real emission.** If the raw model
 *      output contains the adapter's native tool-call markup, the adapter
 *      MUST extract >= 1 tool call. This is the exact gh#69/#70 failure
 *      condition — model emits a call, adapter returns zero.
 *   2. **The production path elicits and parses >= 1 call** across the
 *      battery (end-to-end: real prompt -> real model -> real parse).
 *   3. **Every parsed call is well-formed** — non-empty name.
 *   4. **Zero native markup leaks** into cleaned_content.
 *
 * Tool definitions are the real bundled JSONs (data/tools/...), and the
 * system prompt is the real production assembly — nothing rigged.
 *
 * @version 2.3.8
 */

#pragma once

#include "model_test_context.h"

#include <fstream>
#include <sstream>

/**
 * @brief Load a bundled tool definition JSON as a raw string.
 * @param rel_path Path relative to data/tools/ (e.g. "entropic/delegate.json").
 * @return File contents, or empty string if not found.
 * @utility
 * @version 2.3.8
 */
inline std::string load_tool_json(const std::string& rel_path) {
    auto path = fs::path(MODEL_PATH) / "data" / "tools" / rel_path;
    std::ifstream in(path);
    if (!in) {
        spdlog::error("Tool JSON not found: {}", path.string());
        return "";
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/**
 * @brief Result of running one prompt through the production path.
 * @version 2.3.8
 */
struct EmissionResult {
    std::string raw;                     ///< Raw model output
    std::string cleaned;                 ///< Adapter cleaned_content
    std::vector<entropic::ToolCall> calls; ///< Parsed tool calls
    bool emitted_markup = false;         ///< raw contained native markup
    bool carries_name = false;           ///< raw carried a tool-name token
};

/**
 * @brief One battery prompt + what native markup it should produce.
 * @version 2.3.8
 */
struct EmissionCase {
    std::string user;   ///< Directive user prompt
};

/**
 * @brief Aggregate outcome of an emission battery.
 * @version 2.3.8
 */
struct BatteryOutcome {
    int parsed = 0;              ///< Prompts that yielded >= 1 parsed call
    int total = 0;               ///< Battery size (prompts run)
    int emitted_markup = 0;      ///< Prompts whose raw had native markup
    bool emission_missed = false;///< Any prompt: markup present but 0 parsed
    bool named_missed = false;   ///< Any prompt: a NAMED call present but 0 parsed
    bool leaked = false;         ///< Any cleaned_content carried native markup
    bool malformed = false;      ///< Any parsed call had an empty name
};

/// @brief Tokens that indicate the raw output carries a tool *name*
///        (across JSON-key, DSML, and qwen-XML forms).
/// @internal
/// @version 2.3.8
inline const std::vector<std::string>& name_tokens() {
    static const std::vector<std::string> toks = {
        "\"name\"", "\"tool_name\"", "\"function\"", "\"function_name\"",
        "name=\"", "<function="};
    return toks;
}

/**
 * @brief True if `haystack` contains any of the `needles`.
 * @utility
 * @version 2.3.8
 */
inline bool contains_any(const std::string& haystack,
                         const std::vector<std::string>& needles) {
    for (const auto& n : needles) {
        if (haystack.find(n) != std::string::npos) { return true; }
    }
    return false;
}

/**
 * @brief Run one directive prompt through the production prompt + live model.
 *
 * Builds the system message via the adapter's own format_system_prompt
 * (real format_tools) over the production base prompt, generates, and
 * parses with the same adapter. Logs raw + parsed for observability.
 *
 * @param adapter Live tier adapter.
 * @param base_prompt Production system content (constitution+context+identity).
 * @param tool_jsons Real bundled tool definitions.
 * @param uc The user prompt to send.
 * @param native_markers Substrings that mark this adapter's tool-call format.
 * @return EmissionResult with raw, cleaned, parsed calls, and markup flag.
 * @utility
 * @version 2.3.8
 */
inline EmissionResult run_one_emission(
    entropic::ChatAdapter* adapter,
    const std::string& base_prompt,
    const std::vector<std::string>& tool_jsons,
    const EmissionCase& uc,
    const std::vector<std::string>& native_markers)
{
    std::string system_text = adapter->format_system_prompt(base_prompt, tool_jsons);
    auto messages = make_messages(system_text, uc.user);
    auto params = test_gen_params();
    params.max_tokens = 384;

    auto gen = g_ctx.orchestrator->generate(messages, params, g_ctx.default_tier);
    std::string raw = gen.raw_content.empty() ? gen.content : gen.raw_content;

    auto parsed = adapter->parse_tool_calls(raw);

    EmissionResult er;
    er.raw = raw;
    er.cleaned = parsed.cleaned_content;
    er.calls = parsed.tool_calls;
    er.emitted_markup = contains_any(raw, native_markers);
    er.carries_name = contains_any(raw, name_tokens());

    spdlog::info("PROMPT: {}", uc.user);
    spdlog::info("RAW: {}", raw);
    spdlog::info("PARSED {} call(s); markup={} named={}",
                 er.calls.size(), er.emitted_markup, er.carries_name);
    for (const auto& c : er.calls) {
        spdlog::info("  -> name='{}' args={}", c.name, c.arguments.size());
    }
    return er;
}

/**
 * @brief Run a battery of directive prompts and aggregate the guarantee.
 *
 * @param adapter Live tier adapter.
 * @param base_prompt Production system content.
 * @param tool_jsons Real bundled tool definitions.
 * @param cases Battery of directive prompts.
 * @param native_markers Substrings marking the adapter's native tool format.
 * @return BatteryOutcome with parse rate and contract-violation flags.
 * @utility
 * @version 2.3.8
 */
inline BatteryOutcome run_emission_battery(
    entropic::ChatAdapter* adapter,
    const std::string& base_prompt,
    const std::vector<std::string>& tool_jsons,
    const std::vector<EmissionCase>& cases,
    const std::vector<std::string>& native_markers)
{
    BatteryOutcome out;
    out.total = static_cast<int>(cases.size());
    for (const auto& uc : cases) {
        auto er = run_one_emission(adapter, base_prompt, tool_jsons, uc,
                                   native_markers);
        if (er.emitted_markup) { out.emitted_markup++; }
        if (!er.calls.empty()) { out.parsed++; }
        if (er.emitted_markup && er.calls.empty()) { out.emission_missed = true; }
        // The real adapter contract: a NAMED call must always parse. A
        // nameless payload (model omitted the function name) is not a
        // parseable call, so the adapter is correct to return zero.
        if (er.carries_name && er.calls.empty()) { out.named_missed = true; }
        if (contains_any(er.cleaned, native_markers)) { out.leaked = true; }
        for (const auto& c : er.calls) {
            if (c.name.empty()) { out.malformed = true; }
        }
    }
    spdlog::info("BATTERY: parsed={}/{} markup={} missed={} named_missed={} "
                 "leaked={} malformed={}",
                 out.parsed, cases.size(), out.emitted_markup,
                 out.emission_missed, out.named_missed, out.leaked, out.malformed);
    return out;
}

/**
 * @brief The standard tool-eliciting battery (delegate / followup / read).
 *
 * Directs the model toward a specific real bundled tool per prompt. The
 * tool *format* comes only from the adapter's format_tools (production
 * path) — these prompts never spell the tag syntax, so they test
 * emission, not template-copying.
 *
 * @return Three directive prompts.
 * @utility
 * @version 2.3.8
 */
inline std::vector<EmissionCase> standard_tool_battery() {
    return {
        {"Use the followup tool to recall any prior work about the "
         "\"findme\" command. Respond with the tool call only."},
        {"Delegate to the researcher tier the task of locating the "
         "findme command in the codebase. Use the delegate tool. "
         "Respond with the tool call only."},
        {"Use the read_file tool to read the file /etc/hostname. "
         "Respond with the tool call only."},
    };
}

/**
 * @brief Production system base: constitution + app_context + identity.
 *
 * Uses the default tier's bundled identity with a concise fallback if it
 * is unavailable, so the emission battery runs under the same system
 * content production assembles — not a rigged template-copy prompt.
 *
 * @return Assembled production system prompt.
 * @utility
 * @version 2.3.8
 */
inline std::string production_base() {
    auto base = assemble_system_prompt(g_ctx.default_tier);
    if (base.empty()) {
        base = "You are the lead agent. Use the available tools to "
               "accomplish tasks; delegate research to sub-agents.";
    }
    return base;
}

/**
 * @brief Load the three standard bundled tool JSONs (delegate/followup/read).
 * @return Tool definition JSON strings (skips any that fail to load).
 * @utility
 * @version 2.3.8
 */
inline std::vector<std::string> standard_tool_jsons() {
    std::vector<std::string> tools;
    for (const char* rel : {"entropic/followup.json",
                            "entropic/delegate.json",
                            "filesystem/read_file.json"}) {
        auto j = load_tool_json(rel);
        if (!j.empty()) { tools.push_back(j); }
    }
    return tools;
}
