# SPDX-License-Identifier: LGPL-3.0-or-later
# Smallest-possible consumer of an installed entropic release.
#
# Used by scripts/smoke_distribution.sh and (eventually) by the Phase 3
# post-publish release smoke-test job. Confirms the consumer experience:
#
#   find_package(entropic 2.0 REQUIRED)
#   target_link_libraries(... entropic::entropic)
#
# works against a freshly installed distribution with no manual include
# or library paths, no LD_LIBRARY_PATH, and the right SOVERSION match.

cmake_minimum_required(VERSION 3.21)
project(entropic-smoke-consumer CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(entropic 2.0 REQUIRED)

add_executable(entropic-smoke smoke.cpp)
target_link_libraries(entropic-smoke PRIVATE entropic::entropic)
