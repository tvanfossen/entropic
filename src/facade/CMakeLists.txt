add_library(entropic-facade)

target_sources(entropic-facade PRIVATE
    entropic.cpp
)

target_link_libraries(entropic-facade PUBLIC
    entropic-types
    entropic-config
    entropic-prompts
    entropic-core
)

target_compile_definitions(entropic-facade PRIVATE ENTROPIC_BUILDING_DLL)

set_target_properties(entropic-facade PROPERTIES
    OUTPUT_NAME "rentropic"
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)
