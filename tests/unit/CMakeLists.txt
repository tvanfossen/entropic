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
add_executable(entropic-inference-tests
    inference/test_backend_lifecycle.cpp
    inference/test_adapter_base.cpp
    inference/test_qwen35_adapter.cpp
    inference/test_orchestrator.cpp
    inference/test_generation_params.cpp
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

include(CTest)
include(Catch)
catch_discover_tests(entropic-tests)
catch_discover_tests(entropic-config-tests)
catch_discover_tests(entropic-inference-tests)
