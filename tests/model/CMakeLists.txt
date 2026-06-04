# SPDX-License-Identifier: Apache-2.0
# v1.10.2: C++ model test suite — BDD behavioral validation with live model.
#
# 29 individual executables, one per test. Each loads its own model
# independently — no shared state, no cross-test corruption.
#
# NOT a pre-commit hook — manual developer run.
#
# Gate: ENTROPIC_BUILD_MODEL_TESTS=OFF (default)
# Enable: cmake --preset dev -DENTROPIC_BUILD_MODEL_TESTS=ON
#
# Run:  ctest -L model
# Reports: test-reports/model/

if(NOT ENTROPIC_BUILD_MODEL_TESTS)
    return()
endif()

# ── Common link libraries ───────────────────────────────────

set(MODEL_TEST_LIBS
    entropic-facade
    entropic::inference
    entropic-core
    entropic-types
    entropic-mcp
    entropic-config
    entropic-prompts
    entropic-storage
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    Catch2::Catch2WithMain
)

set(MODEL_TEST_DEFS
    # Model-test artifacts land under build/test-reports/ (gitignored)
    # consistent with tasks.py:MODEL_RESULTS_FILE.
    TEST_REPORTS_DIR="${PROJECT_SOURCE_DIR}/build/test-reports/model"
    MODEL_PATH="${PROJECT_SOURCE_DIR}"
)

# ── Helper: add one model test executable ───────────────────

function(add_model_test NAME SOURCE)
    add_executable(${NAME} ${SOURCE})
    target_link_libraries(${NAME} PRIVATE ${MODEL_TEST_LIBS})
    target_compile_definitions(${NAME} PRIVATE ${MODEL_TEST_DEFS})
    add_test(NAME model/${NAME} COMMAND ${NAME})
    set_tests_properties(model/${NAME} PROPERTIES
        LABELS "model"
        RUN_SERIAL TRUE
        TIMEOUT 120
    )
endfunction()

# ── Subsystem tests (18) — orchestrator-level ───────────────

add_model_test(test-basic-generation      test_basic_generation.cpp)
add_model_test(test-tool-call-parsing     test_tool_call_parsing.cpp)
add_model_test(test-tier-routing          test_tier_routing.cpp)
add_model_test(test-delegation            test_delegation.cpp)
add_model_test(test-grammar-constraint    test_grammar_constraint.cpp)
add_model_test(test-context-retention     test_context_retention.cpp)
add_model_test(test-logprob-evaluation    test_logprob_evaluation.cpp)
add_model_test(test-dynamic-identity      test_dynamic_identity.cpp)
add_model_test(test-gpu-profile           test_gpu_profile.cpp)
add_model_test(test-mcp-auth-denial       test_mcp_auth_denial.cpp)
add_model_test(test-external-mcp          test_external_mcp.cpp)
add_model_test(test-session-persistence   test_session_persistence.cpp)
add_model_test(test-audit-log             test_audit_log.cpp)
add_model_test(test-custom-compactor      test_e10_custom_compactor.cpp)
add_model_test(test-backend-capabilities  test_backend_capabilities.cpp)
add_model_test(test-adapter-lifecycle     test_adapter_lifecycle.cpp)
add_model_test(test-constitutional-validation test_constitutional_validation.cpp)
add_model_test(test-multimodal-fallback   test_multimodal_fallback.cpp)
add_model_test(test-context-bleed         test_delegation_context_bleed.cpp)

# ── v2.1.9 new family tests (gh#44/#45/#46/#47) ─────────────
# Each test overrides the default tier to load a v2.1.9 bundled GGUF
# (Qwen 3.6, Gemma 4, Nemotron 3) and validates the corresponding
# adapter end-to-end. SKIP cleanly if the GGUF isn't present;
# developer runs `entropic download <key>` first.
add_model_test(test-v219-qwen36       test_v219_qwen36_family.cpp)
add_model_test(test-v219-gemma4-e2b   test_v219_gemma4_family.cpp)
add_model_test(test-v219-gemma4-e4b   test_v219_gemma4_e4b_family.cpp)
# v2.3.8 (gh#62): cross-quant tool-call consistency for E4B at UD-Q4_K_XL.
# Same adapter + battery + assertions as the Q8_0 sibling above; the
# different binary makes a quant regression visible in the release gate.
add_model_test(test-v219-gemma4-e4b-q4 test_v219_gemma4_e4b_q4_family.cpp)
add_model_test(test-v219-gemma4-a4b   test_v219_gemma4_a4b_family.cpp)
add_model_test(test-v219-nemotron3    test_v219_nemotron3_family.cpp)

