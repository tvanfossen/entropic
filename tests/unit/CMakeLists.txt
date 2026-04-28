# SPDX-License-Identifier: LGPL-3.0-or-later
add_executable(entropic-tests
    types/error_test.cpp
    types/config_structs_test.cpp
    types/logging_test.cpp
    types/content_test.cpp
    types/message_multimodal_test.cpp
)

target_link_libraries(entropic-tests PRIVATE
    entropic-types
    spdlog::spdlog
    Catch2::Catch2WithMain
)

# v1.8.1: Config + Prompts tests
add_executable(entropic-config-tests
    config/bundled_models_test.cpp
    config/validate_test.cpp
    config/loader_test.cpp
    config/yaml_util_test.cpp
    config/env_overrides_test.cpp
    prompts/prompt_parse_test.cpp
    prompts/classification_prompt_test.cpp
)

target_include_directories(entropic-config-tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/config
)

target_link_libraries(entropic-config-tests PRIVATE
    entropic-prompts
    entropic-config
    entropic-types
    ryml::ryml
    spdlog::spdlog
    Catch2::Catch2WithMain
)

# Pass test data directory as a compile-time define
target_compile_definitions(entropic-config-tests PRIVATE
    TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../data"
)

# v1.8.2: Inference tests
# v1.8.3: Added prompt_cache_test.cpp
# v1.9.10: Added logprob_evaluation_test.cpp
# v1.9.13: Added backend_capabilities_test.cpp
add_executable(entropic-inference-tests
    inference/backend_lifecycle_test.cpp
    inference/adapter_base_test.cpp
    inference/qwen35_adapter_test.cpp
    inference/orchestrator_test.cpp
    inference/generation_params_test.cpp
    inference/prompt_cache_test.cpp
    inference/logprob_evaluation_test.cpp
    inference/vision_adapter_test.cpp
    inference/image_preprocessor_test.cpp
    inference/backend_capabilities_test.cpp
)

target_include_directories(entropic-inference-tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/inference
)

target_link_libraries(entropic-inference-tests PRIVATE
    entropic::inference
    entropic-types
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    Catch2::Catch2WithMain
)

# v1.9.11: Test data directory for image preprocessor tests
target_compile_definitions(entropic-inference-tests PRIVATE
    TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../data"
)

# v1.9.2: Adapter manager tests (uses mock llama.cpp adapter symbols)
add_executable(entropic-adapter-tests
    inference/adapter_manager_test.cpp
    inference/adapter_orchestrator_test.cpp
    ${PROJECT_SOURCE_DIR}/src/inference/adapter_manager.cpp
)

target_include_directories(entropic-adapter-tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/inference
)

target_link_libraries(entropic-adapter-tests PRIVATE
    entropic-types
    llama
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    Catch2::Catch2WithMain
)

# v1.9.3: Grammar registry tests
add_executable(entropic-grammar-tests
    inference/grammar_registry_test.cpp
    inference/grammar_orchestrator_test.cpp
    ${PROJECT_SOURCE_DIR}/src/inference/grammar_registry.cpp
)

target_include_directories(entropic-grammar-tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/inference
)

target_link_libraries(entropic-grammar-tests PRIVATE
    entropic-types
    llama
    spdlog::spdlog
    Catch2::Catch2WithMain
)

# v1.9.7: Profile registry + throughput tracker tests
add_executable(entropic-cap-tests
    inference/profile_registry_test.cpp
    inference/throughput_tracker_test.cpp
    ${PROJECT_SOURCE_DIR}/src/inference/profile_registry.cpp
    ${PROJECT_SOURCE_DIR}/src/inference/throughput_tracker.cpp
)

target_include_directories(entropic-cap-tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/inference
)

target_link_libraries(entropic-cap-tests PRIVATE
    entropic-types
    spdlog::spdlog
    Catch2::Catch2WithMain
)

# v1.8.4: Core engine tests
# v1.8.6: Added delegation, worktree, auto-chain tests
add_executable(entropic-core-tests
    core/compaction_test.cpp
    core/compactor_registry_test.cpp
    core/directives_test.cpp
    core/context_manager_test.cpp
    core/engine_test.cpp
    core/worktree_test.cpp
    core/test_worktree_session_isolation.cpp
    core/delegation_manager_test.cpp
    core/auto_chain_test.cpp
    core/hook_registry_test.cpp
    core/constitutional_validator_test.cpp
    core/validation_hook_test.cpp
    core/response_generator_test.cpp
)

target_include_directories(entropic-core-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/core
)

target_link_libraries(entropic-core-tests PRIVATE
    entropic-core
    entropic-types
    spdlog::spdlog
    Catch2::Catch2WithMain
)

