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
    TEST_REPORTS_DIR="${PROJECT_SOURCE_DIR}/test-reports/model"
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

# Pass mock server binary path for external MCP test
if(TARGET mock-mcp-server)
    target_compile_definitions(test-external-mcp PRIVATE
        MOCK_MCP_SERVER_PATH="$<TARGET_FILE:mock-mcp-server>")
    add_dependencies(test-external-mcp mock-mcp-server)
endif()

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
