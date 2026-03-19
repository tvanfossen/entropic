cmake_minimum_required(VERSION 3.21)
project(entropic
    VERSION 1.8.1
    LANGUAGES C CXX
    DESCRIPTION "Entropic inference engine"
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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
option(ENTROPIC_BUILD_TESTS     "Build test suite"                  ON)
option(ENTROPIC_BUILD_DOCS      "Build Doxygen documentation"       OFF)

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

# ── Generate entropic_config.h ──────────────────────────
set(ENTROPIC_DATA_DIR "${CMAKE_INSTALL_PREFIX}/share/entropic/data"
    CACHE PATH "Install path for bundled data files")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/entropic/entropic_config.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/include/entropic/entropic_config.h"
    @ONLY
)

# ── Subdirectories ──────────────────────────────────────
add_subdirectory(extern)
add_subdirectory(src/types)
add_subdirectory(src/config)
add_subdirectory(src/prompts)
add_subdirectory(src/facade)

if(ENTROPIC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(ENTROPIC_BUILD_DOCS)
    find_package(Doxygen REQUIRED)
    add_subdirectory(docs)
endif()

# ── Install ─────────────────────────────────────────────
install(DIRECTORY include/entropic
    DESTINATION include
    FILES_MATCHING PATTERN "*.h"
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/include/entropic/entropic_config.h"
    DESTINATION include/entropic
)
