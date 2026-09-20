## Change Impact Report

| REQ | Name | Functions Changed |
|-----|------|-------------------|
| REQ-ABI-001 | Pure C at every .so boundary — opaque handles and explicit ownership | entropic_context_usage, entropic_state_save, entropic_state_load |
| REQ-ABI-002 | C++ exceptions never cross any .so boundary | entropic_state_save, entropic_state_load, entropic_update_identity, entropic_get_identity_config, entropic_list_identities, entropic_get_logprobs |
| REQ-API-004 | Three configure entry points, one shared body, one-shot per handle | reject_if_configured, init_orchestrator, validate_prompt_sources |
| REQ-API-005 | Uniform precondition guard on every exported entry point | entropic_run_as, entropic_set_delegation_callbacks, entropic_set_state_observer, entropic_set_queue_observer, entropic_context_get, entropic_context_usage, entropic_state_save, entropic_state_load, entropic_metrics_json, entropic_update_identity, entropic_destroy_identity, entropic_get_identity_config, entropic_list_identities, entropic_identity_count, entropic_get_logprobs, entropic_get_diagnostic_prompt, entropic_speculative_compat |
| REQ-API-008 | Single cross-boundary allocator pair and explicit ownership transfer | entropic_run_as, entropic_context_get, entropic_metrics_json, entropic_get_identity_config, entropic_list_identities, entropic_get_logprobs, entropic_get_diagnostic_prompt, entropic_speculative_compat |
| REQ-API-009 | Run entry-point family, result contract, and cross-thread interruptibility | try_begin_turn, claim, entropic_run_as, entropic_run_session, entropic_run_session_as, entropic_run_session_streaming |
| REQ-API-010 | Observer and callback slots survive configure and fire uniformly | entropic_run_as, entropic_set_delegation_callbacks, entropic_set_state_observer, entropic_set_queue_observer |
| REQ-API-013 | Identity frontmatter threaded into config before the orchestrator snapshot | init_orchestrator |
| REQ-BRIDGE-001 | External bridge exposes the engine over a peer-authenticated unix socket | start_external_bridge |
| REQ-CFG-001 | Layered configuration resolution with most-specific-layer-wins precedence | parse_models_config, parse_config_string |
| REQ-CFG-002 | Whole-block replace semantics for models and routing across layers | parse_models_config |
| REQ-CFG-003 | Bundled model registry resolution — keys, selectors, and search order | parse_model_config |
| REQ-CFG-005 | Per-tier knobs override global defaults without an all-or-nothing switch | parse_gpu_layers, parse_model_config |
| REQ-CFG-006 | Fail-loud validation — reject bad or inert configuration at load time | parse_config_string |
| REQ-COMPACT-002 | Fill-gated tool-result pruning and persistent context anchors | run_loop, run, reinject_context_anchors, dir_anchor |
| REQ-DELEG-001 | Delegation admission guards reject before a child loop runs | push_delegation_isolation_unsafe, reject_delegation_if_guarded, reject_pipeline_if_guarded, execute_pending_pipeline |
| REQ-DELEG-002 | Delegation lifecycle from preconditions to result fold-back | check_delegation_preconditions, execute_delegation, execute_resume_delegation, fetch_resume_payload, resolve_resume_delegation, run_pending_delegation, ensure_sandbox_manager, resolve_session_root, entropic_set_delegation_callbacks |
| REQ-DELEG-004 | Sequential pipeline with forward carry and per-stage reporting | execute_pipeline, run_pipeline_stage, reject_pipeline_if_guarded, execute_pending_pipeline |
| REQ-DELEG-005 | Opt-in delegation sandbox isolation, wired end to end | parse_delegation_config, mint_delegation_id, restore_root_for, push_delegation_isolation_unsafe, resolve_session_root, sandbox_for_session, isolation_unsafe_tools, next_delegation_id, facade_session_root, facade_swap_tool_dir, set_working_dir_all, tools_without_readonly_hint |
| REQ-HOOK-002 | Engine fires hooks at every lifecycle boundary, including a vetoable completion | loop |
| REQ-IDEN-001 | Tier resolution contract and per-tier loop overrides | entropic_run_session_as |
| REQ-IDEN-002 | Identity lifecycle separates immutable static from dynamic identities | entropic_update_identity, entropic_destroy_identity, entropic_get_identity_config, entropic_list_identities, entropic_identity_count |
| REQ-IDEN-003 | Handoff rules and auto-chain continuation | should_auto_chain, try_auto_chain |
| REQ-INFER-016 | Speculative decoding is gated on compatibility and must not change the output | entropic_speculative_compat |
| REQ-INFER-019 | Model pool dedup and VRAM residency policy govern which tier is resident | require_ready_backend |
| REQ-INFER-024 | Backends declare capabilities and expose state and log-probability introspection | entropic_get_logprobs |
| REQ-LOOP-001 | Agent state machine with observable, dual-channel transitions | run_loop, run, run_turn, run_streaming, run_streaming, entropic_set_state_observer, entropic_run_session, entropic_session_context_get, entropic_session_drop, entropic_session_list |
| REQ-LOOP-002 | Bounded loop termination with synthetic completion on cap | run, loop |
| REQ-LOOP-003 | Fixed per-iteration pipeline with pre-generate cancellation | dispatch_pending_or_halt |
| REQ-LOOP-006 | Interrupt and pause semantics across nested loops | run_loop, dispatch_pending_or_halt, interrupt, reset_interrupt |
| REQ-LOOP-008 | Family-aware streaming reasoning filter with UTF-8 alignment | run_streaming, run_streaming |
| REQ-MCP-001 | MCPServerBase holds the shared logic; concrete servers override only deltas | set_working_dir_all |
| REQ-MCP-007 | Tool calls route by server prefix uniformly across all three server kinds | tools_without_readonly_hint, external_tools_without_readonly_hint |
| REQ-SAFE-001 | Untrusted bytes are sanitized at ingress, never at egress | entropic_context_get |
| REQ-TYPE-005 | Config types declare their own defaults; optional means absent, not default | parse_gpu_layers, parse_model_config |

**Total: 35 requirement(s) affected, 126 function(s) changed**
