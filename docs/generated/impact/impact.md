## Change Impact Report

| REQ | Name | Functions Changed |
|-----|------|-------------------|
| REQ-ABI-001 | Pure C at every .so boundary — opaque handles and explicit ownership | entropic_destroy, entropic_context_usage, entropic_state_save, entropic_state_load |
| REQ-ABI-002 | C++ exceptions never cross any .so boundary | entropic_run_batch, entropic_state_save, entropic_state_load, entropic_adapter_load, entropic_get_logprobs, entropic_compute_perplexity, entropic_model_has_vision, entropic_session_bind_workspace |
| REQ-API-002 | Handle lifecycle — create, configure, destroy, NULL-safe teardown | entropic_destroy |
| REQ-API-003 | Multiple handles are fully independent within one process | entropic_destroy |
| REQ-API-004 | Three configure entry points, one shared body, one-shot per handle | validate_prompt_sources, configure_common |
| REQ-API-005 | Uniform precondition guard on every exported entry point | entropic_run_batch, entropic_interrupt, entropic_context_usage, entropic_state_save, entropic_state_load, entropic_metrics_json, entropic_adapter_load, entropic_identity_count, entropic_get_logprobs, entropic_compute_perplexity, entropic_model_has_vision, entropic_set_residency_observer, entropic_release_model, entropic_set_path_approval_callback, entropic_session_bind_workspace |
| REQ-API-008 | Single cross-boundary allocator pair and explicit ownership transfer | entropic_run_batch, entropic_metrics_json, entropic_get_logprobs |
| REQ-API-009 | Run entry-point family, result contract, and cross-thread interruptibility | claim, entropic_run_batch, entropic_interrupt, entropic_interrupt_session, entropic_run_session, entropic_run_session_as, entropic_run_session_streaming |
| REQ-API-010 | Observer and callback slots survive configure and fire uniformly | entropic_set_residency_observer, entropic_set_path_approval_callback |
| REQ-API-013 | Identity frontmatter threaded into config before the orchestrator snapshot | wire_tool_executor |
| REQ-CFG-001 | Layered configuration resolution with most-specific-layer-wins precedence | parse_models_config, load_project_layer, load_layered |
| REQ-CFG-002 | Whole-block replace semantics for models and routing across layers | parse_models_config, load_project_layer, load_layered |
| REQ-CFG-003 | Bundled model registry resolution — keys, selectors, and search order | parse_model_config |
| REQ-CFG-005 | Per-tier knobs override global defaults without an all-or-nothing switch | parse_gpu_layers, parse_model_config, parse_tier_config, parse_speculative_config |
| REQ-CFG-007 | Usable configuration with no user config present | load_layered |
| REQ-DELEG-005 | Opt-in delegation sandbox isolation, wired end to end | parse_delegation_config, facade_session_root, facade_swap_tool_dir, set_working_dir_all, tools_without_readonly_hint |
| REQ-DELEG-006 | Explicit delegation context, required-context tiers, resume by target | si_latest_delegation_for_target |
| REQ-IDEN-001 | Tier resolution contract and per-tier loop overrides | entropic_run_session_as |
| REQ-IDEN-002 | Identity lifecycle separates immutable static from dynamic identities | entropic_identity_count |
| REQ-INFER-007 | Named grammars are registered, validated, and resolved by a fixed precedence | warn_tier_grammars_step |
| REQ-INFER-018 | Same-prefix batch generation is gated, per-request-constrained and seq-safe | entropic_run_batch |
| REQ-INFER-019 | Model pool dedup and VRAM residency policy govern which tier is resident | require_ready_backend, entropic_set_residency_observer, entropic_release_model |
| REQ-INFER-023 | LoRA adapters hot-swap under a single-HOT-per-context constraint | entropic_adapter_load |
| REQ-INFER-024 | Backends declare capabilities and expose state and log-probability introspection | entropic_get_logprobs, entropic_compute_perplexity |
| REQ-INFER-025 | Multimodal input is bounded, tier-gated and degrades gracefully | entropic_model_has_vision |
| REQ-LOOP-001 | Agent state machine with observable, dual-channel transitions | entropic_run_session, entropic_session_context_get |
| REQ-LOOP-006 | Interrupt and pause semantics across nested loops | entropic_interrupt, entropic_interrupt_session |
| REQ-LOOP-010 | Session conversations are readable, writable, and losslessly round-trippable | entropic_session_context_set |
| REQ-MCP-001 | MCPServerBase holds the shared logic; concrete servers override only deltas | init_builtins, set_working_dir_all, FilesystemServer |
| REQ-MCP-007 | Tool calls route by server prefix uniformly across all three server kinds | init_builtins, tools_without_readonly_hint, external_tools_without_readonly_hint |
| REQ-MCP-014 | A locked tier's allowed_tools list is enforced at dispatch time | wire_tool_executor |
| REQ-MCP-021 | Filesystem tools are root-confined, read-before-write gated, and size-bounded | parse_outside_root_access, parse_outside_root_list, confined_mcp_config, outside_root_thunk, wire_outside_root_approver, entropic_set_path_approval_callback, set_outside_root_approver, path_within, first_containing, outside_root_refusal, refuse_denied, refuse_escape, check_read_before_write, execute, execute, execute, FilesystemServer, set_working_dir, resolve_path, authorize_outside_root, ask_outside_root_approver, set_outside_root_approver, log_outside_root_policy |
| REQ-MCP-022 | Glob, grep, and read honour .gitignore plus .explorerignore semantics | FilesystemServer, set_working_dir |
| REQ-MCP-025 | External servers are discovered safely, directive-stripped, and health-monitored | connect_and_register_external, make_transport |
| REQ-MCP-027 | Named workspaces bind a session to its own tool root and servers | workspace_for, workspace_servers, confined_mcp_config, build_workspace, validate_workspace_args, entropic_session_bind_workspace |
| REQ-TYPE-005 | Config types declare their own defaults; optional means absent, not default | parse_gpu_layers, parse_model_config, parse_tier_config, parse_speculative_config |

**Total: 36 requirement(s) affected, 122 function(s) changed**
