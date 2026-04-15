# SPDX-License-Identifier: LGPL-3.0-or-later
# Mock MCP server binary for integration tests
add_executable(mock-mcp-server
    mock_mcp_server.cpp
)

target_link_libraries(mock-mcp-server PRIVATE
    nlohmann_json::nlohmann_json
)
