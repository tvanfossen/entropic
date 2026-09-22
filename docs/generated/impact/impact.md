## Change Impact Report

| REQ | Name | Functions Changed |
|-----|------|-------------------|
| REQ-ABI-001 | Pure C at every .so boundary — opaque handles and explicit ownership | entropic_destroy, entropic_api_version, entropic_seconds_since_last_activity, entropic_queue_user_message, entropic_user_message_queue_depth, entropic_state_save, entropic_free_logprob_result |
| REQ-ABI-002 | C++ exceptions never cross any .so boundary | do_configure_json, entropic_run_batch, entropic_run_streaming, entropic_state_save, entropic_adapter_load, entropic_adapter_unload, entropic_get_logprobs, entropic_compute_perplexity, entropic_model_has_vision, entropic_workspace_create |
| REQ-API-002 | Handle lifecycle — create, configure, destroy, NULL-safe teardown | entropic_destroy |
| REQ-API-003 | Multiple handles are fully independent within one process | entropic_destroy |
| REQ-API-004 | Three configure entry points, one shared body, one-shot per handle | validate_prompt_sources, configure_common, do_configure_json |
| REQ-API-005 | Uniform precondition guard on every exported entry point | entropic_seconds_since_last_activity, entropic_run_batch, entropic_run_streaming, entropic_queue_user_message, entropic_user_message_queue_depth, entropic_state_save, entropic_metrics_json, entropic_adapter_load, entropic_adapter_unload, entropic_get_logprobs, entropic_compute_perplexity, entropic_model_has_vision, entropic_validation_set_enabled, entropic_validation_set_identity, entropic_residency_snapshot, entropic_set_path_approval_callback, entropic_workspace_create |
| REQ-API-008 | Single cross-boundary allocator pair and explicit ownership transfer | entropic_run_batch, entropic_metrics_json, entropic_get_logprobs, entropic_free_logprob_result, entropic_residency_snapshot |
| REQ-API-009 | Run entry-point family, result contract, and cross-thread interruptibility | claim, entropic_run_batch, entropic_run_streaming, entropic_run_session_as, entropic_run_session_streaming |
| REQ-API-010 | Observer and callback slots survive configure and fire uniformly | entropic_run_streaming, entropic_set_path_approval_callback |
| REQ-API-013 | Identity frontmatter threaded into config before the orchestrator snapshot | wire_tool_executor |
| REQ-BRIDGE-001 | External bridge exposes the engine over a peer-authenticated unix socket | start_external_bridge |
| REQ-DELEG-005 | Opt-in delegation sandbox isolation, wired end to end | ToolRootLock, facade_session_root, swap_session_tool_dir, set_working_dir_all, enter_working_dir, leave_working_dir, lock, unlock, lock_shared |
| REQ-DELEG-006 | Explicit delegation context, required-context tiers, resume by target | si_latest_delegation_for_target |
| REQ-IDEN-001 | Tier resolution contract and per-tier loop overrides | entropic_run_session_as |
| REQ-INFER-007 | Named grammars are registered, validated, and resolved by a fixed precedence | warn_tier_grammars_step |
| REQ-INFER-018 | Same-prefix batch generation is gated, per-request-constrained and seq-safe | entropic_run_batch |
| REQ-INFER-019 | Model pool dedup and VRAM residency policy govern which tier is resident | require_ready_backend, entropic_residency_snapshot |
| REQ-INFER-023 | LoRA adapters hot-swap under a single-HOT-per-context constraint | entropic_adapter_load, entropic_adapter_unload |
| REQ-INFER-024 | Backends declare capabilities and expose state and log-probability introspection | entropic_get_logprobs, entropic_compute_perplexity |
| REQ-INFER-025 | Multimodal input is bounded, tier-gated and degrades gracefully | entropic_model_has_vision |
| REQ-LOOP-001 | Agent state machine with observable, dual-channel transitions | entropic_session_context_get, entropic_session_context_count, entropic_session_context_clear |
| REQ-LOOP-009 | Per-session-key run concurrency, on by default, with one lock on the generation path | ToolRootLock, ToolRootShared, set_working_dir_all, enter_working_dir, leave_working_dir, route_tool_call, lock, unlock, lock_shared, unlock_shared, owned_by_this_thread, ToolRootShared, ~ToolRootShared |
| REQ-LOOP-010 | Session conversations are readable, writable, and losslessly round-trippable | entropic_session_context_set |
| REQ-MCP-001 | MCPServerBase holds the shared logic; concrete servers override only deltas | init_builtins, set_working_dir_all |
| REQ-MCP-002 | Tool results always cross boundaries as a ServerResponse JSON envelope | execute |
| REQ-MCP-007 | Tool calls route by server prefix uniformly across all three server kinds | init_builtins, execute, route_tool_call, list_server_info |
| REQ-MCP-009 | Operator permission gate with deny precedence and server-derived patterns | execute |
| REQ-MCP-014 | A locked tier's allowed_tools list is enforced at dispatch time | wire_tool_executor |
| REQ-MCP-021 | Filesystem tools are root-confined, read-before-write gated, and size-bounded | confined_mcp_config, outside_root_thunk, wire_outside_root_approver, entropic_set_path_approval_callback, set_outside_root_approver |
| REQ-MCP-025 | External servers are discovered safely, directive-stripped, and health-monitored | initialize_external_servers, connect_and_register_external |
| REQ-MCP-027 | Named workspaces bind a session to its own tool root and servers | workspace_for, workspace_servers, confined_mcp_config, build_workspace, validate_workspace_args, entropic_workspace_create, resolve_registration_target |
| REQ-TYPE-002 | C ABI enums are append-only and pinned to the Python IntEnum mirrors | entropic_api_version |
| REQ-VALID-001 | Layered validation gating with cheap-path skips | entropic_validation_set_enabled, entropic_validation_set_identity |

**Total: 33 requirement(s) affected, 116 function(s) changed**
