// SPDX-License-Identifier: LGPL-3.0-or-later
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

/**
 * @brief Build an InferenceInterface wired to an orchestrator.
 *
 * The returned interface's function pointers delegate to the
 * orchestrator for generation, routing, tool parsing, and
 * response completeness checks. The interface is valid for the
 * lifetime of the orchestrator.
 *
 * @param orchestrator Orchestrator to wire (must outlive the interface).
 * @param default_tier Default tier name for generation/routing.
 * @return Fully wired InferenceInterface.
 * @version 2.0.1
 */
InferenceInterface build_orchestrator_interface(
    ModelOrchestrator* orchestrator,
    const std::string& default_tier);

} // namespace entropic
