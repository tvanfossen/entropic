# ── External dependencies ────────────────────────────────
# Fetched at configure time via FetchContent. Header-only libraries
# are INTERFACE targets. llama.cpp is a submodule but NOT built in v1.8.0.

include(FetchContent)

# ── spdlog (header-only structured logging) ──────────────
FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.15.2
    GIT_SHALLOW    TRUE
)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(spdlog)

# ── Catch2 (test framework, BDD-style) ──────────────────
if(ENTROPIC_BUILD_TESTS)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.8.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} PARENT_SCOPE)
endif()

# ── ryml (YAML parsing — single-header amalgamation) ─────
FetchContent_Declare(ryml
    GIT_REPOSITORY https://github.com/biojppm/rapidyaml.git
    GIT_TAG        v0.7.2
    GIT_SHALLOW    TRUE
)
set(RYML_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(RYML_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(ryml)
# Override hidden visibility — ryml symbols must be visible when linked
# into entropic shared libraries (config.so, prompts.so).
set_target_properties(ryml PROPERTIES CXX_VISIBILITY_PRESET default)
set_target_properties(c4core PROPERTIES CXX_VISIBILITY_PRESET default)

# ── llama.cpp ────────────────────────────────────────────
# v1.8.2: Build llama.cpp as part of the inference backend.
# Submodule pinned at b8420.
set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
# Bridge ENTROPIC_HAS_CUDA → GGML_CUDA so llama.cpp builds with GPU support
if(ENTROPIC_HAS_CUDA)
    set(GGML_CUDA ON CACHE BOOL "" FORCE)
endif()
set(BUILD_SHARED_LIBS_SAVED ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE_SAVED ${CMAKE_POSITION_INDEPENDENT_CODE})
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_subdirectory(llama.cpp)
set(BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS_SAVED})
set(CMAKE_POSITION_INDEPENDENT_CODE ${CMAKE_POSITION_INDEPENDENT_CODE_SAVED})

# ── nlohmann/json (header-only JSON) ────────────────────
FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(json)

# ── cpp-httplib (header-only HTTP/SSE client) ────────────
FetchContent_Declare(httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.18.3
    GIT_SHALLOW    TRUE
)
set(HTTPLIB_COMPILE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_REQUIRE_OPENSSL OFF CACHE BOOL "" FORCE)
set(HTTPLIB_REQUIRE_ZLIB OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(httplib)
