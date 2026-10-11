## Change Impact Report

| REQ | Name | Functions Changed |
|-----|------|-------------------|
| REQ-BRIDGE-001 | External bridge exposes the engine over a peer-authenticated unix socket | start_external_bridge, bridge_project_dir |
| REQ-CFG-001 | Layered configuration resolution with most-specific-layer-wins precedence | load_layered |
| REQ-CFG-002 | Whole-block replace semantics for models and routing across layers | load_layered |
| REQ-CFG-007 | Usable configuration with no user config present | load_layered |
| REQ-MCP-025 | External servers are discovered safely, directive-stripped, and health-monitored | expected_bridge_socket, wire_external_server_policy, initialize_external_servers, set_discovered_server_approver, set_own_bridge_socket |
| REQ-MCP-027 | Named workspaces bind a session to its own tool root and servers | build_workspace |

**Total: 6 requirement(s) affected, 11 function(s) changed**
