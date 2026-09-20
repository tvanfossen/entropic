## Change Impact Report

| REQ | Name | Functions Changed |
|-----|------|-------------------|
| REQ-API-009 | Run entry-point family, result contract, and cross-thread interruptibility | try_begin_turn |
| REQ-COMPACT-001 | Threshold compaction preserving task-bearing messages, snapshot first | count_message |
| REQ-COMPACT-002 | Fill-gated tool-result pruning and persistent context anchors | run_loop, run, reinject_context_anchors, dir_anchor |
| REQ-DELEG-002 | Delegation lifecycle from preconditions to result fold-back | resolve_resume_delegation, run_pending_delegation, ensure_sandbox_manager |
| REQ-HOOK-002 | Engine fires hooks at every lifecycle boundary, including a vetoable completion | handle_hook, set_state |
| REQ-IDEN-001 | Tier resolution contract and per-tier loop overrides | seed_system_prompt_for_tier |
| REQ-LOOP-001 | Agent state machine with observable, dual-channel transitions | run_loop, run, is_terminal_state, set_state, run_turn, run_streaming, run_streaming |
| REQ-LOOP-002 | Bounded loop termination with synthetic completion on cap | run |
| REQ-LOOP-005 | Opt-in thinking-budget gating with nudge then hard cut | budget_units_consumed |
| REQ-LOOP-006 | Interrupt and pause semantics across nested loops | RunCancelScope, run_loop, interrupt, spawn_cancel_poller, dispatch_batch_generate, handle_pause, current_run_cancel, current_run_cancelled, RunCancelScope, cancel_current_run |
| REQ-LOOP-008 | Family-aware streaming reasoning filter with UTF-8 alignment | run_streaming, run_streaming, stream_token_callback |
| REQ-MCP-002 | Tool results always cross boundaries as a ServerResponse JSON envelope | execute |
| REQ-MCP-007 | Tool calls route by server prefix uniformly across all three server kinds | query_tools |
| REQ-MCP-025 | External servers are discovered safely, directive-stripped, and health-monitored | execute, response_problem, mismatched_response_envelope, build_request, build_request_id, response_matches, send_initialize, query_tools, send_request, interrupt, clear_interrupt |
| REQ-MCP-026 | A JSON-RPC response is paired with the request that asked for it | execute, response_problem, mismatched_response_envelope, build_request_id, response_matches, send_initialize, query_tools, send_request, drain_orphaned_responses |
| REQ-VALID-001 | Layered validation gating with cheap-path skips | validate, handle_hook |
| REQ-VALID-002 | Critique-and-revise loop with typed verdicts and content safety valve | validate, build_critique_prompt, run_validation_loop |
| REQ-VALID-003 | Consumer-driven pause, resume and accept of a failing validation | resume_retry, run_validation_loop |

**Total: 18 requirement(s) affected, 63 function(s) changed**
