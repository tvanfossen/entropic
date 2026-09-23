## Change Impact Report

| REQ | Name | Functions Changed |
|-----|------|-------------------|
| REQ-LOOP-009 | Per-session-key run concurrency, on by default, with one lock on the generation path | execute_tool |
| REQ-MCP-002 | Tool results always cross boundaries as a ServerResponse JSON envelope | execute_tool |
| REQ-MCP-012 | Tool calls pass an ordered precondition pipeline with typed rejection kinds | check_call_preconditions, process_single_call |
| REQ-MCP-015 | Duplicates, errors, and denials return corrective guidance, not bare failure | record_tool_call, finalize_tool_call, check_repeated_failure |
| REQ-MCP-016 | Anti-spiral tracking warns softly then hard-blocks repeated identical tools | check_repeated_failure, check_spiral_blocks |
| REQ-MCP-017 | PRE/POST_TOOL_CALL hooks fire on every exit path, PRE may rewrite the args and POST the result | process_single_call, finalize_tool_call, apply_pre_tool_modification, fire_pre_tool_hook, build_pre_tool_json |
| REQ-MCP-019 | Tool results are size-capped with a marker and classified into typed kinds | classify_tool_result, finalize_tool_call |
| REQ-MCP-020 | A bounded, thread-safe ring buffer retains recent tool calls for introspection | execute_tool |
| REQ-MCP-024 | Engine-level entropic.* tools validate their arguments and emit typed directives | build_complete_directive |
| REQ-MCP-027 | Named workspaces bind a session to its own tool root and servers | set_server_resolver, servers_for, execute_tool |

**Total: 10 requirement(s) affected, 21 function(s) changed**
