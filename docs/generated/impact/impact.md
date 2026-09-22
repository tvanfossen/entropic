## Change Impact Report

| REQ | Name | Functions Changed |
|-----|------|-------------------|
| REQ-ABI-001 | Pure C at every .so boundary — opaque handles and explicit ownership | entropic_destroy, entropic_context_clear, entropic_context_usage, entropic_state_save, entropic_state_load, entropic_free_logprob_result |
| REQ-ABI-002 | C++ exceptions never cross any .so boundary | entropic_run_batch, entropic_state_save, entropic_state_load, entropic_adapter_load, entropic_list_identities, entropic_get_logprobs, entropic_compute_perplexity, entropic_workspace_create, entropic_session_bind_workspace, entropic_register_mcp_server |
| REQ-API-002 | Handle lifecycle — create, configure, destroy, NULL-safe teardown | entropic_destroy |
| REQ-API-003 | Multiple handles are fully independent within one process | entropic_destroy |
| REQ-API-004 | Three configure entry points, one shared body, one-shot per handle | validate_prompt_sources, configure_common |
| REQ-API-005 | Uniform precondition guard on every exported entry point | entropic_run_batch, entropic_context_clear, entropic_context_usage, entropic_state_save, entropic_state_load, entropic_metrics_json, entropic_adapter_load, entropic_list_identities, entropic_identity_count, entropic_get_logprobs, entropic_compute_perplexity, entropic_set_residency_observer, entropic_release_model, entropic_set_path_approval_callback, entropic_workspace_create, entropic_session_bind_workspace, entropic_register_mcp_server |
| REQ-API-008 | Single cross-boundary allocator pair and explicit ownership transfer | entropic_run_batch, entropic_metrics_json, entropic_list_identities, entropic_get_logprobs, entropic_free_logprob_result |
| REQ-API-009 | Run entry-point family, result contract, and cross-thread interruptibility | claim, entropic_run_batch, entropic_interrupt_session, entropic_run_session, entropic_run_session_as, entropic_run_session_streaming |
| REQ-API-010 | Observer and callback slots survive configure and fire uniformly | entropic_set_residency_observer, entropic_set_path_approval_callback |
| REQ-API-013 | Identity frontmatter threaded into config before the orchestrator snapshot | wire_tool_executor |
| REQ-DELEG-005 | Opt-in delegation sandbox isolation, wired end to end | facade_session_root, swap_session_tool_dir, set_working_dir_all, enter_working_dir, leave_working_dir, tools_without_readonly_hint |
| REQ-DELEG-006 | Explicit delegation context, required-context tiers, resume by target | si_latest_delegation_for_target, DelegateTool, execute, pipeline_rejection, execute, extract_context_refs |
| REQ-IDEN-001 | Tier resolution contract and per-tier loop overrides | entropic_run_session_as |
| REQ-IDEN-002 | Identity lifecycle separates immutable static from dynamic identities | entropic_list_identities, entropic_identity_count |
| REQ-INFER-007 | Named grammars are registered, validated, and resolved by a fixed precedence | warn_tier_grammars_step |
| REQ-INFER-018 | Same-prefix batch generation is gated, per-request-constrained and seq-safe | entropic_run_batch |
| REQ-INFER-019 | Model pool dedup and VRAM residency policy govern which tier is resident | require_ready_backend, entropic_set_residency_observer, entropic_release_model |
| REQ-INFER-023 | LoRA adapters hot-swap under a single-HOT-per-context constraint | entropic_adapter_load |
| REQ-INFER-024 | Backends declare capabilities and expose state and log-probability introspection | entropic_get_logprobs, entropic_compute_perplexity |
| REQ-LOOP-001 | Agent state machine with observable, dual-channel transitions | entropic_run_session, entropic_session_context_get, entropic_session_context_clear, entropic_session_drop |
| REQ-LOOP-006 | Interrupt and pause semantics across nested loops | entropic_interrupt_session |
| REQ-LOOP-009 | Per-session-key run concurrency, on by default, with one lock on the generation path | with, peek, release, session_count, entropic_context_clear, entropic_session_context_clear, entropic_session_drop, release_session_tool_state, set_working_dir_all, enter_working_dir, leave_working_dir, release_session, route_tool_call, execute, release_session, session_count, record_read, was_read, release_session, session_count, release_session, session_count, execute_tool |
| REQ-LOOP-010 | Session conversations are readable, writable, and losslessly round-trippable | entropic_session_context_set |
| REQ-MCP-001 | MCPServerBase holds the shared logic; concrete servers override only deltas | init_builtins, set_working_dir_all, FilesystemServer |
| REQ-MCP-002 | Tool results always cross boundaries as a ServerResponse JSON envelope | execute_tool, build_directive |
| REQ-MCP-007 | Tool calls route by server prefix uniformly across all three server kinds | init_builtins, tools_without_readonly_hint, external_tools_without_readonly_hint, route_tool_call |
| REQ-MCP-008 | Plugin tools are argument-validated on the same terms as built-ins | plugin_tool_schema |
| REQ-MCP-011 | Every tool declares its required access level, defaulting to WRITE | required_access_level, required_access_level |
| REQ-MCP-012 | Tool calls pass an ordered precondition pipeline with typed rejection kinds | process_single_call |
| REQ-MCP-013 | Tool arguments are validated against the declared JSON schema before dispatch | DelegateTool, PipelineTool |
| REQ-MCP-014 | A locked tier's allowed_tools list is enforced at dispatch time | wire_tool_executor |
| REQ-MCP-015 | Duplicates, errors, and denials return corrective guidance, not bare failure | skip_duplicate_check |
| REQ-MCP-017 | PRE/POST_TOOL_CALL hooks fire on every exit path and POST may rewrite the result | process_single_call |
| REQ-MCP-019 | Tool results are size-capped with a marker and classified into typed kinds | classify_tool_result |
| REQ-MCP-020 | A bounded, thread-safe ring buffer retains recent tool calls for introspection | execute_tool |
| REQ-MCP-021 | Filesystem tools are root-confined, read-before-write gated, and size-bounded | confined_mcp_config, outside_root_thunk, wire_outside_root_approver, entropic_set_path_approval_callback, set_outside_root_approver, release_session, record_read, was_read, release_session, path_within, first_containing, outside_root_refusal, refuse_denied, refuse_escape, check_read_before_write, collect_glob_matches, check_read_gates, execute, compile_grep_or_error, compute_max_read_bytes, FilesystemServer, skip_duplicate_check, tracker, release_session, max_read_bytes, resolve_path, authorize_outside_root, ask_outside_root_approver |
| REQ-MCP-022 | Glob, grep, and read honour .gitignore plus .explorerignore semantics | classify_glob_entry, collect_glob_matches, check_read_gates, grep_search, FilesystemServer, ignore |
| REQ-MCP-024 | Engine-level entropic.* tools validate their arguments and emit typed directives | execute, DelegateTool, execute, PipelineTool, pipeline_rejection, execute, execute, register_delegation_tools, release_session, build_complete_directive, build_directive |
| REQ-MCP-025 | External servers are discovered safely, directive-stripped, and health-monitored | entropic_register_mcp_server, connect_and_register_external, make_transport |
| REQ-MCP-027 | Named workspaces bind a session to its own tool root and servers | workspace_for, workspace_servers, confined_mcp_config, build_workspace, validate_workspace_args, entropic_workspace_create, entropic_session_bind_workspace, resolve_registration_target, set_server_resolver, servers_for, execute_tool |

**Total: 40 requirement(s) affected, 182 function(s) changed**
