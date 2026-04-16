# SPDX-License-Identifier: LGPL-3.0-or-later
# Python ctypes wrapper generation from entropic.h
#
# Runs scripts/generate_wrapper.py against the C API header to produce
# a Python module with ctypes bindings. The generated module is placed
# in the build tree at python/entropic_native/__init__.py.
#
# Package name is entropic_native (not entropic) to avoid collision with
# src/entropic/ during the v1.9.0→v1.9.15 transition window.
#
# @brief CMake integration for auto-generated Python wrapper.
# @version 2

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(WRAPPER_OUTPUT "${CMAKE_BINARY_DIR}/python/entropic_native/__init__.py")
set(WRAPPER_GENERATOR "${CMAKE_SOURCE_DIR}/scripts/generate_wrapper.py")
set(ENTROPIC_HEADER "${CMAKE_SOURCE_DIR}/include/entropic/entropic.h")
set(TYPES_DIR "${CMAKE_SOURCE_DIR}/include/entropic/types")

add_custom_command(
    OUTPUT ${WRAPPER_OUTPUT}
    COMMAND ${Python3_EXECUTABLE} ${WRAPPER_GENERATOR}
        --header ${ENTROPIC_HEADER}
        --types-dir ${TYPES_DIR}
        --output ${WRAPPER_OUTPUT}
        --lib-name librentropic.so
    DEPENDS
        ${ENTROPIC_HEADER}
        ${TYPES_DIR}/error.h
        ${TYPES_DIR}/hooks.h
        ${WRAPPER_GENERATOR}
    COMMENT "Generating Python ctypes wrapper from entropic.h"
)

add_custom_target(python_wrapper ALL DEPENDS ${WRAPPER_OUTPUT})
add_dependencies(python_wrapper entropic-facade)

# Python wrapper install disabled until v2.1.1 — Python3_SITEARCH
# resolves to the system interpreter's site-packages, not the venv.
# Wrapper generation still runs (build-tree only). Tracked in the
# v2.1.1 proposal as a re-enable item with proper venv detection.
#
# install(
#     DIRECTORY ${CMAKE_BINARY_DIR}/python/entropic_native
#     DESTINATION ${Python3_SITEARCH}
# )
# install(
#     FILES
#         ${CMAKE_SOURCE_DIR}/python/entropic_native/py.typed
#     DESTINATION ${Python3_SITEARCH}/entropic_native
# )
# install(
#     TARGETS entropic-facade
#     LIBRARY DESTINATION ${Python3_SITEARCH}/entropic_native/lib
# )
