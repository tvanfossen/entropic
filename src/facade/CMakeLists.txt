add_library(entropic-facade)

target_sources(entropic-facade PRIVATE
    entropic.cpp
    entropic_mcp.cpp
    entropic_identity.cpp
    entropic_hooks.cpp
    entropic_storage.cpp
    entropic_audit.cpp
    entropic_compaction.cpp
)

target_link_libraries(entropic-facade PUBLIC
    entropic-types
    entropic-config
    entropic-prompts
    entropic-core
    entropic::inference
    entropic-mcp
    entropic-storage
)

target_include_directories(entropic-facade PRIVATE
    ${CMAKE_SOURCE_DIR}/src/inference  # LlamaCppBackend for adapter wiring
    ${CMAKE_SOURCE_DIR}/extern/llama.cpp/include       # llama.h
    ${CMAKE_SOURCE_DIR}/extern/llama.cpp/ggml/include  # ggml.h
)

target_compile_definitions(entropic-facade PRIVATE ENTROPIC_BUILDING_DLL)

set_target_properties(entropic-facade PROPERTIES
    OUTPUT_NAME "rentropic"
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

# v2.0.5: install as the sole SHARED library of the distribution.
# EXPORT clause is consumed by the install(EXPORT ...) call added in
# Phase 1.3 (CMake package config).
include(GNUInstallDirs)
install(TARGETS entropic-facade
    EXPORT entropic-targets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
