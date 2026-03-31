add_executable(entropic-tests
    test_error.cpp
    test_config_structs.cpp
    test_logging.cpp
)

target_link_libraries(entropic-tests PRIVATE
    entropic-types
    spdlog::spdlog
    Catch2::Catch2WithMain
)

# v1.8.1: Config + Prompts tests
add_executable(entropic-config-tests
    test_bundled_models.cpp
    test_config_validate.cpp
    test_config_loader.cpp
    test_prompt_parse.cpp
    test_classification_prompt.cpp
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
# v1.8.3: Added test_prompt_cache.cpp
add_executable(entropic-inference-tests
    inference/test_backend_lifecycle.cpp
    inference/test_adapter_base.cpp
    inference/test_qwen35_adapter.cpp
    inference/test_orchestrator.cpp
    inference/test_generation_params.cpp
    inference/test_prompt_cache.cpp
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

# v1.9.2: Adapter manager tests (uses mock llama.cpp adapter symbols)
add_executable(entropic-adapter-tests
    inference/test_adapter_manager.cpp
    inference/test_adapter_orchestrator.cpp
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
    inference/test_grammar_registry.cpp
    inference/test_grammar_orchestrator.cpp
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

# v1.8.4: Core engine tests
# v1.8.6: Added delegation, worktree, auto-chain tests
add_executable(entropic-core-tests
    core/test_compaction.cpp
    core/test_directives.cpp
    core/test_context_manager.cpp
    core/test_engine.cpp
    core/test_worktree.cpp
    core/test_delegation_manager.cpp
    core/test_auto_chain.cpp
    core/test_hook_registry.cpp
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
    mcp/test_tool_registry.cpp
    mcp/test_server_base.cpp
    mcp/test_permission_manager.cpp
    mcp/test_tool_executor.cpp
    mcp/test_server_manager.cpp
    mcp/test_tool_calling_loop.cpp
    # Per-server behavioral tests
    mcp/servers/test_filesystem.cpp
    mcp/servers/test_bash.cpp
    mcp/servers/test_git.cpp
    mcp/servers/test_diagnostics.cpp
    mcp/servers/test_web.cpp
    mcp/servers/test_entropic.cpp
    # v1.8.7: External MCP tests
    mcp/test_external_client.cpp
    mcp/test_reconnect_policy.cpp
    mcp/test_mcp_json_discovery.cpp
    # v1.9.4: MCP authorization tests
    mcp/test_mcp_key_set.cpp
    mcp/test_mcp_authorization.cpp
    mcp/test_tool_executor_authorization.cpp
)

target_include_directories(entropic-mcp-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Pass source-tree data dir for tool JSON resolution in tests
target_compile_definitions(entropic-mcp-tests PRIVATE
    TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/src/entropic/data"
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
        storage/test_database.cpp
        storage/test_backend.cpp
        storage/test_c_interface.cpp
        storage/test_logger.cpp
        storage/test_permission_persister.cpp
    )

    target_link_libraries(entropic-storage-tests PRIVATE
        entropic-storage
        entropic-types
        nlohmann_json::nlohmann_json
        spdlog::spdlog
        Catch2::Catch2WithMain
    )

    # test_database.cpp uses sqlite3 API directly for stmt binding
    target_link_libraries(entropic-storage-tests PRIVATE SQLite::SQLite3)
endif()

# v1.8.9: C API stabilization tests
add_executable(entropic-api-tests
    test_api_version.cpp
    test_identity_api.cpp
    test_hook_stub.cpp
    test_error_enum.cpp
)

target_link_libraries(entropic-api-tests PRIVATE
    entropic-facade
    entropic-core
    entropic-types
    spdlog::spdlog
    Catch2::Catch2WithMain
)

# v1.8.9: C99 header compilation test (pure C, not C++)
add_executable(entropic-c99-test test_header_c99.c)
target_link_libraries(entropic-c99-test PRIVATE entropic-facade entropic-types)
set_target_properties(entropic-c99-test PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
)
# C99 test just needs to compile and link — run it as a test
add_test(NAME entropic-c99-header-test COMMAND entropic-c99-test)

include(CTest)
include(Catch)
catch_discover_tests(entropic-tests)
catch_discover_tests(entropic-config-tests)
catch_discover_tests(entropic-inference-tests)
catch_discover_tests(entropic-adapter-tests)
catch_discover_tests(entropic-grammar-tests)
catch_discover_tests(entropic-core-tests)
catch_discover_tests(entropic-mcp-tests)
catch_discover_tests(entropic-api-tests)
if(ENTROPIC_STORAGE_SQLITE)
    catch_discover_tests(entropic-storage-tests)
endif()
