## Change Impact Report

| REQ | Name | Functions Changed |
|-----|------|-------------------|
| REQ-ABI-001 | Pure C at every .so boundary — opaque handles and explicit ownership | entropic_context_usage, entropic_state_save, entropic_state_load, entropic_free_logprob_result |
| REQ-ABI-002 | C++ exceptions never cross any .so boundary | entropic_run_batch, entropic_state_save, entropic_state_load, entropic_adapter_load, entropic_list_identities, entropic_get_logprobs, entropic_compute_perplexity |
| REQ-API-004 | Three configure entry points, one shared body, one-shot per handle | init_orchestrator, validate_prompt_sources, configure_common |
| REQ-API-005 | Uniform precondition guard on every exported entry point | entropic_run_batch, entropic_set_critique_callbacks, entropic_interrupt, entropic_context_get, entropic_context_usage, entropic_state_save, entropic_state_load, entropic_metrics_json, entropic_adapter_load, entropic_list_identities, entropic_identity_count, entropic_get_logprobs, entropic_compute_perplexity, entropic_set_residency_observer, entropic_release_model |
| REQ-API-008 | Single cross-boundary allocator pair and explicit ownership transfer | entropic_run_batch, entropic_context_get, entropic_metrics_json, entropic_list_identities, entropic_get_logprobs, entropic_free_logprob_result |
| REQ-API-009 | Run entry-point family, result contract, and cross-thread interruptibility | try_begin_turn, entropic_run_batch, entropic_interrupt, entropic_run_session, entropic_run_session_as, entropic_run_session_streaming |
| REQ-API-010 | Observer and callback slots survive configure and fire uniformly | entropic_set_critique_callbacks, entropic_set_residency_observer |
| REQ-API-013 | Identity frontmatter threaded into config before the orchestrator snapshot | init_orchestrator |
| REQ-CFG-001 | Layered configuration resolution with most-specific-layer-wins precedence | parse_models_config, parse_config_string |
| REQ-CFG-002 | Whole-block replace semantics for models and routing across layers | parse_models_config |
| REQ-CFG-003 | Bundled model registry resolution — keys, selectors, and search order | parse_model_config |
| REQ-CFG-005 | Per-tier knobs override global defaults without an all-or-nothing switch | parse_gpu_layers, parse_model_config, parse_tier_config |
| REQ-CFG-006 | Fail-loud validation — reject bad or inert configuration at load time | parse_config_string |
| REQ-COMPACT-002 | Fill-gated tool-result pruning and persistent context anchors | run_loop, run, reinject_context_anchors, dir_anchor |
| REQ-DELEG-001 | Delegation admission guards reject before a child loop runs | push_delegation_isolation_unsafe, reject_delegation_if_guarded, reject_pipeline_if_guarded, execute_pending_pipeline |
| REQ-DELEG-002 | Delegation lifecycle from preconditions to result fold-back | check_delegation_preconditions, execute_delegation, build_resumed_child_context, execute_resume_delegation, build_child_context, dir_delegate, execute_pending_delegation, resolve_resume_delegation, run_pending_delegation, ensure_sandbox_manager, resolve_session_root |
| REQ-DELEG-003 | Relay-single-delegate promotion with coverage-gap suppression | log_relay_status |
| REQ-DELEG-004 | Sequential pipeline with forward carry and per-stage reporting | execute_pipeline, run_pipeline_stage, dir_pipeline, reject_pipeline_if_guarded, execute_pending_pipeline |
| REQ-DELEG-005 | Opt-in delegation sandbox isolation, wired end to end | parse_delegation_config, mint_delegation_id, restore_root_for, push_delegation_isolation_unsafe, resolve_session_root, sandbox_for_session, isolation_unsafe_tools, facade_session_root, facade_swap_tool_dir, set_working_dir_all, tools_without_readonly_hint |
| REQ-DELEG-006 | Explicit delegation context, required-context tiers, resume by target | format_context_block, resolve_latest_for_target, resolve_resume_delegation, si_latest_delegation_for_target, DelegateTool, execute, pipeline_rejection, execute, extract_context_refs, latest_delegation_for_target |
| REQ-HOOK-002 | Engine fires hooks at every lifecycle boundary, including a vetoable completion | loop |
| REQ-IDEN-001 | Tier resolution contract and per-tier loop overrides | seed_system_prompt_for_tier, entropic_run_session_as |
| REQ-IDEN-002 | Identity lifecycle separates immutable static from dynamic identities | entropic_list_identities, entropic_identity_count |
| REQ-INFER-007 | Named grammars are registered, validated, and resolved by a fixed precedence | warn_tier_grammars_step |
| REQ-INFER-018 | Same-prefix batch generation is gated, per-request-constrained and seq-safe | entropic_run_batch |
| REQ-INFER-019 | Model pool dedup and VRAM residency policy govern which tier is resident | require_ready_backend, entropic_set_residency_observer, entropic_release_model |
| REQ-INFER-023 | LoRA adapters hot-swap under a single-HOT-per-context constraint | entropic_adapter_load |
| REQ-INFER-024 | Backends declare capabilities and expose state and log-probability introspection | entropic_get_logprobs, entropic_compute_perplexity |
| REQ-LOOP-001 | Agent state machine with observable, dual-channel transitions | run_loop, run, run_turn, run_streaming, entropic_run_session, entropic_session_context_get |
| REQ-LOOP-002 | Bounded loop termination with synthetic completion on cap | run, loop |
| REQ-LOOP-003 | Fixed per-iteration pipeline with pre-generate cancellation | dispatch_pending_or_halt, execute_pending_delegation |
| REQ-LOOP-006 | Interrupt and pause semantics across nested loops | run_loop, dispatch_pending_or_halt, interrupt, reset_interrupt, entropic_interrupt |
| REQ-LOOP-008 | Family-aware streaming reasoning filter with UTF-8 alignment | run_streaming |
| REQ-MCP-001 | MCPServerBase holds the shared logic; concrete servers override only deltas | init_builtins, set_working_dir_all |
| REQ-MCP-002 | Tool results always cross boundaries as a ServerResponse JSON envelope | build_directive |
| REQ-MCP-007 | Tool calls route by server prefix uniformly across all three server kinds | init_builtins, append_external_tools, tools_without_readonly_hint, external_tools_without_readonly_hint |
| REQ-MCP-013 | Tool arguments are validated against the declared JSON schema before dispatch | DelegateTool, PipelineTool |
| REQ-MCP-024 | Engine-level entropic.* tools validate their arguments and emit typed directives | DelegateTool, execute, PipelineTool, pipeline_rejection, execute, execute, register_delegation_tools, build_directive |
| REQ-SAFE-001 | Untrusted bytes are sanitized at ingress, never at egress | entropic_context_get |
| REQ-STOR-005 | Delegation record lifecycle with parent-conversation guard | latest_delegation_for_target |
| REQ-TYPE-005 | Config types declare their own defaults; optional means absent, not default | parse_gpu_layers, parse_model_config, parse_tier_config |
| REQ-VALID-002 | Critique-and-revise loop with typed verdicts and content safety valve | entropic_set_critique_callbacks |

**Total: 42 requirement(s) affected, 150 function(s) changed**
