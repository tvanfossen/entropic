# SPDX-License-Identifier: LGPL-3.0-or-later
# v1.8.7: External MCP integration tests
add_executable(entropic-integration-tests
    test_external_mcp.cpp
)

target_include_directories(entropic-integration-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Pass mock server binary path as compile-time define
target_compile_definitions(entropic-integration-tests PRIVATE
    MOCK_MCP_SERVER_PATH="$<TARGET_FILE:mock-mcp-server>"
)

target_link_libraries(entropic-integration-tests PRIVATE
    entropic-mcp
    entropic-core
    entropic-types
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    Catch2::Catch2WithMain
)

# Ensure mock server is built before integration tests
add_dependencies(entropic-integration-tests mock-mcp-server)

# v1.8.8: Storage integration tests
if(ENTROPIC_STORAGE_SQLITE)
    add_executable(entropic-storage-integration-tests
        test_storage_integration.cpp
    )

    target_link_libraries(entropic-storage-integration-tests PRIVATE
        entropic-storage
        entropic-core
        entropic-types
        nlohmann_json::nlohmann_json
        spdlog::spdlog
        Catch2::Catch2WithMain
    )
endif()

include(Catch)
catch_discover_tests(entropic-integration-tests)
if(ENTROPIC_STORAGE_SQLITE)
    catch_discover_tests(entropic-storage-integration-tests)
endif()
