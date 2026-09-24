// SPDX-License-Identifier: Apache-2.0
/**
 * @file model_skip_reason.h
 * @brief gh#149 (v2.13.0): why a model test skipped, stated once, truthfully.
 *
 * `init_orchestrator_for_v219_family` declines to run for FIVE distinct
 * reasons — a registry key that resolves to nothing, a GGUF that is not on
 * disk, an operator waiver, a host that cannot hold the WARM load, and a load
 * that failed with the file right there — and the only thing it handed back
 * was a bool. Every SCENARIO then re-derived a reason it could not know and
 * printed the same literal: "<key> GGUF not present — run `entropic download
 * <key>`". For three of those five causes the model WAS on disk, so the
 * release record asserted something nothing had verified and sent the reader
 * after an action that changes nothing. That is architecture decision #57's
 * shape: a documented-but-false contract.
 *
 * The fix is to carry the reason from the point of decision rather than guess
 * at it downstream. This header holds the mapping and nothing else: no
 * orchestrator, no filesystem, no registry — just cause plus observed facts to
 * text, which is why it can be pinned by a CPU unit test with no GGUF
 * (tests/unit/inference/model_skip_reason_test.cpp).
 *
 * Wording bar is `gh87_verify_helpers.h`, which already named its real
 * condition: say which rule fired, carry the measured numbers that decided it,
 * and never suggest a remedy that would not change the outcome.
 *
 * @version 2.13.0
 */

#pragma once

#include <cstdint>
#include <string>

