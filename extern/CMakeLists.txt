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

# ── llama.cpp ────────────────────────────────────────────
# Submodule added but NOT built in v1.8.0. Build integration deferred
# to v1.8.2 when the inference backend is implemented.
# set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
# set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
# add_subdirectory(llama.cpp)
