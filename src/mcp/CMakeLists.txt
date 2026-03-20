# librentropic-mcp.so — base + registry + executor + permissions + manager
add_library(entropic-mcp)

target_sources(entropic-mcp PRIVATE
    tool_base.cpp
    tool_registry.cpp
    server_base.cpp
    permission_manager.cpp
    server_manager.cpp
    tool_executor.cpp
    mcp_c_api.cpp
    # Built-in servers (compiled into mcp.so for now; individual .so later)
    servers/filesystem.cpp
    servers/bash.cpp
    servers/git.cpp
    servers/diagnostics.cpp
    servers/web.cpp
    servers/entropic_server.cpp
    # v1.8.7: External MCP client
    transport_stdio.cpp
    transport_sse.cpp
    external_client.cpp
    reconnect_policy.cpp
    health_monitor.cpp
    mcp_json_discovery.cpp
)

target_include_directories(entropic-mcp PUBLIC
    ${PROJECT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/../../include
)

target_link_libraries(entropic-mcp PUBLIC
    entropic-types
    entropic-core
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    httplib::httplib
)

target_compile_definitions(entropic-mcp PRIVATE ENTROPIC_BUILDING_DLL)

set_target_properties(entropic-mcp PROPERTIES
    OUTPUT_NAME "rentropic-mcp"
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    CXX_VISIBILITY_PRESET default
)
