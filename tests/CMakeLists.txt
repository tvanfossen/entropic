add_subdirectory(unit)
add_subdirectory(helpers)
add_subdirectory(integration)
add_subdirectory(edge)
add_subdirectory(concurrency)

# Stress tests: too slow for pre-commit. Separate CTest label.
add_subdirectory(stress)

# Benchmark harness: measurement infrastructure, no live model runs.
add_subdirectory(benchmark)
