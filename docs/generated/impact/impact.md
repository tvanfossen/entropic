## Change Impact Report

| REQ | Name | Functions Changed |
|-----|------|-------------------|
| REQ-CFG-001 | Layered configuration resolution with most-specific-layer-wins precedence | load_layered |
| REQ-CFG-002 | Whole-block replace semantics for models and routing across layers | load_layered |
| REQ-CFG-007 | Usable configuration with no user config present | load_layered |
| REQ-MCP-002 | Tool results always cross boundaries as a ServerResponse JSON envelope | route_external_or_unknown |
| REQ-MCP-005 | Configured plugins load loudly or fail loudly — never silently skipped | load_plugins |
| REQ-MCP-006 | Plugin instances get serialised calls and ordered teardown | shutdown |
| REQ-MCP-007 | Tool calls route by server prefix uniformly across all three server kinds | query_tools, append_external_tools, external_tools_without_readonly_hint, list_tools, server_names, route_external_or_unknown, connect_external_server, list_server_info |
| REQ-MCP-025 | External servers are discovered safely, directive-stripped, and health-monitored | send_initialize, query_tools, set_init_timeout_ms, shutdown, interrupt_external_tools, clear_external_tool_interrupts, initialize_external_servers, set_discovered_server_approver, set_own_bridge_socket, connect_and_register_external, connect_external_server |
| REQ-MCP-026 | A JSON-RPC response is paired with the request that asked for it | send_initialize, query_tools |

**Total: 9 requirement(s) affected, 27 function(s) changed**
