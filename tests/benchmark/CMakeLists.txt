# v1.9.14: Benchmark harness — performance measurement infrastructure
#
# CTest label "benchmark" for independent execution:
#   ctest -L benchmark
#
# Deferred: actual inference benchmarks need a live model and GPU.
# This CMakeLists.txt establishes the targets and measurement utilities.
# Source files will be populated when benchmark runners are implemented.

# Placeholder: no source files yet. Uncomment when benchmark harness is written.
# add_executable(entropic-benchmark
#     bench_inference.cpp
#     bench_engine_overhead.cpp
#     bench_memory_profile.cpp
# )
#
# target_link_libraries(entropic-benchmark PRIVATE
#     entropic-facade
#     entropic-core
#     entropic-types
#     nlohmann_json::nlohmann_json
#     spdlog::spdlog
#     Catch2::Catch2WithMain
# )
#
# # Label as "benchmark" for CTest filtering
# catch_discover_tests(entropic-benchmark
#     PROPERTIES LABELS "benchmark"
# )
