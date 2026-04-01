# v1.9.14: Edge case tests — malformed inputs, error paths
#
# Fast (unit-test speed), run in standard test suite.
# Tests exercise error handling paths that normal unit tests don't reach.

add_executable(entropic-edge-tool-tests
    test_malformed_tool_calls.cpp
)

target_link_libraries(entropic-edge-tool-tests PRIVATE
    entropic-mcp
    entropic-core
    entropic-types
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    Catch2::Catch2WithMain
)

add_executable(entropic-edge-config-tests
    test_config_edge_cases.cpp
)

target_link_libraries(entropic-edge-config-tests PRIVATE
    entropic-config
    entropic-prompts
    entropic-types
    ryml::ryml
    spdlog::spdlog
    Catch2::Catch2WithMain
)

target_compile_definitions(entropic-edge-config-tests PRIVATE
    TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../data"
)

include(Catch)
catch_discover_tests(entropic-edge-tool-tests)
catch_discover_tests(entropic-edge-config-tests)
