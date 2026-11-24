## Change Impact Report

| REQ | Name | Functions Changed |
|-----|------|-------------------|
| REQ-LOOP-009 | Per-session-key run concurrency, on by default, with one lock on the generation path | execute_tool |
| REQ-MCP-001 | MCPServerBase holds the shared logic; concrete servers override only deltas | set_working_dir |
| REQ-MCP-002 | Tool results always cross boundaries as a ServerResponse JSON envelope | disconnected_error, parse_tool_result_text, execute_tool |
| REQ-MCP-007 | Tool calls route by server prefix uniformly across all three server kinds | connect_external_server |
| REQ-MCP-009 | Operator permission gate with deny precedence and server-derived patterns | args_to_pattern, check_approval |
| REQ-MCP-010 | Per-identity MCP key authorization is opt-in but default-deny once enabled | check_mcp_authorization |
| REQ-MCP-011 | Every tool declares its required access level, defaulting to WRITE | check_mcp_authorization |
| REQ-MCP-012 | Tool calls pass an ordered precondition pipeline with typed rejection kinds | requires_approval, requires_approval, requires_approval, check_approval, check_mcp_authorization |
| REQ-MCP-013 | Tool arguments are validated against the declared JSON schema before dispatch | normalize_args |
| REQ-MCP-015 | Duplicates, errors, and denials return corrective guidance, not bare failure | create_denied_message, create_error_message, create_duplicate_message |
| REQ-MCP-020 | A bounded, thread-safe ring buffer retains recent tool calls for introspection | execute_tool |
| REQ-MCP-024 | Engine-level entropic.* tools validate their arguments and emit typed directives | followup_results |
| REQ-MCP-025 | External servers are discovered safely, directive-stripped, and health-monitored | connect_declared_servers, connect_discovered_servers, initialize_external_servers, connect_declared_servers, connect_discovered_servers, connect_external_server, disconnected_error |
| REQ-MCP-027 | Named workspaces bind a session to its own tool root and servers | execute_tool |

**Total: 14 requirement(s) affected, 29 function(s) changed**
