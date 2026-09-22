## Change Impact Report

| REQ | Name | Functions Changed |
|-----|------|-------------------|
| REQ-API-009 | Run entry-point family, result contract, and cross-thread interruptibility | try_begin_turn |
| REQ-COMPACT-002 | Fill-gated tool-result pruning and persistent context anchors | run_loop, run, reinject_context_anchors |
| REQ-DELEG-001 | Delegation admission guards reject before a child loop runs | push_delegation_rejected, push_delegation_cycle_rejected, push_delegation_repeat_blocked, push_delegation_isolation_unsafe, reject_delegation_if_guarded |
| REQ-DELEG-002 | Delegation lifecycle from preconditions to result fold-back | push_delegation_result, push_resume_failure, fetch_resume_payload, resolve_resume_delegation, run_pending_delegation, ensure_sandbox_manager, resolve_session_root |
| REQ-DELEG-003 | Relay-single-delegate promotion with coverage-gap suppression | dir_complete, finalize_delegation_result, log_relay_status |
| REQ-DELEG-005 | Opt-in delegation sandbox isolation, wired end to end | push_delegation_isolation_unsafe, resolve_session_root, isolation_unsafe_tools |
| REQ-DELEG-006 | Explicit delegation context, required-context tiers, resume by target | resolve_latest_for_target, resolve_resume_delegation |
| REQ-HOOK-002 | Engine fires hooks at every lifecycle boundary, including a vetoable completion | loop, set_state, dir_complete |
| REQ-IDEN-001 | Tier resolution contract and per-tier loop overrides | seed_system_prompt_for_tier |
| REQ-IDEN-003 | Handoff rules and auto-chain continuation | should_auto_chain, try_auto_chain |
| REQ-LOOP-001 | Agent state machine with observable, dual-channel transitions | run_loop, run, is_terminal_state, set_state, run_turn, run_streaming |
| REQ-LOOP-002 | Bounded loop termination with synthetic completion that carries the run's last real output | run, force_iteration_cap_completion, loop, should_stop |
| REQ-LOOP-003 | Fixed per-iteration pipeline with pre-generate cancellation | dispatch_pending_or_halt |
| REQ-LOOP-005 | Opt-in thinking-budget gating with nudge then hard cut | budget_units_consumed, hard_cut_budget |
| REQ-LOOP-006 | Interrupt and pause semantics across nested loops | run_loop, dispatch_pending_or_halt, interrupt, set_external_reset, reset_interrupt |
| REQ-LOOP-008 | Family-aware streaming reasoning filter with UTF-8 alignment | run_streaming |

**Total: 16 requirement(s) affected, 49 function(s) changed**
