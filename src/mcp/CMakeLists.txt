# SPDX-License-Identifier: LGPL-3.0-or-later
# librentropic-mcp.so — base + registry + executor + permissions + manager
add_library(entropic-mcp OBJECT)

target_sources(entropic-mcp PRIVATE
    tool_base.cpp
    tool_registry.cpp
    server_base.cpp
    permission_manager.cpp
    server_manager.cpp
    tool_executor.cpp
    mcp_c_api.cpp
    # v1.9.4: MCP authorization
    mcp_key_set.cpp
    mcp_authorization.cpp
    # v1.9.12: Tool call history ring buffer
    tool_call_history.cpp
    # Built-in servers (compiled into mcp.so for now; individual .so later)
    servers/filesystem.cpp
    servers/ignore_matcher.cpp
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
    # v2.1.0: tool-result content classifiers (#44)
    tool_result_classify.cpp
)
# v2.1.1 (#3): utf8_sanitize.cpp relocated to entropic-core's source list.
# It was originally added in v2.1.0 (#47) as an MCP-only utility, but the
# v2.1.1 boundary policy (see include/entropic/mcp/utf8_sanitize.h) makes
# it a callsite for entropic-core (response_generator) and entropic-facade
# (json_serializers, audit replay) too. Compiling it in entropic-core
# keeps the layering clean — core does not depend on mcp; mcp depends on
# core. Header location and the entropic::mcp:: namespace are preserved
# for ABI/source compatibility.

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
    CXX_VISIBILITY_PRESET default
)
