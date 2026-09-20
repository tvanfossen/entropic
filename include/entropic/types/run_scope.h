// SPDX-License-Identifier: Apache-2.0
/**
 * @file run_scope.h
 * @brief gh#158: the cancel token of the run executing on THIS thread.
 *
 * @par Why a thread-local and not a parameter
 * gh#150 gave external MCP transports a `cancel_flag_` that
 * `ServerManager::interrupt_external_tools()` trips on EVERY transport at
 * once. That is correct while a handle runs one turn at a time — the only
 * in-flight tool call belongs to the only run. Once runs are keyed per
 * session (gh#158) it stops being correct: interrupting session A would
 * abort session B's in-flight `filesystem.read` too, and B would see an
 * empty result it cannot distinguish from a legitimate one (the exact
 * failure mode gh#150 was filed for).
 *
 * The honest fix is a cancel token carried by the REQUEST. Threading one
 * through would change `ToolExecutionInterface` (a C function-pointer struct
 * on the core↔facade seam), `ServerManager::call_tool`, `ExternalClient` and
 * `Transport::send_request` — four signatures, three `.so` boundaries, for a
 * value that is already unambiguous from context: a run is a BLOCKING call on
 * the caller's thread, and every tool call it makes happens on that same
 * thread. So the token is published per thread instead of per parameter.
 *
 * This is the same mechanism `entropic::log::HandleLogScope` already uses to
 * route a log line to the right session without threading a handle id through
 * every logging call, and it is deliberately modelled on it.
 *
 * @par Ownership
 * A scope BORROWS the atomic; it never owns it. The engine's per-run state
 * outlives the scope by construction (the scope is nested inside the run that
 * owns the flag), so the pointer cannot dangle while it is installed.
 *
 * @version 2.13.0
 */

#pragma once

#include <entropic/entropic_export.h>

#include <atomic>

namespace entropic {

/**
 * @brief The cancel token of the run executing on this thread, if any.
 *
 * @return Pointer to the run's cancel flag, or nullptr when no run scope is
 *         active on this thread (a plain API call, a consumer thread, a test).
 * @req REQ-LOOP-006
 * @version 2.13.0
 */
ENTROPIC_EXPORT const std::atomic<bool>* current_run_cancel();

/**
 * @brief Whether THIS thread's run has been cancelled.
 *
 * The form every poll site wants: a transport or a decode loop asks "should I
 * stop?", not "what is my token?". Returns false when no scope is installed,
 * so a caller outside a run is never spuriously cancelled.
 *
 * @return true when a run scope is active on this thread AND its flag is set.
 * @req REQ-LOOP-006
 * @version 2.13.0
 */
ENTROPIC_EXPORT bool current_run_cancelled();

/**
 * @brief RAII publication of one run's cancel token to its own thread.
 *
 * Restores the previous value on destruction rather than clearing it, so a
 * nested scope (a child delegation loop inside a parent turn) cannot orphan
 * the parent's token when it unwinds.
 *
 * @return An RAII guard binding this thread to `token` until it goes out of
 *         scope, at which point the previous token is restored.
 * @req REQ-LOOP-006
 * @version 2.13.0
 */
class ENTROPIC_EXPORT RunCancelScope {
public:
    /**
     * @brief Publish `token` as this thread's run cancel token.
     * @param token Borrowed cancel flag; nullptr installs "no token".
     * @return n/a (constructor).
     * @req REQ-LOOP-006
     * @version 2.13.0
     */
    explicit RunCancelScope(const std::atomic<bool>* token);

    /**
     * @brief Restore the previously installed token.
     * @return n/a (destructor).
     * @req REQ-LOOP-006
     * @version 2.13.0
     */
    ~RunCancelScope();

    RunCancelScope(const RunCancelScope&) = delete;
    RunCancelScope& operator=(const RunCancelScope&) = delete;

private:
    const std::atomic<bool>* previous_;
};

}  // namespace entropic
