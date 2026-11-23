## Change Impact Report

| REQ | Name | Functions Changed |
|-----|------|-------------------|
| REQ-COMPACT-002 | Fill-gated tool-result pruning and persistent context anchors | dir_anchor |
| REQ-DELEG-002 | Delegation lifecycle from preconditions to result fold-back | ensure_sandbox_manager |
| REQ-DELEG-003 | Relay-single-delegate promotion with coverage-gap suppression | dir_complete |
| REQ-HOOK-002 | Engine fires hooks at every lifecycle boundary, including a vetoable completion | fire_hook_info, dir_complete, build_tool_manifest, build_tool_evidence, build_tool_results_json |
| REQ-IDEN-001 | Tier resolution contract and per-tier loop overrides | build_params_json, build_params_json |
| REQ-LOOP-001 | Agent state machine with observable, dual-channel transitions | run_streaming |
| REQ-LOOP-003 | Fixed per-iteration pipeline with pre-generate cancellation | process_generation_result |
| REQ-LOOP-004 | Empty-turn allowance and tool-call correction nudge | evaluate_no_tool_decision, record_explicit_completion_failure, restrict_tools |
| REQ-LOOP-006 | Interrupt and pause semantics across nested loops | spawn_cancel_poller, handle_pause |
| REQ-LOOP-007 | Engine-authored transcript shaping keeps the system prompt bit-stable | prepare_prompts, inject_engine_state_reminder |
| REQ-LOOP-008 | Family-aware streaming reasoning filter with UTF-8 alignment | run_streaming |
| REQ-LOOP-010 | Session conversations are readable, writable, and losslessly round-trippable | set_session_messages |

**Total: 12 requirement(s) affected, 21 function(s) changed**
