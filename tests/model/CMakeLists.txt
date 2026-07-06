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

# gh#106 (v2.9.0): observe QAT thinking-model tool-call structure (TEMP/TDD).
add_model_test(test-gh106-qat-thinking     test_gh106_qat_thinking.cpp)

# gh#106 (v2.9.0): target-owned MTP speculative decode — engaged + lossless.
add_model_test(test-gh106-mtp              test_gh106_mtp.cpp)

# gh#106 (v2.9.0): MTP combined call path through orchestrator->generate().
add_model_test(test-gh106-mtp-route        test_gh106_mtp_route.cpp)

# gh#108 (v2.9.1): MTP fail-loud guards (temp/grammar/tools/streaming/n_draft)
# + greedy engagement + emergent multi-turn, on the live backend.
add_model_test(test-gh108-mtp-guards       test_gh108_mtp_guards.cpp)
set_tests_properties(model/test-gh108-mtp-guards PROPERTIES TIMEOUT 360)

# gh#108 (v2.9.3): side-by-side benchmark, E4B Q8 baseline vs the target
# optimized config (E4B Q2-mobile + MTP + flash + q4 KV). Functional-only
# assertions; prints both full responses + timing for human quality judgment.
add_model_test(test-gh108-config-benchmark test_gh108_config_benchmark.cpp)
set_tests_properties(model/test-gh108-config-benchmark PROPERTIES TIMEOUT 360)

# gh#108 (v2.9.3) follow-up: multi-turn tool-calling agentic benchmark,
# E4B Q8 baseline vs E4B Q4_K_XL+MTP+flash+q4KV, through the orchestrator
# route. Loads/runs 2 models x up to 10 orchestrator calls each — generous
# timeout.
add_model_test(test-gh108-agentic-benchmark test_gh108_agentic_benchmark.cpp)
set_tests_properties(model/test-gh108-agentic-benchmark PROPERTIES TIMEOUT 900)

# gh#108 follow-up: CPU-only (gpu_layers=0) feasibility check for an
# embedded-SoC-class deployment (RK3588-style: no CUDA, unified memory).
# CPU decode is slow — generous timeout for 4 configs x ~8 calls each.
add_model_test(test-gh108-cpu-feasibility test_gh108_cpu_feasibility.cpp)
set_tests_properties(model/test-gh108-cpu-feasibility PROPERTIES TIMEOUT 3600)

# gh#106 (v2.9.0): MTP functions on the mobile QAT (TQ2_0) trunks (E2B+E4B) —
# new×new interaction. Two cases in one binary; TQ2_0 is compute-bound on Pascal
# (~3x) and E4B is a 3GB trunk, so allow a generous timeout for both.
add_model_test(test-gh106-mtp-qat-mobile   test_gh106_mtp_qat_mobile.cpp)
set_tests_properties(model/test-gh106-mtp-qat-mobile PROPERTIES TIMEOUT 600)

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

add_model_test(test-gh97-hybrid-cache test_gh97_hybrid_cache.cpp)
set_tests_properties(model/test-gh97-hybrid-cache PROPERTIES TIMEOUT 300)

# gh#99 (v2.8.0): facade-driven per-call tier grammar switching on one model.
add_model_test(test-gh99-run-as test_gh99_run_as.cpp)
set_tests_properties(model/test-gh99-run-as PROPERTIES TIMEOUT 300)

# gh#98 (v2.8.0): same-prefix batch — prefill-once + correctness + hybrid
# serial fallback (the qwen35moe case needs the larger partial-offload timeout).
add_model_test(test-gh98-batch test_gh98_batch.cpp)
set_tests_properties(model/test-gh98-batch PROPERTIES TIMEOUT 600)

# gh#100 (v2.8.0): q8_0 KV-cache validation on Gemma-4 E2B-Q4 + flash_attn —
# per-seq KV ~halved (→ ~2x #98 batch slots) without the E4B-Q4 breakage.
add_model_test(test-gh100-q8-kv test_gh100_q8_kv.cpp)
set_tests_properties(model/test-gh100-q8-kv PROPERTIES TIMEOUT 300)

# gh#93 (v2.8.0): facade-harness behavioral — enable_thinking:false, autoparser
# multi-param, cancel-bridge interrupt.
add_model_test(test-gh93-behavioral test_gh93_behavioral.cpp)
set_tests_properties(model/test-gh93-behavioral PROPERTIES TIMEOUT 300)

# gh#103 (v2.8.2): EMERGENT multi-turn proof that "sequential" tool_call_mode
# hard-stops generation at the first tool call, through the combined orchestrator
# path, on qwen35moe (PEG_NATIVE close marker + hybrid-arch coherence). 13GB
# partial-offload + 3-turn loop → generous timeout.
add_model_test(test-gh103-sequential-stop test_gh103_sequential_stop.cpp)
set_tests_properties(model/test-gh103-sequential-stop PROPERTIES TIMEOUT 600)

# gh#103 (v2.8.2): gemma4 severe-case proof — sequential mode stops at the gemma
# close marker `<tool_call|>` AND the tool call extracts (resolves the consumer's
# "delegate registers as zero tool calls" report: runaway-defeats-extraction vs a
# parse bug). gemma4_e4b ~4.5-7.7GB → generous timeout.
add_model_test(test-gh103-gemma-sequential test_gh103_gemma_sequential.cpp)
set_tests_properties(model/test-gh103-gemma-sequential PROPERTIES TIMEOUT 600)

# gh#105 (v2.8.3): RED-first — constitutional validator's toolless render must
# not clobber the main generation's PEG parser before the engine re-parses
# (zero tool-call extraction with validation on). Full engine loop + validation,
# gemma4_e4b (the only family on the reliable-parse path).
add_model_test(test-gh105-validator-clobbers test_gh105_validator_clobbers.cpp)
set_tests_properties(model/test-gh105-validator-clobbers PROPERTIES TIMEOUT 600)

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
