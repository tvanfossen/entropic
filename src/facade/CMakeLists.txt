# SPDX-License-Identifier: LGPL-3.0-or-later
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

# v2.0.5: sublibs are OBJECT libraries consumed as raw object files
# ($<TARGET_OBJECTS:...>) rather than link-library targets. This absorbs
# their .o files into librentropic.so directly without making them
# part of the facade's link-dep closure — so install(EXPORT) does not
# recurse into unreachable sublib/FetchContent targets.
target_sources(entropic-facade PRIVATE
    $<TARGET_OBJECTS:entropic-types>
    $<TARGET_OBJECTS:entropic-config>
    $<TARGET_OBJECTS:entropic-prompts>
    $<TARGET_OBJECTS:entropic-core>
    $<TARGET_OBJECTS:entropic-mcp>
    $<TARGET_OBJECTS:entropic-storage>
    $<TARGET_OBJECTS:entropic::inference>
)

# External build-time deps the sublibs reference, now linked PRIVATE at
# the facade boundary so they resolve at the shared library's link step.
# All are absorbed as static code into librentropic.so; none leak into
# the consumer-visible export. $<LINK_ONLY:> suppresses CMake's pedantic
# tracking of these FetchContent targets in the install(EXPORT) set.
target_link_libraries(entropic-facade PRIVATE
    $<LINK_ONLY:spdlog::spdlog>
    $<LINK_ONLY:ryml::ryml>
    $<LINK_ONLY:nlohmann_json::nlohmann_json>
    $<LINK_ONLY:httplib::httplib>
    $<LINK_ONLY:llama>
    # libdl for dladdr() — used by config/data_dir.cpp to derive
    # <prefix>/share/entropic relative to librentropic.so at runtime.
    # On glibc 2.34+ dlfcn is merged into libc; CMAKE_DL_LIBS keeps
    # older targets working.
    $<LINK_ONLY:${CMAKE_DL_LIBS}>
)
if(ENTROPIC_STORAGE_SQLITE)
    target_link_libraries(entropic-facade PRIVATE $<LINK_ONLY:SQLite::SQLite3>)
endif()

# Public include dirs: during the build, the in-tree include/ and
# generated config/ headers; for installed consumers, <prefix>/include/.
target_include_directories(entropic-facade
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src/inference  # LlamaCppBackend for adapter wiring
        ${CMAKE_SOURCE_DIR}/extern/llama.cpp/include       # llama.h
        ${CMAKE_SOURCE_DIR}/extern/llama.cpp/ggml/include  # ggml.h
)

target_compile_features(entropic-facade PUBLIC cxx_std_20)

target_compile_definitions(entropic-facade PRIVATE ENTROPIC_BUILDING_DLL)

set_target_properties(entropic-facade PROPERTIES
    OUTPUT_NAME "rentropic"
    EXPORT_NAME entropic       # exported as entropic::entropic
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
