// SPDX-License-Identifier: Apache-2.0
/**
 * @file device_memory.cpp
 * @brief gh#142: ggml-backed free-VRAM query. See device_memory.h.
 * @version 2.11.0
 */

#include "device_memory.h"

#include <entropic/types/logging.h>

#include <ggml-backend.h>

#include "partial_offload.h"   // kMemlockUnlimited

#include <sys/resource.h>

namespace {
auto logger = entropic::log::get("inference.device_memory");
}  // namespace

namespace entropic {

/**
 * @brief Free VRAM on the first GPU device ggml reports, in bytes.
 *
 * Returns 0 rather than guessing when no GPU device is registered — a CPU-only
 * build or a machine with no usable GPU. The caller reads 0 as "budget unknown"
 * and leaves the admission gate disabled, which is the correct behaviour for a
 * CPU tier: there is no VRAM to run out of.
 *
 * @return Free VRAM bytes, or 0 when unknown.
 * @req REQ-INFER-019
 * @version 2.11.0
 */
uint64_t query_device_free_vram_bytes() {
    ggml_backend_dev_t device =
        ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_GPU);
    if (device == nullptr) {
        logger->info("[residency] no GPU device registered — "
                     "VRAM budget unknown, admission gate disabled");
        return 0;
    }
    size_t free_bytes = 0;
    size_t total_bytes = 0;
    ggml_backend_dev_memory(device, &free_bytes, &total_bytes);
    logger->info("[residency] device '{}' reports {} MiB free of {} MiB total",
                 ggml_backend_dev_name(device),
                 free_bytes / (1024 * 1024), total_bytes / (1024 * 1024));
    return static_cast<uint64_t>(free_bytes);
}

/**
 * @brief This process's RLIMIT_MEMLOCK soft limit (gh#148). See header.
 *
 * `RLIM_INFINITY` and an unreadable limit both map to `kMemlockUnlimited`:
 * a gate that cannot measure must not refuse.
 *
 * @return Soft limit in bytes, or kMemlockUnlimited.
 * @req REQ-INFER-019
 * @version 2.13.0
 */
uint64_t host_memlock_limit_bytes() {
    struct rlimit rl {};
    if (getrlimit(RLIMIT_MEMLOCK, &rl) != 0
        || rl.rlim_cur == RLIM_INFINITY) {
        return kMemlockUnlimited;
    }
    return static_cast<uint64_t>(rl.rlim_cur);
}

}  // namespace entropic
