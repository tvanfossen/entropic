## Change Impact Report

| REQ | Name | Functions Changed |
|-----|------|-------------------|
| REQ-ABI-001 | Pure C at every .so boundary — opaque handles and explicit ownership | entropic_destroy, entropic_context_clear, entropic_state_save, entropic_state_load |
| REQ-ABI-002 | C++ exceptions never cross any .so boundary | entropic_run_batch, entropic_state_save, entropic_state_load, entropic_adapter_load, entropic_get_logprobs, entropic_compute_perplexity, entropic_model_has_vision, entropic_workspace_create, entropic_session_bind_workspace, entropic_register_mcp_server |
| REQ-API-002 | Handle lifecycle — create, configure, destroy, NULL-safe teardown | entropic_destroy |
| REQ-API-003 | Multiple handles are fully independent within one process | entropic_destroy |
| REQ-API-004 | Three configure entry points, one shared body, one-shot per handle | validate_prompt_sources, configure_common |
| REQ-API-005 | Uniform precondition guard on every exported entry point | entropic_run_batch, entropic_context_clear, entropic_state_save, entropic_state_load, entropic_adapter_load, entropic_get_logprobs, entropic_compute_perplexity, entropic_model_has_vision, entropic_release_model, entropic_set_path_approval_callback, entropic_workspace_create, entropic_session_bind_workspace, entropic_register_mcp_server |
| REQ-API-008 | Single cross-boundary allocator pair and explicit ownership transfer | entropic_run_batch, entropic_get_logprobs |
| REQ-API-009 | Run entry-point family, result contract, and cross-thread interruptibility | try_begin_turn, claim, entropic_run_batch, entropic_interrupt_session, entropic_run_session, entropic_run_session_as, entropic_run_session_streaming |
| REQ-API-010 | Observer and callback slots survive configure and fire uniformly | entropic_set_path_approval_callback |
| REQ-API-013 | Identity frontmatter threaded into config before the orchestrator snapshot | wire_tool_executor |
| REQ-CFG-001 | Layered configuration resolution with most-specific-layer-wins precedence | parse_models_config, parse_config_string |
| REQ-CFG-002 | Whole-block replace semantics for models and routing across layers | parse_models_config |
| REQ-CFG-003 | Bundled model registry resolution — keys, selectors, and search order | parse_model_config |
| REQ-CFG-005 | Per-tier knobs override global defaults without an all-or-nothing switch | parse_gpu_layers, parse_model_config, parse_tier_config |
| REQ-CFG-006 | Fail-loud validation — reject bad or inert configuration at load time | parse_config_string, validate, warn_unresolved_tier_grammars, expert_offload_conflict_reason |
| REQ-COMPACT-001 | Threshold compaction preserving task-bearing messages, snapshot first | count_message |
| REQ-COMPACT-002 | Fill-gated tool-result pruning and persistent context anchors | begin_loop_preamble, run_loop, run, reinject_context_anchors, dir_anchor |
| REQ-DELEG-001 | Delegation admission guards reject before a child loop runs | push_delegation_isolation_unsafe, reject_delegation_if_guarded, reject_pipeline_if_guarded, execute_pending_pipeline |
| REQ-DELEG-002 | Delegation lifecycle from preconditions to result fold-back | check_delegation_preconditions, execute_delegation, build_resumed_child_context, execute_resume_delegation, build_child_context, run_child, resolve_max_iterations, dir_delegate, execute_pending_delegation, resolve_resume_delegation, run_pending_delegation, ensure_sandbox_manager, resolve_session_root |
| REQ-DELEG-004 | Sequential pipeline with forward carry and per-stage reporting | execute_pipeline, run_pipeline_stage, dir_pipeline, reject_pipeline_if_guarded, execute_pending_pipeline |
| REQ-DELEG-005 | Opt-in delegation sandbox isolation, wired end to end | ToolRootLock, parse_delegation_config, mint_delegation_id, restore_root_for, push_delegation_isolation_unsafe, resolve_session_root, sandbox_for_session, isolation_unsafe_tools, next_delegation_id, facade_session_root, swap_session_tool_dir, set_working_dir_all, enter_working_dir, leave_working_dir, tools_without_readonly_hint, lock, unlock, lock_shared |
| REQ-DELEG-006 | Explicit delegation context, required-context tiers, resume by target | format_context_block, resolve_latest_for_target, resolve_resume_delegation, si_latest_delegation_for_target, DelegateTool, execute, pipeline_rejection, execute, extract_context_refs, latest_delegation_for_target |
| REQ-HOOK-002 | Engine fires hooks at every lifecycle boundary, including a vetoable completion | handle_hook, loop |
| REQ-IDEN-001 | Tier resolution contract and per-tier loop overrides | resolve_max_iterations, begin_loop_preamble, run_loop, run, seed_system_prompt_for_tier, entropic_run_session_as |
| REQ-INFER-002 | Teardown releases every model, context and adapter handle | do_load_active, promote_to_active, load_and_activate, do_load_active |
| REQ-INFER-005 | Every decode path honours cooperative cancellation within one token | generate_streaming |
| REQ-INFER-006 | Sampler chain construction has a fixed order and default-preserving gating | create |
| REQ-INFER-007 | Named grammars are registered, validated, and resolved by a fixed precedence | grammar_search_paths, warn_unresolved_tier_grammars, warn_tier_grammars_step, tier_grammar_unresolved, generate_streaming, resolve_grammar_key, refuse_unresolved_tier_grammar, refuse_unresolved_batch_grammars |
| REQ-INFER-008 | Every declared grammar source reaches the sampler and exactly one wins | active_tool_grammar, resolve_grammar_source, is_request_grammar, grammar_source_name, describe_grammar, apply_grammar_source, active_tool_grammar, create, generate_streaming, finish_generation, record_generation, generation_records |
| REQ-INFER-009 | Tool staging drives a native render whose parse context is captured | resolve_and_stage |
| REQ-INFER-010 | One template-first / adapter-second rule parses every raw emission | diagnose_empty_content, explain_empty_content, warn_if_content_vanished |
| REQ-INFER-011 | Reasoning-block markers are declared once per model family | generate_streaming |
| REQ-INFER-018 | Same-prefix batch generation is gated, per-request-constrained and seq-safe | entropic_run_batch, plan_batch_kv |
| REQ-INFER-019 | Model pool dedup and VRAM residency policy govern which tier is resident | require_ready_backend, entropic_release_model, host_memlock_limit_bytes, activate_default_tier, resolve_auto_gpu_layers, config_admits, refuse_residency, ensure_model, release_models, release_backend, announce_eviction, mlock_refused, gpu_offload_refused, forget_session |
| REQ-INFER-020 | Tier routing, handoff rules and secondary model roles | ensure_secondary_roles, classify_task |
| REQ-INFER-023 | LoRA adapters hot-swap under a single-HOT-per-context constraint | entropic_adapter_load, load, release_handles_for_model, preload_adapters_for_model |
| REQ-INFER-024 | Backends declare capabilities and expose state and log-probability introspection | entropic_get_logprobs, entropic_compute_perplexity |
| REQ-INFER-025 | Multimodal input is bounded, tier-gated and degrades gracefully | entropic_model_has_vision, tier_declares_vision |
| REQ-INFER-026 | A prompt that cannot fit the tier context is refused, never decoded | handle_terminal_finish_reasons, generate_batch, context_fit_overflows, context_overflow_message, refuse_over_context, prefill_error |
| REQ-INFER-027 | Expert-tensor offload places routed experts on the host while attention and KV stay resident (EXPERIMENTAL) | validate, expert_block_pattern, expert_offload_patterns, expert_offload_conflict_reason, expert_offload_load_refusal, data, do_load_active, expert_offload_admits, load_gpu_model |
| REQ-LOOP-001 | Agent state machine with observable, dual-channel transitions | begin_loop_preamble, run_loop, run, run_turn, entropic_run_session, entropic_session_context_clear, entropic_session_drop |
| REQ-LOOP-002 | Bounded loop termination with synthetic completion that carries the run's last real output | resolve_max_iterations, run, finish_with_carried_output, force_iteration_cap_completion, loop |
| REQ-LOOP-003 | Fixed per-iteration pipeline with pre-generate cancellation | dispatch_pending_or_halt, execute_pending_delegation, generate_batch |
| REQ-LOOP-005 | Opt-in thinking-budget gating with nudge then a hard cut that carries the run's real output | finish_with_carried_output, hard_cut_budget |
| REQ-LOOP-006 | Interrupt and pause semantics across nested loops | RunCancelScope, run_loop, dispatch_pending_or_halt, handle_terminal_finish_reasons, interrupt, reset_interrupt, spawn_cancel_poller, dispatch_batch_generate, handle_pause, entropic_interrupt_session, current_run_cancel, current_run_cancelled, RunCancelScope, cancel_current_run, ~RunCancelScope |
| REQ-LOOP-008 | Family-aware streaming reasoning filter with UTF-8 alignment | stream_token_callback |
| REQ-LOOP-009 | Per-session-key run concurrency, on by default, with one lock on the generation path | with, peek, release, session_count, ToolRootLock, ToolRootShared, RunSessionScope, entropic_context_clear, entropic_session_context_clear, entropic_session_drop, release_session_tool_state, plan_batch_kv, plan_disturbs_sessions, set_working_dir_all, enter_working_dir, leave_working_dir, release_session, route_tool_call, execute, release_session, session_count, record_read, was_read, release_session, session_count, release_session, session_count, execute_tool, lock, unlock, lock_shared, unlock_shared, owned_by_this_thread, ToolRootShared, ~ToolRootShared, current_run_session, RunSessionScope, ~RunSessionScope |
| REQ-LOOP-010 | Session conversations are readable, writable, and losslessly round-trippable | forget_session_kv, set_session_messages, entropic_session_context_set, serialize_message, serialize_messages, forget_session_kv, forget_session_kv, forget_session |
| REQ-MCP-001 | MCPServerBase holds the shared logic; concrete servers override only deltas | init_builtins, set_working_dir_all, FilesystemServer |
| REQ-MCP-002 | Tool results always cross boundaries as a ServerResponse JSON envelope | execute, execute_tool, build_directive |
| REQ-MCP-007 | Tool calls route by server prefix uniformly across all three server kinds | query_tools, init_builtins, tools_without_readonly_hint, external_tools_without_readonly_hint, route_tool_call |
| REQ-MCP-012 | Tool calls pass an ordered precondition pipeline with typed rejection kinds | process_single_call |
| REQ-MCP-013 | Tool arguments are validated against the declared JSON schema before dispatch | DelegateTool, PipelineTool |
| REQ-MCP-014 | A locked tier's allowed_tools list is enforced at dispatch time | wire_tool_executor |
| REQ-MCP-017 | PRE/POST_TOOL_CALL hooks fire on every exit path, PRE may rewrite the args and POST the result | process_single_call, apply_pre_tool_modification, fire_pre_tool_hook |
| REQ-MCP-020 | A bounded, thread-safe ring buffer retains recent tool calls for introspection | execute_tool |
| REQ-MCP-021 | Filesystem tools are root-confined, read-before-write gated, and size-bounded | parse_outside_root_access, parse_outside_root_list, confined_mcp_config, outside_root_thunk, wire_outside_root_approver, entropic_set_path_approval_callback, set_outside_root_approver, release_session, record_read, was_read, release_session, path_within, first_containing, outside_root_refusal, refuse_denied, refuse_escape, check_read_before_write, collect_glob_matches, execute, execute, execute, execute, FilesystemServer, set_working_dir, tracker, release_session, resolve_path, authorize_outside_root, ask_outside_root_approver, set_outside_root_approver, log_outside_root_policy |
| REQ-MCP-022 | Glob, grep, and read honour .gitignore plus .explorerignore semantics | collect_glob_matches, grep_search, execute, FilesystemServer, set_working_dir, is_skipped_dir_name, compile_pattern, push_rule, add_pattern, load_file, load, load_nested_gitignores, visit_discovery_entry, prunes_discovery, is_ignored |
| REQ-MCP-023 | Bash and git servers gate on operator approval, not command pattern matching; bash enforces a timeout on the whole process group | parse_bash_config, init_builtins, spawn_shell, pump, shell_exited, collect_until_exit, drain, reap, run_shell, timeout_error, execute, timeout |
| REQ-MCP-024 | Engine-level entropic.* tools validate their arguments and emit typed directives | execute, DelegateTool, execute, PipelineTool, pipeline_rejection, execute, execute, register_delegation_tools, release_session, build_directive |
| REQ-MCP-025 | External servers are discovered safely, directive-stripped, and health-monitored | entropic_register_mcp_server, execute, response_problem, mismatched_response_envelope, build_request, build_request_id, response_matches, send_initialize, query_tools, make_transport, send_request |
| REQ-MCP-026 | A JSON-RPC response is paired with the request that asked for it | execute, response_problem, mismatched_response_envelope, build_request_id, response_matches, send_initialize, query_tools, send_request, drain_orphaned_responses |
| REQ-MCP-027 | Named workspaces bind a session to its own tool root and servers | set_working_dir, workspace_for, workspace_servers, confined_mcp_config, build_workspace, validate_workspace_args, entropic_workspace_create, entropic_session_bind_workspace, resolve_registration_target, set_server_resolver, servers_for, execute_tool |
| REQ-SAFE-001 | Untrusted bytes are sanitized at ingress, never at egress | serialize_message, serialize_messages |
| REQ-STOR-005 | Delegation record lifecycle with parent-conversation guard | latest_delegation_for_target |
| REQ-TYPE-004 | Sentinel count members bound enum validity and force exhaustive wiring | resolve_grammar_source |
| REQ-TYPE-005 | Config types declare their own defaults; optional means absent, not default | parse_gpu_layers, parse_model_config, parse_tier_config, classify_app_context, app_context_state_message, load_app_context, load_shared_prompt_sources, validate_configured_prompts |
| REQ-VALID-001 | Layered validation gating with cheap-path skips | validate, handle_hook |
| REQ-VALID-002 | Critique-and-revise loop with typed verdicts and content safety valve | validate, build_critique_prompt, run_validation_loop |
| REQ-VALID-003 | Consumer-driven pause, resume and accept of a failing validation | resume_retry, run_validation_loop |

**Total: 70 requirement(s) affected, 406 function(s) changed**
