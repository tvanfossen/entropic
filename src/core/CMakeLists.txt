add_library(entropic-core)

target_sources(entropic-core PRIVATE
    engine_types.cpp
    compaction.cpp
    compactor_registry.cpp
    directives.cpp
    context_manager.cpp
    response_generator.cpp
    engine.cpp
    worktree.cpp
    delegation.cpp
    hook_registry.cpp
    identity_manager.cpp
    constitutional_validator.cpp
    stream_think_filter.cpp
)

target_include_directories(entropic-core PUBLIC
    ${PROJECT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/../../include
)

target_link_libraries(entropic-core PUBLIC
    entropic-types
    spdlog::spdlog
    nlohmann_json::nlohmann_json
)

target_compile_definitions(entropic-core PRIVATE ENTROPIC_BUILDING_DLL)

set_target_properties(entropic-core PROPERTIES
    OUTPUT_NAME "rentropic-core"
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    CXX_VISIBILITY_PRESET default
)
