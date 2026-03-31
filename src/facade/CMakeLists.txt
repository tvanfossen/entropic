add_library(entropic-facade)

target_sources(entropic-facade PRIVATE
    entropic.cpp
    entropic_mcp.cpp
    entropic_identity.cpp
    entropic_hooks.cpp
    entropic_storage.cpp
    entropic_audit.cpp
)

target_link_libraries(entropic-facade PUBLIC
    entropic-types
    entropic-config
    entropic-prompts
    entropic-core
    entropic-mcp
)

target_compile_definitions(entropic-facade PRIVATE ENTROPIC_BUILDING_DLL)

set_target_properties(entropic-facade PROPERTIES
    OUTPUT_NAME "rentropic"
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)
