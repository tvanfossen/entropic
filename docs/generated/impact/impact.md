## Change Impact Report

| REQ | Name | Functions Changed |
|-----|------|-------------------|
| REQ-DELEG-006 | Explicit delegation context, required-context tiers, resume by target | extract_context_refs |
| REQ-MCP-002 | Tool results always cross boundaries as a ServerResponse JSON envelope | execute_tool, build_directive |
| REQ-MCP-012 | Tool calls pass an ordered precondition pipeline with typed rejection kinds | process_single_call |
| REQ-MCP-017 | PRE/POST_TOOL_CALL hooks fire on every exit path and POST may rewrite the result | process_single_call |
| REQ-MCP-020 | A bounded, thread-safe ring buffer retains recent tool calls for introspection | execute_tool |
| REQ-MCP-024 | Engine-level entropic.* tools validate their arguments and emit typed directives | build_complete_directive, build_directive |
| REQ-MCP-027 | Named workspaces bind a session to its own tool root and servers | set_server_resolver, servers_for, execute_tool |

**Total: 7 requirement(s) affected, 11 function(s) changed**
