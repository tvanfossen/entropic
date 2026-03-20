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

include(CTest)
include(Catch)
catch_discover_tests(entropic-tests)
catch_discover_tests(entropic-config-tests)
catch_discover_tests(entropic-inference-tests)
catch_discover_tests(entropic-core-tests)
catch_discover_tests(entropic-mcp-tests)
