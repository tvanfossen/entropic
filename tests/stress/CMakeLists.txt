# SPDX-License-Identifier: LGPL-3.0-or-later
# v1.9.14: Stress tests — long-running, high-volume
#
# NOT pre-commit. Too slow (30-60 min). Separate CTest label "stress"
# so they can be run independently:
#   ctest -L stress
#
# Deferred: these tests need a live model and GPU.
# This CMakeLists.txt establishes the targets so CI can reference them.
# Test source files will be populated when the engine can run end-to-end.

# Placeholder: no source files yet. Uncomment when stress tests are written.
# add_executable(entropic-stress-tests
#     test_long_session.cpp
#     test_context_pressure.cpp
#     test_rapid_interrupt.cpp
# )
#
# target_link_libraries(entropic-stress-tests PRIVATE
#     entropic-facade
#     entropic-core
#     entropic-types
#     spdlog::spdlog
#     Catch2::Catch2WithMain
# )
#
# # Label as "stress" for CTest filtering
# catch_discover_tests(entropic-stress-tests
#     PROPERTIES LABELS "stress"
# )
