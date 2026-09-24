// SPDX-License-Identifier: Apache-2.0
/**
 * @file device_memory.h
 * @brief gh#142: ask the GPU how much VRAM is actually free.
 *
 * @par Why this exists
 * `orchestrator.h` documented the VRAM budget as resolving
 * "`ENTROPIC_VRAM_BUDGET_BYTES` -> CUDA `cudaMemGetInfo` -> 0", but only the
 * env var was ever implemented — the header promised a capability the engine
 * did not have. With no variable set the budget was 0, which the gate reads as
 * "unknown, do not enforce", so every default deployment ran with the admission
 * gate disabled and a too-large tier aborted the process inside llama.cpp
 * instead of being refused.
 *
 * This closes that gap through ggml's own device abstraction rather than the
 * CUDA runtime directly, so it reports honestly for whichever backend variant
 * was compiled in and needs no CUDA-specific build plumbing.
 *
 * @par Free, not total
 * The query returns FREE VRAM, which is the number that matters: gh#142's
 * reporter hit this on a GPU busier than the developer's, and a budget derived
 * from total capacity would have admitted the load and aborted anyway. It is
 * sampled once at `initialize()`, so it reflects the device as the engine found
 * it — a later consumer of VRAM can still cause a failure this gate cannot see.
 *
 * @version 2.11.0
 */

#pragma once

#include <cstdint>

namespace entropic {

/**
 * @brief Free VRAM on the first GPU device ggml reports, in bytes.
 *
 * @return Free VRAM bytes, or 0 when there is no GPU device or the build has no
 *         GPU backend — 0 means "unknown" and leaves the budget gate disabled.
 * @req REQ-INFER-019
 * @version 2.11.0
 */
uint64_t query_device_free_vram_bytes();

/**
 * @brief This process's RLIMIT_MEMLOCK, in bytes (gh#148).
 *
 * The ceiling on what `use_mlock` can pin. Reported so the admission gate
 * can REFUSE a configuration that would lock more than the limit allows
 * rather than discovering it as an OOM kill — pinned pages cannot be
 * reclaimed under pressure, which is what turned partial offload of a
 * 13 GB model from slow into fatal (v2.12.0).
 *
 * @return The soft limit in bytes, or `kMemlockUnlimited` when unlimited or
 *         unreadable — both mean "this gate has nothing to say".
 * @req REQ-INFER-019
 * @version 2.13.0
 */
uint64_t host_memlock_limit_bytes();

}  // namespace entropic
