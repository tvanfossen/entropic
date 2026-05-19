// SPDX-License-Identifier: Apache-2.0
/**
 * @file interface_factory.h
 * @brief Factory for building InferenceInterface from a ModelOrchestrator.
 *
 * Bridges the orchestrator's C++ API to the C function pointer interface
 * that AgentEngine consumes. Keeps all JSON marshaling and type conversion
 * inside the inference .so — the facade never touches it.
 *
 * @version 2.0.1
 */

#pragma once

#include <entropic/interfaces/i_inference_callbacks.h>
#include <string>

namespace entropic {

class ModelOrchestrator;
struct InterfaceContext;  // gh#58 follow-up (v2.2.6): per-handle context

/**
 * @brief Build an InferenceInterface wired to an orchestrator.
 *
 * Pre-v2.2.6 this stored its callback user_data in a process-global
 * `s_ctx` static. A second configure call deleted the first handle's
 * context, leaving the first handle's interface pointing to freed
 * memory (gh#58). The context is now owned by the caller: pass a
 * non-null `out_context` and free it with
 * `destroy_orchestrator_interface()` when the interface is no longer
 * needed (typically alongside the orchestrator on engine destroy).
 *
 * @param orchestrator Orchestrator to wire (must outlive the interface).
 * @param default_tier Default tier name for generation/routing.
 * @param out_context Receives the heap-allocated context; caller owns.
 * @return Fully wired InferenceInterface.
 * @version 2.2.6
 */
InferenceInterface build_orchestrator_interface(
    ModelOrchestrator* orchestrator,
    const std::string& default_tier,
    InterfaceContext** out_context);

/**
 * @brief Free a context returned by build_orchestrator_interface().
 * Safe to call on nullptr.
 * @version 2.2.6
 */
void destroy_orchestrator_interface(InterfaceContext* context);

} // namespace entropic
