# SPDX-License-Identifier: LGPL-3.0-or-later
#
# OpenAI-compatible HTTP server bridging librentropic to clients that
# already speak the OpenAI Chat Completions API.
#
# Standalone build (after installing librentropic):
#   cmake -B build -DCMAKE_PREFIX_PATH=$HOME/.local/entropic
#   cmake --build build
#
# In-tree build (against the entropic build dir):
#   cmake -B build -DENTROPIC_LIB_DIR=../../build/full \
#                  -DENTROPIC_INCLUDE_DIR=../../include
#   cmake --build build

cmake_minimum_required(VERSION 3.21)
project(entropic-openai-server CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.18.3
)
FetchContent_MakeAvailable(httplib)

# ── Pull nlohmann/json — same source the engine uses ─────
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

add_executable(entropic-openai-server src/main.cpp)
target_link_libraries(entropic-openai-server PRIVATE
    httplib::httplib
    nlohmann_json::nlohmann_json
)

# Wire up entropic include + lib (matches examples/headless layout)
if(DEFINED ENTROPIC_LIB_DIR)
    target_link_directories(entropic-openai-server PRIVATE ${ENTROPIC_LIB_DIR})
endif()

if(DEFINED ENTROPIC_INCLUDE_DIR)
    target_include_directories(entropic-openai-server PRIVATE ${ENTROPIC_INCLUDE_DIR})
else()
    target_include_directories(entropic-openai-server PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../include)
endif()

if(DEFINED ENTROPIC_BUILD_INCLUDE_DIR)
    target_include_directories(entropic-openai-server PRIVATE
        ${ENTROPIC_BUILD_INCLUDE_DIR})
endif()

target_link_libraries(entropic-openai-server PRIVATE rentropic)
