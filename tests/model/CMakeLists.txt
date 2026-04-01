# v1.10.0: Merged model test + benchmark suite
#
# Combines model quality tests and performance benchmarks into one suite.
# Both require GPU and a loaded model — neither runs in pre-commit.
#
# Gate: ENTROPIC_BUILD_MODEL_TESTS=OFF (default)
# Enable: cmake -DENTROPIC_BUILD_MODEL_TESTS=ON ..
#
# CTest label "model" for independent execution:
#   ctest -L model
#
# Reports written to: test-reports/model/
#
# Source files will be populated as C++ model tests are implemented.
# Python model tests were removed in v1.9.15 — C++ replacements go here.

if(NOT ENTROPIC_BUILD_MODEL_TESTS)
    return()
endif()

# Model quality tests
# add_executable(entropic-model-tests
#     model_inference_test.cpp
#     model_routing_test.cpp
#     model_identity_test.cpp
# )
#
# target_link_libraries(entropic-model-tests PRIVATE
#     entropic-facade
#     entropic::inference
#     entropic-core
#     entropic-types
#     nlohmann_json::nlohmann_json
#     spdlog::spdlog
#     Catch2::Catch2WithMain
# )
#
# target_compile_definitions(entropic-model-tests PRIVATE
#     TEST_REPORTS_DIR="${PROJECT_SOURCE_DIR}/test-reports/model"
# )
#
# catch_discover_tests(entropic-model-tests
#     PROPERTIES LABELS "model"
# )

# Performance benchmarks
# add_executable(entropic-benchmarks
#     bench_inference.cpp
#     bench_engine_overhead.cpp
#     bench_memory_profile.cpp
# )
#
# target_link_libraries(entropic-benchmarks PRIVATE
#     entropic-facade
#     entropic::inference
#     entropic-core
#     entropic-types
#     nlohmann_json::nlohmann_json
#     spdlog::spdlog
#     Catch2::Catch2WithMain
# )
#
# target_compile_definitions(entropic-benchmarks PRIVATE
#     TEST_REPORTS_DIR="${PROJECT_SOURCE_DIR}/test-reports/model"
# )
#
# catch_discover_tests(entropic-benchmarks
#     PROPERTIES LABELS "model;benchmark"
# )
