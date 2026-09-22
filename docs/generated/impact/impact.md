## Change Impact Report

| REQ | Name | Functions Changed |
|-----|------|-------------------|
| REQ-LOOP-009 | Per-session-key run concurrency, on by default, with one lock on the generation path | execute_tool |
| REQ-MCP-002 | Tool results always cross boundaries as a ServerResponse JSON envelope | execute_tool |
| REQ-MCP-012 | Tool calls pass an ordered precondition pipeline with typed rejection kinds | process_single_call |
| REQ-MCP-017 | PRE/POST_TOOL_CALL hooks fire on every exit path, PRE may rewrite the args and POST the result | process_single_call, apply_pre_tool_modification, fire_pre_tool_hook, build_post_tool_json, build_pre_tool_json |
| REQ-MCP-019 | Tool results are size-capped with a marker and classified into typed kinds | classify_tool_result |
| REQ-MCP-020 | A bounded, thread-safe ring buffer retains recent tool calls for introspection | execute_tool |
| REQ-MCP-027 | Named workspaces bind a session to its own tool root and servers | set_server_resolver, servers_for, execute_tool |

**Total: 7 requirement(s) affected, 13 function(s) changed**
