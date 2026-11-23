## Change Impact Report

| REQ | Name | Functions Changed |
|-----|------|-------------------|
| REQ-API-004 | Three configure entry points, one shared body, one-shot per handle | validate_prompt_sources |
| REQ-API-005 | Uniform precondition guard on every exported entry point | entropic_release_model |
| REQ-API-009 | Run entry-point family, result contract, and cross-thread interruptibility | try_begin_turn |
| REQ-BRIDGE-001 | External bridge exposes the engine over a peer-authenticated unix socket | configure_server, bind_loopback, tool_definitions, session_tool_names, mark_tasks_cancelling, any_cancelling_left, turn_timeout_result, bind_session_workspace, handle_shutdown, tool_handlers, initialize_result, sentinel_suffix_for_status |
| REQ-CFG-001 | Layered configuration resolution with most-specific-layer-wins precedence | apply_env, apply_mcp_external_env |
| REQ-CFG-005 | Per-tier knobs override global defaults without an all-or-nothing switch | parse_gpu_layers |
| REQ-DELEG-005 | Opt-in delegation sandbox isolation, wired end to end | parse_delegation_config |
| REQ-HOOK-002 | Engine fires hooks at every lifecycle boundary, including a vetoable completion | fire_hook_info, build_tool_manifest, build_tool_evidence, build_tool_results_json |
| REQ-IDEN-001 | Tier resolution contract and per-tier loop overrides | build_params_json, build_params_json |
| REQ-INFER-002 | Teardown releases every model, context and adapter handle | teardown_mtp_draft |
| REQ-INFER-007 | Named grammars are registered, validated, and resolved by a fixed precedence | normalize_grammar_key, warn_tier_grammars_step |
| REQ-INFER-008 | Every declared grammar source reaches the sampler and exactly one wins | apply_grammar_source |
| REQ-INFER-009 | Tool staging drives a native render whose parse context is captured | resolve_and_stage, to_common_chat, mcp_tools_to_common_chat |
| REQ-INFER-010 | One template-first / adapter-second rule parses every raw emission | apply_fenced_fallbacks |
| REQ-INFER-015 | MTP speculative decode fails loud and never silently falls back | effective_n_draft |
| REQ-INFER-019 | Model pool dedup and VRAM residency policy govern which tier is resident | log_fit_recommendation, resolve_auto_gpu_layers, config_admits, refuse_residency, release_backend, announce_eviction, resolve_vram_budget_bytes, ensure_vram_budget, footprint_inputs_for, estimate_footprint_bytes, require_ready_backend, entropic_release_model |
| REQ-INFER-020 | Tier routing, handoff rules and secondary model roles | extract_latest_user_message, ensure_secondary_roles |
| REQ-INFER-021 | Per-tier sampler configuration applies only where the caller did not decide | apply_if_default |
| REQ-INFER-023 | LoRA adapters hot-swap under a single-HOT-per-context constraint | preload_adapters_for_model |
| REQ-INFER-027 | Expert-tensor offload places routed experts on the host while attention and KV stay resident (EXPERIMENTAL) | build_load_mparams, model_expert_count, expert_offload_admits, load_gpu_model |
| REQ-LOOP-003 | Fixed per-iteration pipeline with pre-generate cancellation | process_generation_result |
| REQ-LOOP-004 | Empty-turn allowance and tool-call correction nudge | evaluate_no_tool_decision, record_explicit_completion_failure |
| REQ-LOOP-006 | Interrupt and pause semantics across nested loops | spawn_cancel_poller, this_thread_run, this_thread_run |
| REQ-LOOP-007 | Engine-authored transcript shaping keeps the system prompt bit-stable | prepare_prompts, inject_engine_state_reminder |
| REQ-LOOP-009 | Per-session-key run concurrency, on by default, with one lock on the generation path | execute_tool, try_begin_turn |
| REQ-LOOP-010 | Session conversations are readable, writable, and losslessly round-trippable | set_session_messages |
| REQ-MCP-002 | Tool results always cross boundaries as a ServerResponse JSON envelope | execute_tool |
| REQ-MCP-010 | Per-identity MCP key authorization is opt-in but default-deny once enabled | check_mcp_authorization |
| REQ-MCP-011 | Every tool declares its required access level, defaulting to WRITE | check_mcp_authorization |
| REQ-MCP-012 | Tool calls pass an ordered precondition pipeline with typed rejection kinds | check_mcp_authorization |
| REQ-MCP-015 | Duplicates, errors, and denials return corrective guidance, not bare failure | create_denied_message, create_error_message, create_duplicate_message |
| REQ-MCP-020 | A bounded, thread-safe ring buffer retains recent tool calls for introspection | execute_tool |
| REQ-MCP-027 | Named workspaces bind a session to its own tool root and servers | execute_tool |
| REQ-TYPE-005 | Config types declare their own defaults; optional means absent, not default | parse_gpu_layers |

**Total: 34 requirement(s) affected, 75 function(s) changed**
