# SPDX-License-Identifier: Apache-2.0
# v1.9.14: Concurrency tests — thread-safety validation
#
# Primary TSan targets. Exercise ToolCallHistory, ProfileRegistry,
# ThroughputTracker, MCPKeySet, HookRegistry from multiple threads.

add_executable(entropic-concurrency-tests
    test_thread_safety.cpp
)

target_include_directories(entropic-concurrency-tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/inference
    ${PROJECT_SOURCE_DIR}/src/mcp
    # gh#158 (v2.13.0): the per-session run guard is exercised against the
    # shared MockInference, the same one the core tests drive.
    ${PROJECT_SOURCE_DIR}/tests/unit/core
)

target_link_libraries(entropic-concurrency-tests PRIVATE
    entropic-core
    entropic-mcp
    entropic::inference
    entropic-types
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    Catch2::Catch2WithMain
)

# gh#171 (v2.14.0): a real external MCP child to connect/disconnect while
# other threads route through the same ServerManager.
target_compile_definitions(entropic-concurrency-tests PRIVATE
    MOCK_MCP_SERVER_PATH="$<TARGET_FILE:mock-mcp-server>")
add_dependencies(entropic-concurrency-tests mock-mcp-server)

include(Catch)
catch_discover_tests(entropic-concurrency-tests)