# ── v2.1.11 speculative-decoding tests (gh#36) ───────────────
# Both load Qwen3.6-A3B (target, GPU) + Qwen3.5-0.8B (draft, CPU).
# Correctness asserts bit-identical output vs plain decode; speedup
# asserts ≥1.8× tokens/sec gain on a >500-token generation. SKIP
# cleanly if the draft GGUF is missing.
add_model_test(test-speculative-correctness  test_speculative_correctness.cpp)
add_model_test(test-speculative-speedup      test_speculative_speedup.cpp)
set_tests_properties(model/test-speculative-speedup PROPERTIES TIMEOUT 360)

# Pass mock server binary path for external MCP test
if(TARGET mock-mcp-server)
    target_compile_definitions(test-external-mcp PRIVATE
        MOCK_MCP_SERVER_PATH="$<TARGET_FILE:mock-mcp-server>")
    add_dependencies(test-external-mcp mock-mcp-server)
endif()

# test-constitutional-validation baseline is ~68s; under parallel load
# it can brush the default 120s ctest timeout. Bump to give it headroom.
set_tests_properties(model/test-constitutional-validation PROPERTIES
    TIMEOUT 240)

# ── Engine-loop tests (11) — full AgentEngine::run() ────────

add_model_test(test-e0-true-e2e           test_e0_true_e2e.cpp)
add_model_test(test-e1-single-turn        test_e1_single_turn.cpp)
add_model_test(test-e2-tool-call          test_e2_tool_call.cpp)
add_model_test(test-e3-routing            test_e3_routing.cpp)
add_model_test(test-e4-multi-turn         test_e4_multi_turn.cpp)
add_model_test(test-e5-compaction         test_e5_compaction.cpp)
add_model_test(test-e6-state-transitions  test_e6_state_transitions.cpp)
add_model_test(test-e7-delegation         test_e7_delegation.cpp)
add_model_test(test-e8-mcp-auth           test_e8_mcp_auth.cpp)
add_model_test(test-hook-lifecycle        test_hook_lifecycle.cpp)
add_model_test(test-e9-dynamic-identity   test_e9_dynamic_identity.cpp)

# v2.3.10: coverage-only backend method smoke. Exercises every public
# LlamaCppBackend entry-point with a loaded model — structural asserts
# only. Designed to push librentropic-inference coverage past the 70%
# threshold without depending on small-model intelligence.
add_model_test(test-v2310-backend-coverage-smoke
               test_v2310_backend_coverage_smoke.cpp)

# v2.4.0: minor-release ceremony — closes the model-test scope gaps
# from the v2.3.11–v2.3.27 patch stack. Sampler knobs (presence /
# frequency / logit_bias) end-to-end through real complete() calls;
# ModelConfig knob smoke; state save/load round-trip (the v2.3.25
# release notes flagged this as deferred to model-test scope).
add_model_test(test-v240-minor-ceremony
               test_v240_minor_ceremony.cpp)

# gh#87 increment-1 gate (v2.7.0): real-flow common_chat render→generate
# →parse on gemma4. Needs the llama.cpp common/ include dir (private to
# entropic-inference).
add_model_test(test-gh87-common-chat-e2e test_gh87_common_chat_e2e.cpp)
target_include_directories(test-gh87-common-chat-e2e PRIVATE
    ${PROJECT_SOURCE_DIR}/extern/llama.cpp/common)

# gh#87 increment-2 gate (v2.7.0): drives the SAME triple through the
# backend's additive public API (set_active_tools / render_with_tools /
# parse_response). Uses entropic types + the backend header only — no
# direct llama.cpp common/ include needed.
add_model_test(test-gh87-backend-common-chat test_gh87_backend_common_chat.cpp)

# gh#87 increment-3a gate (v2.7.0): the standard generate() entry point
# routes through the common_chat path (render_prompt seam) when tools are
# staged. Combined call site: set_active_tools → generate →
# has_common_chat_params → parse_response.
add_model_test(test-gh87-backend-generate-tools
               test_gh87_backend_generate_tools.cpp)

