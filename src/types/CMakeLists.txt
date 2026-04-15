add_library(entropic-types STATIC)

target_sources(entropic-types PRIVATE
    error.cpp
    logging.cpp
    config.cpp
    content.cpp
    session_logger.cpp
)

target_include_directories(entropic-types PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(entropic-types PUBLIC spdlog::spdlog)

target_compile_definitions(entropic-types PRIVATE ENTROPIC_BUILDING_DLL)

if(ENTROPIC_STATIC)
    target_compile_definitions(entropic-types PUBLIC ENTROPIC_STATIC_BUILD)
endif()

set_target_properties(entropic-types PROPERTIES
    OUTPUT_NAME "rentropic-types"
    CXX_VISIBILITY_PRESET default
)
