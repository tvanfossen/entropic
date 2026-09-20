## Change Impact Report

| REQ | Name | Functions Changed |
|-----|------|-------------------|
| REQ-ABI-001 | Pure C at every .so boundary — opaque handles and explicit ownership | entropic_destroy, entropic_context_count, entropic_context_usage, entropic_state_save, entropic_state_load, entropic_free_logprob_result |
| REQ-ABI-002 | C++ exceptions never cross any .so boundary | entropic_run_batch, entropic_state_save, entropic_state_load, entropic_list_identities, entropic_get_logprobs, entropic_compute_perplexity, entropic_workspace_create, entropic_session_bind_workspace, entropic_register_mcp_server |
| REQ-API-002 | Handle lifecycle — create, configure, destroy, NULL-safe teardown | entropic_destroy |
| REQ-API-003 | Multiple handles are fully independent within one process | entropic_destroy |
| REQ-API-004 | Three configure entry points, one shared body, one-shot per handle | init_orchestrator, validate_prompt_sources, configure_common |
| REQ-API-005 | Uniform precondition guard on every exported entry point | entropic_run_batch, entropic_set_critique_callbacks, entropic_context_get, entropic_context_count, entropic_context_usage, entropic_state_save, entropic_state_load, entropic_metrics_json, entropic_list_identities, entropic_identity_count, entropic_get_logprobs, entropic_compute_perplexity, entropic_set_residency_observer, entropic_workspace_create, entropic_session_bind_workspace, entropic_register_mcp_server |
| REQ-API-008 | Single cross-boundary allocator pair and explicit ownership transfer | entropic_run_batch, entropic_context_get, entropic_metrics_json, entropic_list_identities, entropic_get_logprobs, entropic_free_logprob_result |
| REQ-API-009 | Run entry-point family, result contract, and cross-thread interruptibility | entropic_run_batch, entropic_run_session, entropic_run_session_as, entropic_run_session_streaming |
| REQ-API-010 | Observer and callback slots survive configure and fire uniformly | entropic_set_critique_callbacks, entropic_set_residency_observer |
| REQ-API-013 | Identity frontmatter threaded into config before the orchestrator snapshot | wire_tool_executor, init_orchestrator |
| REQ-DELEG-005 | Opt-in delegation sandbox isolation, wired end to end | facade_session_root, facade_swap_tool_dir, set_working_dir_all, tools_without_readonly_hint |
| REQ-DELEG-006 | Explicit delegation context, required-context tiers, resume by target | si_latest_delegation_for_target, extract_context_refs |
| REQ-IDEN-001 | Tier resolution contract and per-tier loop overrides | entropic_run_session_as |
| REQ-IDEN-002 | Identity lifecycle separates immutable static from dynamic identities | entropic_list_identities, entropic_identity_count |
| REQ-INFER-007 | Named grammars are registered, validated, and resolved by a fixed precedence | warn_tier_grammars_step |
| REQ-INFER-018 | Same-prefix batch generation is gated, per-request-constrained and seq-safe | entropic_run_batch |
| REQ-INFER-019 | Model pool dedup and VRAM residency policy govern which tier is resident | require_ready_backend, entropic_set_residency_observer |
| REQ-INFER-024 | Backends declare capabilities and expose state and log-probability introspection | entropic_get_logprobs, entropic_compute_perplexity |
| REQ-LOOP-001 | Agent state machine with observable, dual-channel transitions | entropic_run_session, entropic_session_context_get |
| REQ-LOOP-006 | Interrupt and pause semantics across nested loops | RunCancelScope, current_run_cancel, current_run_cancelled, RunCancelScope, cancel_current_run, ~RunCancelScope |
| REQ-LOOP-009 | Per-session-key run concurrency, on by default, with one lock on the generation path | RunSessionScope, current_run_session, RunSessionScope, ~RunSessionScope |
| REQ-MCP-001 | MCPServerBase holds the shared logic; concrete servers override only deltas | init_builtins, set_working_dir_all |
| REQ-MCP-002 | Tool results always cross boundaries as a ServerResponse JSON envelope | execute_tool, build_directive |
| REQ-MCP-007 | Tool calls route by server prefix uniformly across all three server kinds | init_builtins, tools_without_readonly_hint, external_tools_without_readonly_hint |
| REQ-MCP-012 | Tool calls pass an ordered precondition pipeline with typed rejection kinds | process_single_call |
| REQ-MCP-014 | A locked tier's allowed_tools list is enforced at dispatch time | wire_tool_executor |
| REQ-MCP-017 | PRE/POST_TOOL_CALL hooks fire on every exit path and POST may rewrite the result | process_single_call |
| REQ-MCP-020 | A bounded, thread-safe ring buffer retains recent tool calls for introspection | execute_tool |
| REQ-MCP-024 | Engine-level entropic.* tools validate their arguments and emit typed directives | build_complete_directive, build_directive |
| REQ-MCP-025 | External servers are discovered safely, directive-stripped, and health-monitored | entropic_register_mcp_server, make_transport |
| REQ-MCP-027 | Named workspaces bind a session to its own tool root and servers | set_working_dir, workspace_for, workspace_servers, build_workspace, validate_workspace_args, entropic_workspace_create, entropic_session_bind_workspace, resolve_registration_target, set_server_resolver, servers_for, execute_tool |
| REQ-SAFE-001 | Untrusted bytes are sanitized at ingress, never at egress | entropic_context_get |
| REQ-VALID-002 | Critique-and-revise loop with typed verdicts and content safety valve | entropic_set_critique_callbacks |

**Total: 33 requirement(s) affected, 105 function(s) changed**
