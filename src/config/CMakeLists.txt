add_library(entropic-config)

target_sources(entropic-config PRIVATE
    yaml_util.cpp
    bundled_models.cpp
    loader.cpp
    validate.cpp
    data_dir.cpp
    env_overrides.cpp
)

target_include_directories(entropic-config
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(entropic-config
    PUBLIC entropic-types ryml::ryml
    PRIVATE spdlog::spdlog nlohmann_json::nlohmann_json
)

target_compile_definitions(entropic-config PRIVATE ENTROPIC_BUILDING_DLL)

set_target_properties(entropic-config PROPERTIES
    OUTPUT_NAME "rentropic-config"
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    CXX_VISIBILITY_PRESET default
    VISIBILITY_INLINES_HIDDEN OFF
)
