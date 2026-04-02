# v1.10.1: Regression tests from manual test findings (test1-test6).
# All use mock inference — CPU-only, CI-runnable.
add_executable(entropic-regression-tests
    test_basic_response.cpp
    test_multi_turn_context.cpp
    test_tool_calling_loop.cpp
    test_delegation_pipeline.cpp
    test_router_classification.cpp
    test_identity_behavior.cpp
)

target_include_directories(entropic-regression-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../unit/core
)

target_link_libraries(entropic-regression-tests PRIVATE
    entropic-core
    entropic-mcp
    entropic-types
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    Catch2::Catch2WithMain
)

include(Catch)
catch_discover_tests(entropic-regression-tests
    TEST_PREFIX "regression/"
    PROPERTIES LABELS "regression"
)
