## Change Impact Report

| REQ | Name | Functions Changed |
|-----|------|-------------------|
| REQ-ABI-001 | Pure C at every .so boundary — opaque handles and explicit ownership | entropic_context_usage, entropic_state_save, entropic_state_load, entropic_free_logprob_result |
| REQ-ABI-002 | C++ exceptions never cross any .so boundary | entropic_run_batch, entropic_state_save, entropic_state_load, entropic_adapter_load, entropic_list_identities, entropic_get_logprobs |
| REQ-API-004 | Three configure entry points, one shared body, one-shot per handle | validate_prompt_sources, configure_common |
| REQ-API-005 | Uniform precondition guard on every exported entry point | entropic_run_batch, entropic_context_usage, entropic_state_save, entropic_state_load, entropic_metrics_json, entropic_adapter_load, entropic_list_identities, entropic_identity_count, entropic_get_logprobs, entropic_set_residency_observer |
| REQ-API-008 | Single cross-boundary allocator pair and explicit ownership transfer | entropic_run_batch, entropic_metrics_json, entropic_list_identities, entropic_get_logprobs, entropic_free_logprob_result |
| REQ-API-009 | Run entry-point family, result contract, and cross-thread interruptibility | try_begin_turn, claim, entropic_run_batch, entropic_interrupt_session, entropic_run_session, entropic_run_session_as, entropic_run_session_streaming |
| REQ-API-010 | Observer and callback slots survive configure and fire uniformly | entropic_set_residency_observer |
| REQ-CFG-001 | Layered configuration resolution with most-specific-layer-wins precedence | parse_models_config |
| REQ-CFG-002 | Whole-block replace semantics for models and routing across layers | parse_models_config |
| REQ-CFG-003 | Bundled model registry resolution — keys, selectors, and search order | parse_model_config |
| REQ-CFG-005 | Per-tier knobs override global defaults without an all-or-nothing switch | parse_gpu_layers, parse_model_config |
| REQ-COMPACT-002 | Fill-gated tool-result pruning and persistent context anchors | run_loop, run |
| REQ-DELEG-002 | Delegation lifecycle from preconditions to result fold-back | build_resumed_child_context, build_child_context |
| REQ-HOOK-002 | Engine fires hooks at every lifecycle boundary, including a vetoable completion | loop |
| REQ-IDEN-001 | Tier resolution contract and per-tier loop overrides | entropic_run_session_as |
| REQ-IDEN-002 | Identity lifecycle separates immutable static from dynamic identities | entropic_list_identities, entropic_identity_count |
| REQ-INFER-002 | Teardown releases every model, context and adapter handle | do_load_active |
| REQ-INFER-005 | Every decode path honours cooperative cancellation within one token | generate_streaming |
| REQ-INFER-007 | Named grammars are registered, validated, and resolved by a fixed precedence | warn_tier_grammars_step, generate_streaming, resolve_grammar_key, refuse_unresolved_tier_grammar, refuse_unresolved_batch_grammars |
| REQ-INFER-008 | Every declared grammar source reaches the sampler and exactly one wins | apply_grammar_source, generate_streaming, finish_generation, record_generation |
| REQ-INFER-009 | Tool staging drives a native render whose parse context is captured | resolve_and_stage |
| REQ-INFER-010 | One template-first / adapter-second rule parses every raw emission | warn_if_content_vanished |
| REQ-INFER-011 | Reasoning-block markers are declared once per model family | generate_streaming |
| REQ-INFER-018 | Same-prefix batch generation is gated, per-request-constrained and seq-safe | entropic_run_batch, plan_batch_kv |
| REQ-INFER-019 | Model pool dedup and VRAM residency policy govern which tier is resident | require_ready_backend, entropic_set_residency_observer, activate_default_tier, log_fit_recommendation, resolve_auto_gpu_layers, config_admits, refuse_residency, ensure_model, release_models, release_backend, announce_eviction |
| REQ-INFER-020 | Tier routing, handoff rules and secondary model roles | ensure_secondary_roles, classify_task |
| REQ-INFER-023 | LoRA adapters hot-swap under a single-HOT-per-context constraint | entropic_adapter_load, preload_adapters_for_model |
| REQ-INFER-024 | Backends declare capabilities and expose state and log-probability introspection | entropic_get_logprobs |
| REQ-LOOP-001 | Agent state machine with observable, dual-channel transitions | run_loop, run, entropic_run_session |
| REQ-LOOP-002 | Bounded loop termination with synthetic completion on cap | run, loop |
| REQ-LOOP-003 | Fixed per-iteration pipeline with pre-generate cancellation | dispatch_pending_or_halt |
| REQ-LOOP-006 | Interrupt and pause semantics across nested loops | RunCancelScope, run_loop, dispatch_pending_or_halt, interrupt, reset_interrupt, entropic_interrupt_session, current_run_cancel, current_run_cancelled, RunCancelScope, ~RunCancelScope |
| REQ-LOOP-009 | Per-session-key run concurrency, opt-in, with one lock on the generation path | plan_batch_kv, plan_disturbs_sessions |
| REQ-MCP-025 | External servers are discovered safely, directive-stripped, and health-monitored | send_request |
| REQ-TYPE-005 | Config types declare their own defaults; optional means absent, not default | parse_gpu_layers, parse_model_config |

**Total: 35 requirement(s) affected, 101 function(s) changed**
