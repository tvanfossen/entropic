## Change Impact Report

| REQ | Name | Functions Changed |
|-----|------|-------------------|
| REQ-ABI-001 | Pure C at every .so boundary — opaque handles and explicit ownership | entropic_configure_dir, entropic_destroy |
| REQ-ABI-002 | C++ exceptions never cross any .so boundary | entropic_configure_dir |
| REQ-API-002 | Handle lifecycle — create, configure, destroy, NULL-safe teardown | entropic_destroy |
| REQ-API-003 | Multiple handles are fully independent within one process | entropic_destroy |
| REQ-API-004 | Three configure entry points, one shared body, one-shot per handle | configure_common, entropic_configure_dir |
| REQ-API-005 | Uniform precondition guard on every exported entry point | entropic_configure_dir |
| REQ-API-009 | Run entry-point family, result contract, and cross-thread interruptibility | entropic_run_session_as, entropic_run_session_streaming, entropic_run_session_streaming_as |
| REQ-BRIDGE-001 | External bridge exposes the engine over a peer-authenticated unix socket | start_http_listener, start_external_bridge |
| REQ-CFG-001 | Layered configuration resolution with most-specific-layer-wins precedence | entropic_configure_dir |
| REQ-DELEG-006 | Explicit delegation context, required-context tiers, resume by target | execute, pipeline_rejection |
| REQ-IDEN-001 | Tier resolution contract and per-tier loop overrides | entropic_run_session_as, entropic_run_session_streaming_as |
| REQ-INFER-007 | Named grammars are registered, validated, and resolved by a fixed precedence | warn_tier_grammars_step |
| REQ-LOOP-009 | Per-session-key run concurrency, on by default, with one lock on the generation path | execute |
| REQ-MCP-002 | Tool results always cross boundaries as a ServerResponse JSON envelope | build_response |
| REQ-MCP-007 | Tool calls route by server prefix uniformly across all three server kinds | query_tools |
| REQ-MCP-013 | Tool arguments are validated against the declared JSON schema before dispatch | render_response_format, load_tool_definition, check_range, check_property_constraints |
| REQ-MCP-015 | Duplicates, errors, and denials return corrective guidance, not bare failure | tool_error_json, record_tool_call |
| REQ-MCP-020 | A bounded, thread-safe ring buffer retains recent tool calls for introspection | record_tool_history |
| REQ-MCP-021 | Filesystem tools are root-confined, read-before-write gated, and size-bounded | read_numbered_lines, apply_insert, collect_glob_matches, collect_entries, do_str_replace, do_insert, apply_edit, check_read_gates, execute, execute, execute, execute |
| REQ-MCP-022 | Glob, grep, and read honour .gitignore plus .explorerignore semantics | read_page, collect_glob_matches, grep_file, check_read_gates, execute, grep_search, execute |
| REQ-MCP-024 | Engine-level entropic.* tools validate their arguments and emit typed directives | execute, execute, pipeline_rejection, execute, execute, execute, execute, execute |
| REQ-MCP-025 | External servers are discovered safely, directive-stripped, and health-monitored | send_initialize, query_tools, build_response, set_init_timeout_ms |
| REQ-MCP-026 | A JSON-RPC response is paired with the request that asked for it | send_initialize, query_tools |

**Total: 23 requirement(s) affected, 62 function(s) changed**
