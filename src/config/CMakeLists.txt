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
    PRIVATE spdlog::spdlog
)

target_compile_definitions(entropic-config PRIVATE ENTROPIC_BUILDING_DLL)

set_target_properties(entropic-config PROPERTIES
    OUTPUT_NAME "rentropic-config"
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)
