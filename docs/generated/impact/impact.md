## Change Impact Report

| REQ | Name | Functions Changed |
|-----|------|-------------------|
| REQ-ABI-002 | C++ exceptions never cross any .so boundary | entropic_workspace_create, entropic_session_bind_workspace, entropic_register_mcp_server |
| REQ-API-005 | Uniform precondition guard on every exported entry point | entropic_workspace_create, entropic_session_bind_workspace, entropic_register_mcp_server |
| REQ-MCP-021 | Filesystem tools are root-confined, read-before-write gated, and size-bounded | confined_mcp_config |
| REQ-MCP-025 | External servers are discovered safely, directive-stripped, and health-monitored | entropic_register_mcp_server |
| REQ-MCP-027 | Named workspaces bind a session to its own tool root and servers | confined_mcp_config, build_workspace, validate_workspace_args, entropic_workspace_create, entropic_session_bind_workspace, resolve_registration_target |

**Total: 5 requirement(s) affected, 14 function(s) changed**
