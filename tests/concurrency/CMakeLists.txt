# SPDX-License-Identifier: LGPL-3.0-or-later
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

include(Catch)
catch_discover_tests(entropic-concurrency-tests)
