// SPDX-License-Identifier: Apache-2.0
/**
 * @file delegate_hook_payload.h
 * @brief Testable free function extracted from fire_delegate_complete_hook.
 *
 * fire_delegate_complete_hook builds a JSON payload and calls j.dump() before
 * dispatching to hooks_.fire_post. The dump is the only site that can throw
 * nlohmann::json::type_error.316 — if summary contains invalid UTF-8 the
 * exception propagates out of the private method and up through run_loop,
 * which is the gh#113 crash mode.
 *
 * Extracting the build+dump step here lets unit tests drive it directly with
 * a bad-UTF8 summary (simulating storage-loaded context, gh#112 interaction)
 * and confirm RED on unfixed code, GREEN on the v2.9.9 sink-guard fix.
 *
 * @internal Not part of the public API.
 * @version 2.9.9
 */

#pragma once

#include <string>

namespace entropic::detail {

/**
 * @brief Build and dump the ON_DELEGATE_COMPLETE hook JSON payload.
 *
 * Mirrors the JSON structure produced by fire_delegate_complete_hook:
 *   {"target_tier":"<target>","success":<bool>,"result_kind":"ok"|"delegation_failed","summary":"<summary>"}
 *
 * @param target  Target tier name (e.g. "researcher").
 * @param success Whether delegation succeeded.
 * @param summary Delegation summary string — MAY contain invalid UTF-8 when
 *                sourced from storage-loaded context (gh#112 interaction).
 *                The caller is responsible for sanitizing before passing here,
 *                OR this function sanitizes internally (v2.9.9 fix).
 * @return Compact JSON string.
 * @throws nlohmann::json::type_error (316) if summary contains invalid UTF-8
 *         and no sanitization is applied. The v2.9.9 fix eliminates this.
 * @version 2.9.9
 */
std::string build_delegate_complete_json(
    const std::string& target,
    bool success,
    const std::string& summary);

} // namespace entropic::detail
