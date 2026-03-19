/**
 * @file enums.h
 * @brief Shared enumerations used across .so boundaries.
 *
 * These are C enums for cross-boundary use. C++ code within a single .so
 * may use typed enums, but these specific values are the wire format.
 *
 * @version 1.8.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Model VRAM lifecycle states.
 *
 * @code
 * COLD ──warm()──> WARM ──activate()──> ACTIVE
 *   ^               |                     |
 *   └──unload()─────┘<──deactivate()──────┘
 * @endcode
 */
typedef enum {
    ENTROPIC_MODEL_STATE_COLD = 0, ///< On disk only, no RAM consumed
    ENTROPIC_MODEL_STATE_WARM,     ///< mmap'd + mlock'd in RAM, slow inference
    ENTROPIC_MODEL_STATE_ACTIVE,   ///< GPU layers loaded, full inference speed
} entropic_model_state_t;

/**
 * @brief Agent execution states.
 */
typedef enum {
    ENTROPIC_AGENT_STATE_IDLE = 0,     ///< No active generation
    ENTROPIC_AGENT_STATE_PLANNING,     ///< Assembling context and tools
    ENTROPIC_AGENT_STATE_EXECUTING,    ///< Generating response
    ENTROPIC_AGENT_STATE_WAITING_TOOL, ///< Tool call in progress
    ENTROPIC_AGENT_STATE_VERIFYING,    ///< Post-generation verification
    ENTROPIC_AGENT_STATE_DELEGATING,   ///< Child delegation running
    ENTROPIC_AGENT_STATE_COMPLETE,     ///< Turn finished
    ENTROPIC_AGENT_STATE_ERROR,        ///< Unrecoverable error
    ENTROPIC_AGENT_STATE_INTERRUPTED,  ///< Cancelled by consumer
    ENTROPIC_AGENT_STATE_PAUSED,       ///< Awaiting user input
} entropic_agent_state_t;

/**
 * @brief Directive types emitted by MCP tool results.
 */
typedef enum {
    ENTROPIC_DIRECTIVE_DELEGATE = 0,    ///< Route to another identity
    ENTROPIC_DIRECTIVE_PIPELINE,        ///< Multi-stage sequential execution
    ENTROPIC_DIRECTIVE_COMPLETE,        ///< Mark task complete
    ENTROPIC_DIRECTIVE_STOP_PROCESSING, ///< Halt directive processing
    ENTROPIC_DIRECTIVE_CONTEXT_ANCHOR,  ///< Replace context anchor
} entropic_directive_type_t;

/**
 * @brief Compute backend types.
 */
typedef enum {
    ENTROPIC_BACKEND_CPU = 0,   ///< CPU-only inference
    ENTROPIC_BACKEND_CUDA,      ///< NVIDIA CUDA
    ENTROPIC_BACKEND_VULKAN,    ///< Vulkan compute
} entropic_compute_backend_t;

#ifdef __cplusplus
}
#endif
