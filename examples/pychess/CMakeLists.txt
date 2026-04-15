# SPDX-License-Identifier: LGPL-3.0-or-later
# PyChess — multi-tier chess example using the entropic C API.
#
# Standalone build (not part of the main entropic build):
#   cmake -B build -DCMAKE_PREFIX_PATH=/path/to/entropic/install
#   cmake --build build
#
# Or, if librentropic is in the main build tree:
#   cmake -B build -DENTROPIC_LIB_DIR=../../build/full
#   cmake --build build

cmake_minimum_required(VERSION 3.21)
project(pychess CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(pychess main.cpp)

# --- Find librentropic ---

# Option 1: Explicit library directory (build tree)
if(DEFINED ENTROPIC_LIB_DIR)
    target_link_directories(pychess PRIVATE ${ENTROPIC_LIB_DIR})
endif()

# Option 2: Explicit include directories (source + build tree)
if(DEFINED ENTROPIC_INCLUDE_DIR)
    target_include_directories(pychess PRIVATE ${ENTROPIC_INCLUDE_DIR})
else()
    # Default: assume source tree layout (examples/ is sibling of include/)
    target_include_directories(pychess PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../include)
endif()

# Generated headers (entropic_config.h) live in the build tree
if(DEFINED ENTROPIC_BUILD_INCLUDE_DIR)
    target_include_directories(pychess PRIVATE ${ENTROPIC_BUILD_INCLUDE_DIR})
endif()

# v2.0.5: sublibs are OBJECT-absorbed into the facade — only
# librentropic.so exists. Link the facade only.
target_link_libraries(pychess PRIVATE rentropic)
