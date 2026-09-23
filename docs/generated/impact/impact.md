## Change Impact Report

| REQ | Name | Functions Changed |
|-----|------|-------------------|
| REQ-COMPACT-001 | Threshold compaction preserving task-bearing messages, snapshot first | context_usage |
| REQ-COMPACT-002 | Fill-gated tool-result pruning and persistent context anchors | run_loop, run, reinject_context_anchors, dir_prune, dir_anchor |
| REQ-DELEG-001 | Delegation admission guards reject before a child loop runs | push_delegation_repeat_blocked, push_delegation_isolation_unsafe, reject_delegation_if_guarded, reject_pipeline_if_guarded, execute_pending_pipeline |
| REQ-DELEG-002 | Delegation lifecycle from preconditions to result fold-back | check_delegation_preconditions, execute_delegation, build_resumed_child_context, execute_resume_delegation, build_child_context, run_child, resolve_max_iterations, init_session_conversation, dir_delegate, execute_pending_delegation, fetch_resume_payload, resolve_resume_delegation, run_pending_delegation, ensure_sandbox_manager, resolve_session_root |
| REQ-DELEG-003 | Relay-single-delegate promotion with coverage-gap suppression | log_relay_status |
| REQ-DELEG-004 | Sequential pipeline with forward carry and per-stage reporting | execute_pipeline, run_pipeline_stage, reject_pipeline_if_guarded, execute_pending_pipeline |
| REQ-DELEG-005 | Opt-in delegation sandbox isolation, wired end to end | mint_delegation_id, restore_root_for, push_delegation_isolation_unsafe, resolve_session_root, sandbox_for_session, isolation_unsafe_tools |
| REQ-DELEG-006 | Explicit delegation context, required-context tiers, resume by target | format_context_block, resolve_latest_for_target, resolve_resume_delegation |
| REQ-HOOK-002 | Engine fires hooks at every lifecycle boundary, including a vetoable completion | loop, set_state |
| REQ-IDEN-001 | Tier resolution contract and per-tier loop overrides | resolve_max_iterations, resolve_max_tool_calls, run_turn_as, seed_system_prompt_for_tier |
| REQ-IDEN-003 | Handoff rules and auto-chain continuation | dir_tier_change |
| REQ-INFER-026 | A prompt that cannot fit the tier context is refused, never decoded | handle_terminal_finish_reasons |
| REQ-LOOP-001 | Agent state machine with observable, dual-channel transitions | run_loop, run, set_state, run_turn, run_streaming |
| REQ-LOOP-002 | Bounded loop termination with synthetic completion that carries the run's last real output | resolve_max_iterations, run, finish_with_carried_output, force_iteration_cap_completion, loop |
| REQ-LOOP-003 | Fixed per-iteration pipeline with pre-generate cancellation | dispatch_pending_or_halt, execute_pending_delegation |
| REQ-LOOP-004 | Empty-turn allowance and tool-call correction nudge | evaluate_no_tool_decision |
| REQ-LOOP-005 | Opt-in thinking-budget gating with nudge then a hard cut that carries the run's real output | finish_with_carried_output, nudge_budget_completion, hard_cut_budget |
| REQ-LOOP-006 | Interrupt and pause semantics across nested loops | run_loop, dispatch_pending_or_halt, handle_terminal_finish_reasons, interrupt, set_external_reset, cancel_pause |
| REQ-LOOP-008 | Family-aware streaming reasoning filter with UTF-8 alignment | run_streaming |
| REQ-LOOP-010 | Session conversations are readable, writable, and losslessly round-trippable | set_session_messages |

**Total: 20 requirement(s) affected, 72 function(s) changed**
