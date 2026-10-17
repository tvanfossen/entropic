## Change Impact Report

| REQ | Name | Functions Changed |
|-----|------|-------------------|
| REQ-ABI-002 | C++ exceptions never cross any .so boundary | entropic_run, entropic_run_streaming |
| REQ-API-002 | Handle lifecycle — create, configure, destroy, NULL-safe teardown | entropic_run |
| REQ-API-005 | Uniform precondition guard on every exported entry point | entropic_run, entropic_run_streaming |
| REQ-API-008 | Single cross-boundary allocator pair and explicit ownership transfer | entropic_run |
| REQ-API-009 | Run entry-point family, result contract, and cross-thread interruptibility | entropic_run, entropic_run_streaming, entropic_run_session |
| REQ-API-010 | Observer and callback slots survive configure and fire uniformly | entropic_run, entropic_run_streaming |
| REQ-API-012 | Final-answer selection from a serialized conversation | render_turn, final_answer_from_context, handle_ask_plain, handle_ask, extract_final_text, final_text_or_reason, failure_detail |
| REQ-BRIDGE-001 | External bridge exposes the engine over a peer-authenticated unix socket | render_turn, final_answer_from_context, handle_ask_plain, handle_ask, derive_async_final_state, async_worker |
| REQ-LOOP-001 | Agent state machine with observable, dual-channel transitions | run_turn, entropic_run_session |
| REQ-LOOP-004 | Empty-turn allowance and tool-call correction nudge | record_explicit_completion_failure |

**Total: 10 requirement(s) affected, 27 function(s) changed**
