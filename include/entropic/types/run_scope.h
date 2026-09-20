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
#include <string>

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
 * @brief Cancel the run executing on this thread (gh#158).
 *
 * The write counterpart of `current_run_cancelled()`, for the one site that
 * needs to stop a run FROM INSIDE it: the pause prompt, when the consumer
 * declines to inject anything, means "abandon this turn". Pre-2.13.0 that
 * site raised `AgentEngine::interrupt_flag_` — the handle-wide flag — which
 * with keyed runs aborts every other session's turn as well.
 *
 * @return true when a run scope was installed on this thread and its token
 *         was set; false when there is none, in which case the caller must
 *         fall back to whatever handle-wide flag it owns.
 * @req REQ-LOOP-006
 * @version 2.13.0
 */
ENTROPIC_EXPORT bool cancel_current_run();

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
 * @version 2.13.0 [reviewed]
 */
class ENTROPIC_EXPORT RunCancelScope {
public:
    /**
     * @brief Publish `token` as this thread's run cancel token.
     *
     * Non-const since gh#158's audit pass: `cancel_current_run()` stops the
     * run from inside it, which is a write.
     *
     * @param token Borrowed cancel flag; nullptr installs "no token".
     * @return n/a (constructor).
     * @req REQ-LOOP-006
     * @version 2.13.0 [reviewed]
     */
    explicit RunCancelScope(std::atomic<bool>* token);

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
    std::atomic<bool>* previous_;
};

/**
 * @brief The session key of the run executing on this thread (gh#166).
 *
 * Same mechanism and same justification as the cancel token above: a run
 * is a blocking call on the caller's thread, and everything it does —
 * including asking the facade "which tools does this turn see?" — happens
 * on that thread. gh#166 binds a session to a WORKSPACE, so the tool root
 * and the tool list are no longer handle-wide constants; the one call
 * that needs them (`get_tool_prompt`, on the inference callback seam) is
 * reached through `i_inference_callbacks.h`, an INTERFACE header that
 * does not change without a proposal. Publishing the key per thread
 * answers the question without touching that contract.
 *
 * @return The running turn's session key, or "" outside a keyed run.
 * @req REQ-LOOP-009
 * @version 2.13.0
 */
ENTROPIC_EXPORT std::string current_run_session();

/**
 * @brief RAII publication of one run's session key to its own thread.
 *
 * Restores the previous value on destruction rather than clearing it, so
 * a nested scope cannot orphan an outer turn's key.
 *
 * @return An RAII guard binding this thread to `key` until it goes out
 *         of scope.
 * @req REQ-LOOP-009
 * @version 2.13.0
 */
class ENTROPIC_EXPORT RunSessionScope {
public:
    /**
     * @brief Publish `key` as this thread's run session key.
     * @param key Session key ("" = the default session).
     * @return n/a (constructor).
     * @req REQ-LOOP-009
     * @version 2.13.0
     */
    explicit RunSessionScope(std::string key);

    /**
     * @brief Restore the previously published key.
     * @return n/a (destructor).
     * @req REQ-LOOP-009
     * @version 2.13.0
     */
    ~RunSessionScope();

    RunSessionScope(const RunSessionScope&) = delete;
    RunSessionScope& operator=(const RunSessionScope&) = delete;

private:
    std::string previous_;  ///< Key restored on destruction
};

}  // namespace entropic
