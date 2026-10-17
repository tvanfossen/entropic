## Change Impact Report

| REQ | Name | Functions Changed |
|-----|------|-------------------|
| REQ-ABI-001 | Pure C at every .so boundary — opaque handles and explicit ownership | entropic_seconds_since_last_activity, entropic_alloc, entropic_free |
| REQ-ABI-002 | C++ exceptions never cross any .so boundary | entropic_run, entropic_run_batch, entropic_run_streaming, entropic_run_messages |
| REQ-API-002 | Handle lifecycle — create, configure, destroy, NULL-safe teardown | entropic_run |
| REQ-API-005 | Uniform precondition guard on every exported entry point | entropic_seconds_since_last_activity, entropic_run, entropic_run_batch, entropic_run_streaming, entropic_run_messages, entropic_context_get |
| REQ-API-008 | Single cross-boundary allocator pair and explicit ownership transfer | entropic_alloc, entropic_free, entropic_run, entropic_run_batch, entropic_run_messages, entropic_context_get |
| REQ-API-009 | Run entry-point family, result contract, and cross-thread interruptibility | entropic_run, entropic_run_batch, entropic_run_streaming, entropic_run_messages, entropic_run_session |
| REQ-API-010 | Observer and callback slots survive configure and fire uniformly | entropic_run, entropic_run_streaming |
| REQ-IDEN-001 | Tier resolution contract and per-tier loop overrides | derive_explicit_completion |
| REQ-INFER-018 | Same-prefix batch generation is gated, per-request-constrained and seq-safe | entropic_run_batch |
| REQ-INFER-025 | Multimodal input is bounded, tier-gated and degrades gracefully | entropic_run_messages |
| REQ-LOOP-001 | Agent state machine with observable, dual-channel transitions | entropic_run_session |
| REQ-SAFE-001 | Untrusted bytes are sanitized at ingress, never at egress | entropic_context_get |

**Total: 12 requirement(s) affected, 32 function(s) changed**
