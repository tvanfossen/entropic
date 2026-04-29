# SPDX-License-Identifier: LGPL-3.0-or-later
cmake_minimum_required(VERSION 3.21)

# v2.1.2 (#4): single source of truth. The repo-root VERSION file is the
# canonical project version — CMake reads it here, pyproject.toml's
# dynamic version reads it for wheel metadata, docs.yml injects it as
# doxygen's PROJECT_NUMBER, and tests/unit/api/api_version_test.cpp
# reads it at test time to verify the runtime entropic_version() string
# matches. Bumping the version is one edit: update VERSION.
file(STRINGS "${CMAKE_SOURCE_DIR}/VERSION" ENTROPIC_VERSION LIMIT_COUNT 1)
project(entropic
    VERSION ${ENTROPIC_VERSION}
    LANGUAGES C CXX
    DESCRIPTION "Entropic inference engine"
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# v2.0.5: sublibs are always built STATIC and absorbed into the single
# librentropic.so facade. PIC is required so the static archives can
# be linked into the shared facade regardless of the top-level build mode.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Library type: default to shared unless ENTROPIC_STATIC is set
if(ENTROPIC_SHARED AND NOT ENTROPIC_STATIC)
    set(BUILD_SHARED_LIBS ON)
else()
    set(BUILD_SHARED_LIBS OFF)
endif()

# Symbol visibility: hidden by default, ENTROPIC_EXPORT opts in
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# ── Options ─────────────────────────────────────────────
option(ENTROPIC_SHARED       "Build shared libraries (.so)"          ON)
option(ENTROPIC_STATIC       "Build static library (.a)"             OFF)
option(ENTROPIC_CUDA         "Build CUDA inference backend"          ON)
option(ENTROPIC_VULKAN       "Build Vulkan inference backend"        OFF)
option(ENTROPIC_CPU_ONLY     "Build CPU-only inference backend"      OFF)
option(ENTROPIC_MCP_FILESYSTEM  "Include filesystem MCP server"     ON)
option(ENTROPIC_MCP_BASH        "Include bash MCP server"           ON)
option(ENTROPIC_MCP_GIT         "Include git MCP server"            ON)
option(ENTROPIC_MCP_DIAGNOSTICS "Include diagnostics MCP server"    ON)
option(ENTROPIC_MCP_WEB         "Include web MCP server"            ON)
option(ENTROPIC_MCP_ENTROPIC    "Include entropic MCP server"       ON)
option(ENTROPIC_STORAGE_SQLITE  "Include SQLite storage backend"    ON)
option(ENTROPIC_STREAMING       "Include streaming generation API"  ON)
option(ENTROPIC_CUDA_GRAPHS    "Enable CUDA graphs (no benefit for autoregressive chat)" OFF)
option(ENTROPIC_BUILD_TESTS        "Build test suite"                  ON)
option(ENTROPIC_BUILD_MODEL_TESTS  "Build model tests (GPU required)"  OFF)
option(ENTROPIC_BUILD_DOCS         "Build Doxygen documentation"       OFF)
option(ENTROPIC_COVERAGE           "Build with coverage instrumentation (gcov)" OFF)

# ── Coverage instrumentation ──────────────────────────
if(ENTROPIC_COVERAGE)
    add_compile_options(--coverage -fprofile-arcs -ftest-coverage)
    add_link_options(--coverage)
    message(STATUS "Coverage instrumentation enabled (gcov)")
endif()

# ── CUDA detection ──────────────────────────────────────
if(ENTROPIC_CUDA AND NOT ENTROPIC_CPU_ONLY)
    include(CheckLanguage)
    check_language(CUDA)
    if(CMAKE_CUDA_COMPILER)
        enable_language(CUDA)
        set(ENTROPIC_HAS_CUDA ON)
        message(STATUS "CUDA found: ${CMAKE_CUDA_COMPILER}")
    else()
        message(WARNING "CUDA requested but not found — falling back to CPU")
        set(ENTROPIC_HAS_CUDA OFF)
    endif()
else()
    set(ENTROPIC_HAS_CUDA OFF)
endif()

# ── CONFIG_ENTROPIC_* bridge for entropic_config.h.in ───
set(CONFIG_ENTROPIC_SHARED   ${ENTROPIC_SHARED})
set(CONFIG_ENTROPIC_STATIC   ${ENTROPIC_STATIC})
set(CONFIG_ENTROPIC_CUDA     ${ENTROPIC_CUDA})
set(CONFIG_ENTROPIC_VULKAN   ${ENTROPIC_VULKAN})
set(CONFIG_ENTROPIC_CPU_ONLY ${ENTROPIC_CPU_ONLY})
set(CONFIG_ENTROPIC_HAS_CUDA ${ENTROPIC_HAS_CUDA})
set(CONFIG_ENTROPIC_MCP_FILESYSTEM  ${ENTROPIC_MCP_FILESYSTEM})
set(CONFIG_ENTROPIC_MCP_BASH        ${ENTROPIC_MCP_BASH})
set(CONFIG_ENTROPIC_MCP_GIT         ${ENTROPIC_MCP_GIT})
set(CONFIG_ENTROPIC_MCP_DIAGNOSTICS ${ENTROPIC_MCP_DIAGNOSTICS})
set(CONFIG_ENTROPIC_MCP_WEB         ${ENTROPIC_MCP_WEB})
set(CONFIG_ENTROPIC_MCP_ENTROPIC    ${ENTROPIC_MCP_ENTROPIC})
set(CONFIG_ENTROPIC_STORAGE_SQLITE  ${ENTROPIC_STORAGE_SQLITE})
set(CONFIG_ENTROPIC_STREAMING       ${ENTROPIC_STREAMING})
set(CONFIG_ENTROPIC_SOURCE_DATA_DIR ${ENTROPIC_SOURCE_DATA_DIR})

# ── API version (bumped on breaking C API changes) ──────
set(ENTROPIC_API_VERSION 2 CACHE STRING "C API version integer")

# ── Generate entropic_config.h ──────────────────────────
# v2.0.5: data is installed directly under share/entropic/ (no nested
# /data/ subdirectory). Matches install(DIRECTORY data/ DESTINATION
# share/entropic/) below and the 4-level discovery in data_dir.cpp.
set(ENTROPIC_DATA_DIR "${CMAKE_INSTALL_PREFIX}/share/entropic"
    CACHE PATH "Install path for bundled data files")
# Source tree data dir for development builds (tools, prompts, grammars)
set(ENTROPIC_SOURCE_DATA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/data"
    CACHE PATH "Source tree path to bundled data files")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/entropic/entropic_config.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/include/entropic/entropic_config.h"
    @ONLY
)

