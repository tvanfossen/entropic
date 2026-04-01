add_subdirectory(unit)
add_subdirectory(helpers)
add_subdirectory(integration)
add_subdirectory(edge)
add_subdirectory(concurrency)

# Stress tests: too slow for pre-commit. Separate CTest label.
add_subdirectory(stress)

# Benchmark harness: merged into tests/model/ at v1.10.0.
# Retained as empty skeleton for backwards compat.
add_subdirectory(benchmark)

# v1.10.0: Merged model quality tests + performance benchmarks.
# Gated behind ENTROPIC_BUILD_MODEL_TESTS (OFF by default).
add_subdirectory(model)
