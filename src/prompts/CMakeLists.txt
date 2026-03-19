add_library(entropic-prompts)

target_sources(entropic-prompts PRIVATE
    manager.cpp
    classification.cpp
)

target_include_directories(entropic-prompts
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(entropic-prompts
    PUBLIC entropic-types ryml::ryml
    PRIVATE spdlog::spdlog
)

target_compile_definitions(entropic-prompts PRIVATE ENTROPIC_BUILDING_DLL)

set_target_properties(entropic-prompts PROPERTIES
    OUTPUT_NAME "rentropic-prompts"
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)