namespace entropic::test {

/**
 * @brief Why a model test's setup did not complete.
 *
 * One value per decision point that returns false. `kNone` means setup
 * succeeded and there is no reason to state — the accessor falls back to its
 * caller's default text, so adding this enum cannot regress a passing run.
 *
 * @version 2.13.0
 */
enum class SkipCause {
    kNone,                    ///< Setup succeeded; no skip.
    kHarnessSetupFailed,      ///< Registry/config load failed before the model.
    kRegistryKeyMissing,      ///< Key absent from data/bundled_models.yaml.
    kGgufMissing,             ///< Key resolves, file is not on disk.
    kLargeModelWaived,        ///< ENTROPIC_SKIP_LARGE_MODEL_TESTS=1.
    kHostRamInsufficient,     ///< MemAvailable cannot hold the WARM load.
    kOrchestratorInitFailed,  ///< File present; the load itself failed.
};

/**
 * @brief What was actually observed where the decision was made.
 *
 * Every field is optional in the sense that a cause uses only the ones it
 * needs; the point is that the numbers travel WITH the cause, so the text can
 * quote them instead of a later site inventing a plausible-sounding reason.
 *
 * @version 2.13.0
 */
struct SkipFacts {
    std::string key;               ///< Registry key, e.g. "qwen3_6_a3b".
    std::string path;              ///< Resolved GGUF path.
    uint64_t file_bytes = 0;       ///< GGUF size on disk.
    uint64_t available_bytes = 0;  ///< MemAvailable at the decision.
    uint64_t needed_bytes = 0;     ///< File plus headroom.
    std::string detail;            ///< Upstream error text, when there is one.
};

/**
 * @brief Render a byte count as whole MiB, matching the harness's warn lines.
 * @param bytes Byte count.
 * @return Decimal MiB, no unit suffix.
 * @utility
 * @version 2.13.0
 */
inline std::string mib(uint64_t bytes) {
    return std::to_string(bytes / (1024ull * 1024));
}

/**
 * @brief The registry never resolved the key — nothing to download.
 * @param f Observed facts.
 * @return Reason text.
 * @utility
 * @version 2.13.0
 */
inline std::string reason_registry_key_missing(const SkipFacts& f) {
    return "'" + f.key + "' is not a key in data/bundled_models.yaml, so the "
           "harness never resolved a model path. Registry defect — fix the "
           "key or the catalog entry.";
}

/**
 * @brief The key resolved and the file genuinely is not there.
 * @param f Observed facts.
 * @return Reason text (the original wording, now true by construction).
 * @utility
 * @version 2.13.0
 */
inline std::string reason_gguf_missing(const SkipFacts& f) {
    return f.key + " GGUF not present at " + f.path
           + " — run `entropic download " + f.key + "`";
}

/**
 * @brief An operator explicitly waived this large model's test.
 * @param f Observed facts.
 * @return Reason text.
 * @utility
 * @version 2.13.0
 */
inline std::string reason_large_model_waived(const SkipFacts& f) {
    return f.key + " skipped by operator allowance: "
           "ENTROPIC_SKIP_LARGE_MODEL_TESTS=1 waives models over 10 GiB and "
           "this GGUF is " + mib(f.file_bytes) + " MiB. The model IS on disk "
           "at " + f.path + " — fetching it again changes nothing. Unset the "
           "variable to run this test.";
}

/**
 * @brief The host could not hold the WARM load, which maps the whole file.
 * @param f Observed facts.
 * @return Reason text.
 * @utility
 * @version 2.13.0
 */
inline std::string reason_host_ram_insufficient(const SkipFacts& f) {
    return f.key + " skipped: the WARM load maps the whole "
           + mib(f.file_bytes) + " MiB GGUF into host RAM regardless of "
           "gpu_layers, and only " + mib(f.available_bytes)
           + " MiB was available (needs ~" + mib(f.needed_bytes)
           + " MiB with headroom). The model IS on disk at " + f.path
           + ". Hardware limit, not a defect — a quantised model of this size "
           "runs fine on a host with the RAM free.";
}

/**
 * @brief The file was there; loading it failed.
 * @param f Observed facts.
 * @return Reason text.
 * @utility
 * @version 2.13.0
 */
inline std::string reason_orchestrator_init_failed(const SkipFacts& f) {
    std::string out = f.key + " GGUF IS on disk at " + f.path
                      + " but the orchestrator failed to load it. This is a "
                        "load failure, not a missing model";
    if (!f.detail.empty()) { out += ": " + f.detail; }
    return out + ". See the per-test log for the backend error.";
}

/**
 * @brief Registry or config resolution failed before any model was reached.
 * @param f Observed facts.
 * @return Reason text.
 * @utility
 * @version 2.13.0
 */
inline std::string reason_harness_setup_failed(const SkipFacts& f) {
    std::string out = "model-test harness setup failed before '" + f.key
                      + "' was reached — bundled_models.yaml or config "
                        "resolution, not the model";
    if (!f.detail.empty()) { out += ": " + f.detail; }
    return out + ".";
}

/**
 * @brief Map a cause plus its facts to the text a SKIP should carry.
 *
 * Single `return` by construction: the dispatch assigns, it does not exit, so
 * adding a cause cannot quietly bypass the mapping.
 *
 * @param cause Which rule fired.
 * @param f Observations recorded where it fired.
 * @return Reason text, or "" for SkipCause::kNone.
 * @utility
 * @version 2.13.0
 */
inline std::string skip_reason_text(SkipCause cause, const SkipFacts& f) {
    std::string out;
    switch (cause) {
        case SkipCause::kHarnessSetupFailed:
            out = reason_harness_setup_failed(f);
            break;
        case SkipCause::kRegistryKeyMissing:
            out = reason_registry_key_missing(f);
            break;
        case SkipCause::kGgufMissing:
            out = reason_gguf_missing(f);
            break;
        case SkipCause::kLargeModelWaived:
            out = reason_large_model_waived(f);
            break;
        case SkipCause::kHostRamInsufficient:
            out = reason_host_ram_insufficient(f);
            break;
        case SkipCause::kOrchestratorInitFailed:
            out = reason_orchestrator_init_failed(f);
            break;
        case SkipCause::kNone:
            break;
    }
    return out;
}

}  // namespace entropic::test