# gh#87 Phase A (v2.7.0): per-family verification that common_chat parses
# each family's native tool-call format — the precondition for retiring the
# hand-rolled adapters. Shared logic in gh87_verify_helpers.h; one thin
# executable per family (one model load each, per the reload-SEGV constraint).
add_model_test(test-gh87-verify-qwen35    test_gh87_verify_qwen35.cpp)
add_model_test(test-gh87-verify-qwen36    test_gh87_verify_qwen36.cpp)
add_model_test(test-gh87-verify-nemotron3 test_gh87_verify_nemotron3.cpp)
# qwen36 is a 13GB A3B MoE under partial offload — give it headroom.
set_tests_properties(model/test-gh87-verify-qwen36 PROPERTIES TIMEOUT 360)

# gh#87 Phase B (v2.7.0): live cutover at the orchestrator level —
# params.tools → stage_active_tools → common_chat render → parse_response →
# result.tool_calls (the agent-loop production path).
add_model_test(test-gh87-orchestrator-cutover
               test_gh87_orchestrator_cutover.cpp)

# gh#87 diagnostic (v2.7.0): multi-parameter tool-call parse through
# common_chat on the default model. Dumps rendered prompt + raw + parse.
add_model_test(test-gh87-multiparam-diag test_gh87_multiparam_diag.cpp)
set_tests_properties(model/test-gh87-multiparam-diag PROPERTIES TIMEOUT 360)

# gh#88 (v2.7.1): EMERGENT multi-turn regression — a real gemma session must
# not accumulate parrotable {action:...} meta-tool result envelopes (Fix 1
# de-fang) and must recover a parroted emission (Fix 2). The class of bug
# that single-turn coverage structurally cannot catch (see test header).
# Multi-turn loop over gemma-4 E4B (~4.5GB, full offload) — modest ceiling.
add_model_test(test-gh88-envelope-priming test_gh88_envelope_priming.cpp)
set_tests_properties(model/test-gh88-envelope-priming PROPERTIES TIMEOUT 300)

# gh#90 (v2.7.2): gemma <|"|> string-escape must preserve string typing for
# numeric-looking string args (coerce_string_typed_args). Deterministic
# parse-path test on a real gemma render.
add_model_test(test-gh90-string-typing test_gh90_string_typing.cpp)
set_tests_properties(model/test-gh90-string-typing PROPERTIES TIMEOUT 300)

# gh#95 (v2.7.4): facade-driven — identity grammar: field must constrain output.
add_model_test(test-gh95-identity-grammar test_gh95_identity_grammar.cpp)
set_tests_properties(model/test-gh95-identity-grammar PROPERTIES TIMEOUT 300)

add_model_test(test-gh96-warmkeep test_gh96_warmkeep.cpp)
set_tests_properties(model/test-gh96-warmkeep PROPERTIES TIMEOUT 300)

add_model_test(test-gh96-warmkeep-oracle test_gh96_warmkeep_oracle.cpp)
set_tests_properties(model/test-gh96-warmkeep-oracle PROPERTIES TIMEOUT 600)

# gh#87 (v2.7.0): generous timeout for the 13GB A3B/A4B tests.
#
# The 120s default (add_model_test) was calibrated for FULL-GPU offload. The
# Qwen3.6-35B-A3B and Gemma-4-26B-A4B GGUFs (~13GB) live on the dev box's HDD
# and run at PARTIAL CPU offload (gpu_layers=20) on an 11GB GPU — far slower
# than full-GPU, and a cold-cache HDD read of ~13GB compounds it (warm: ~6s;
# cold-in-suite: well past 120s). These partial-offload runs validate
# CORRECTNESS, not performance (you cannot estimate model throughput from a
# partial-offload run). The real hazard: a 120s SIGKILL fires MID-CUDA, leaves
# the GPU context corrupted, and the next heavy GPU test (the speculative
# pair) then faults and takes down the terminal. A generous ceiling lets these
# exit cleanly; a genuine hang (>600s) still trips it.
set_tests_properties(
    model/test-v219-gemma4-a4b
    model/test-v219-qwen36
    model/test-gh87-common-chat-e2e
    model/test-gh87-backend-common-chat
    model/test-gh87-backend-generate-tools
    model/test-gh87-orchestrator-cutover
    model/test-gh87-verify-qwen36
    PROPERTIES TIMEOUT 600)