# ── System dependencies ─────────────────────────────────
# v1.8.8: SQLite3 for storage backend (system library, >= 3.35.0 for FTS5)
if(ENTROPIC_STORAGE_SQLITE)
    find_package(SQLite3 3.35.0 REQUIRED)
endif()

# ── Subdirectories ──────────────────────────────────────
add_subdirectory(extern)
add_subdirectory(src/types)
add_subdirectory(src/config)
add_subdirectory(src/prompts)
add_subdirectory(src/inference)
add_subdirectory(src/core)
add_subdirectory(src/mcp)
add_subdirectory(src/storage)
add_subdirectory(src/facade)
add_subdirectory(src/cli)
add_subdirectory(python)

if(ENTROPIC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(ENTROPIC_BUILD_DOCS)
    find_package(Doxygen REQUIRED)
    add_subdirectory(docs)
endif()

# ── Install ─────────────────────────────────────────────
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Public headers (entire tree; internal headers live in src/ not include/)
install(DIRECTORY include/entropic
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h"
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/include/entropic/entropic_config.h"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/entropic
)

# Runtime data: bundled models registry, prompts, grammars, tool defs
install(DIRECTORY data/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/entropic
)

# License + release README (the README shipped with the tarball lives
# under docs/dist-README.md; the repo's primary README.md is for
# contributors).
install(FILES LICENSE NOTICE docs/dist-README.md
    DESTINATION ${CMAKE_INSTALL_DOCDIR}
)

# ── CMake package config (find_package(entropic) support) ──
# Exports the entropic::entropic imported target with include dirs, link
# library, SOVERSION, and C++20 requirement baked in. Matches SONAME
# scheme: SameMajorVersion → 2.x.y compatible, 3.x breaks.
set(ENTROPIC_CMAKE_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/entropic")

install(EXPORT entropic-targets
    FILE entropic-targets.cmake
    NAMESPACE entropic::
    DESTINATION ${ENTROPIC_CMAKE_INSTALL_DIR}
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/entropic-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/entropic-config.cmake"
    INSTALL_DESTINATION ${ENTROPIC_CMAKE_INSTALL_DIR}
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/entropic-config-version.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/entropic-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/entropic-config-version.cmake"
    DESTINATION ${ENTROPIC_CMAKE_INSTALL_DIR}
)
