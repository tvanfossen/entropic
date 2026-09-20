// SPDX-License-Identifier: Apache-2.0
/**
 * @file run_scope.cpp
 * @brief gh#158: per-thread publication of the running turn's cancel token.
 *
 * The thread-local lives in ONE translation unit and is reached only through
 * the exported accessors below — the same shape as `t_current_handle_id` in
 * logging.cpp, and for the same reason: `entropic-types` is an OBJECT library
 * whose objects are embedded in several `.so` files, so a thread-local defined
 * in a header would be a DIFFERENT variable per library. Confining it here and
 * exporting the accessors with default visibility leaves the dynamic linker
 * one definition to resolve against.
 *
 * @version 2.13.0
 */

#include <entropic/types/run_scope.h>

namespace entropic {

namespace {

/// @brief Cancel token of the run executing on this thread (gh#158).
thread_local std::atomic<bool>* t_run_cancel = nullptr;

}  // namespace

/**
 * @brief gh#158: this thread's run cancel token — see header.
 * @return Pointer to the run's cancel flag, or nullptr outside a run.
 * @req REQ-LOOP-006
 * @version 2.13.0
 */
const std::atomic<bool>* current_run_cancel() { return t_run_cancel; }

/**
 * @brief gh#158: has this thread's run been cancelled — see header.
 * @return true when a run scope is active AND its flag is set.
 * @req REQ-LOOP-006
 * @version 2.13.0
 */
bool current_run_cancelled() {
    return t_run_cancel != nullptr
        && t_run_cancel->load(std::memory_order_acquire);
}

/**
 * @brief gh#158 RAII enter — see header.
 * @param token Borrowed cancel flag; nullptr installs "no token".
 * @return n/a (constructor).
 * @req REQ-LOOP-006
 * @version 2.13.0 [reviewed]
 */
RunCancelScope::RunCancelScope(std::atomic<bool>* token)
    : previous_(t_run_cancel) {
    t_run_cancel = token;
}

/**
 * @brief gh#158: cancel this thread's run — see header.
 * @return true when a token was installed and has been set.
 * @req REQ-LOOP-006
 * @version 2.13.0
 */
bool cancel_current_run() {
    if (t_run_cancel == nullptr) {
        return false;
    }
    t_run_cancel->store(true, std::memory_order_release);
    return true;
}

/**
 * @brief gh#158 RAII exit — see header.
 * @return n/a (destructor).
 * @req REQ-LOOP-006
 * @version 2.13.0
 */
RunCancelScope::~RunCancelScope() { t_run_cancel = previous_; }

}  // namespace entropic