# v1.8.5: MCP tool system tests
# v1.8.7: Added external client, reconnect, discovery tests
add_executable(entropic-mcp-tests
    mcp/tool_registry_test.cpp
    mcp/server_base_test.cpp
    mcp/permission_manager_test.cpp
    mcp/tool_executor_test.cpp
    mcp/server_manager_test.cpp
    mcp/tool_calling_loop_test.cpp
    # Per-server behavioral tests
    mcp/servers/filesystem_test.cpp
    mcp/servers/bash_test.cpp
    mcp/servers/git_test.cpp
    mcp/servers/diagnostics_test.cpp
    mcp/servers/web_test.cpp
    mcp/servers/entropic_test.cpp
    # v1.8.7: External MCP tests
    mcp/external_client_test.cpp
    mcp/reconnect_policy_test.cpp
    mcp/mcp_json_discovery_test.cpp
    # v1.9.4: MCP authorization tests
    mcp/mcp_key_set_test.cpp
    mcp/mcp_authorization_test.cpp
    mcp/tool_executor_authorization_test.cpp
    # v1.9.12: Diagnose + self-inspection
    mcp/tool_call_history_test.cpp
    mcp/servers/diagnose_test.cpp
    mcp/servers/inspect_test.cpp
    # v2.0.6-rc16: Context window inspection (P2-16)
    mcp/servers/context_inspect_test.cpp
    # v2.1.0 #47: UTF-8 sanitizer for inbound tool-result strings
    mcp/utf8_sanitize_test.cpp
    # v2.1.0 #44: tool-result content classifiers (empty/error)
    mcp/tool_result_classify_test.cpp
)

target_include_directories(entropic-mcp-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Pass source-tree data dir for tool JSON resolution in tests
target_compile_definitions(entropic-mcp-tests PRIVATE
    TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/data"
)

target_link_libraries(entropic-mcp-tests PRIVATE
    entropic-mcp
    entropic-core
    entropic-types
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    Catch2::Catch2WithMain
)

# v1.8.8: Storage tests
if(ENTROPIC_STORAGE_SQLITE)
    add_executable(entropic-storage-tests
        storage/database_test.cpp
        storage/backend_test.cpp
        storage/c_interface_test.cpp
        storage/permission_persister_test.cpp
        storage/audit_entry_test.cpp
        storage/audit_logger_test.cpp
    )

    target_link_libraries(entropic-storage-tests PRIVATE
        entropic-storage
        entropic-types
        nlohmann_json::nlohmann_json
        spdlog::spdlog
        Catch2::Catch2WithMain
    )

    # database_test.cpp uses sqlite3 API directly for stmt binding
    target_link_libraries(entropic-storage-tests PRIVATE SQLite::SQLite3)
endif()

# v1.8.9: C API stabilization tests
add_executable(entropic-api-tests
    api/api_version_test.cpp
    api/identity_api_test.cpp
    api/hook_stub_test.cpp
    api/error_enum_test.cpp
    api/facade_integration_test.cpp
    # v2.0.11: External bridge tests (links facade for ENTROPIC_EXPORT symbols)
    mcp/external_bridge_test.cpp
)

target_link_libraries(entropic-api-tests PRIVATE
    entropic-facade
    entropic-core
    entropic-types
    spdlog::spdlog
    Catch2::Catch2WithMain
)

# v1.8.9: C99 header compilation test (pure C, not C++)
add_executable(entropic-c99-test api/header_c99_test.c)
target_link_libraries(entropic-c99-test PRIVATE entropic-facade entropic-types)
set_target_properties(entropic-c99-test PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
)
# C99 test just needs to compile and link — run it as a test
add_test(NAME entropic-c99-header-test COMMAND entropic-c99-test)

# v1.9.6: Identity manager tests (cross-library: core + inference + mcp).
# v2.0.5: sublibs are OBJECT libraries now — link them in directly
# rather than compiling individual .cpp files (which would collide with
# the OBJECT libs' own copies when both get absorbed).
add_executable(entropic-identity-tests
    core/identity_manager_test.cpp
    core/identity_validation_test.cpp
)

target_include_directories(entropic-identity-tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/inference
    ${PROJECT_SOURCE_DIR}/src/mcp
)

target_link_libraries(entropic-identity-tests PRIVATE
    entropic-core
    entropic-types
    entropic-mcp
    entropic::inference
    llama
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    Catch2::Catch2WithMain
)

include(CTest)
include(Catch)
catch_discover_tests(entropic-tests)
catch_discover_tests(entropic-config-tests)
catch_discover_tests(entropic-inference-tests)
catch_discover_tests(entropic-adapter-tests)
catch_discover_tests(entropic-grammar-tests)
catch_discover_tests(entropic-cap-tests)
catch_discover_tests(entropic-core-tests)
catch_discover_tests(entropic-mcp-tests)
catch_discover_tests(entropic-api-tests)
catch_discover_tests(entropic-identity-tests)
if(ENTROPIC_STORAGE_SQLITE)
    catch_discover_tests(entropic-storage-tests)
endif()
