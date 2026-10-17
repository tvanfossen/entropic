## Change Impact Report

| REQ | Name | Functions Changed |
|-----|------|-------------------|
| REQ-INFER-019 | Model pool dedup and VRAM residency policy govern which tier is resident | host_can_hold_warm_load |
| REQ-LOOP-003 | Fixed per-iteration pipeline with pre-generate cancellation | process_generation_result |
| REQ-LOOP-009 | Per-session-key run concurrency, on by default, with one lock on the generation path | execute_tool |
| REQ-MCP-002 | Tool results always cross boundaries as a ServerResponse JSON envelope | execute_tool |
| REQ-MCP-010 | Per-identity MCP key authorization is opt-in but default-deny once enabled | check_mcp_authorization |
| REQ-MCP-011 | Every tool declares its required access level, defaulting to WRITE | check_mcp_authorization |
| REQ-MCP-012 | Tool calls pass an ordered precondition pipeline with typed rejection kinds | check_mcp_authorization |
| REQ-MCP-015 | Duplicates, errors, and denials return corrective guidance, not bare failure | create_denied_message, create_error_message, create_duplicate_message |
| REQ-MCP-020 | A bounded, thread-safe ring buffer retains recent tool calls for introspection | execute_tool |
| REQ-MCP-027 | Named workspaces bind a session to its own tool root and servers | execute_tool |

**Total: 10 requirement(s) affected, 12 function(s) changed**
