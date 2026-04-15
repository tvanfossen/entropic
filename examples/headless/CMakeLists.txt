# SPDX-License-Identifier: LGPL-3.0-or-later
# Headless example — scripted CI validation for entropic.
#
# Standalone build (not part of the main entropic build):
#   cmake -B build -DCMAKE_PREFIX_PATH=/path/to/entropic/install
#   cmake --build build
#
# Or, if librentropic is in the main build tree:
#   cmake -B build -DENTROPIC_LIB_DIR=../../build/full
#   cmake --build build

cmake_minimum_required(VERSION 3.21)
project(headless C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(headless main.c)

if(DEFINED ENTROPIC_LIB_DIR)
    target_link_directories(headless PRIVATE ${ENTROPIC_LIB_DIR})
endif()

if(DEFINED ENTROPIC_INCLUDE_DIR)
    target_include_directories(headless PRIVATE ${ENTROPIC_INCLUDE_DIR})
else()
    target_include_directories(headless PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../include)
endif()

if(DEFINED ENTROPIC_BUILD_INCLUDE_DIR)
    target_include_directories(headless PRIVATE ${ENTROPIC_BUILD_INCLUDE_DIR})
endif()

# v2.0.5: sublibs are OBJECT-absorbed into the facade — only
# librentropic.so exists. Link the facade only.
target_link_libraries(headless PRIVATE rentropic)
