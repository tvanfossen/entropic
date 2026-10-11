## Change Impact Report

| REQ | Name | Functions Changed |
|-----|------|-------------------|
| REQ-API-009 | Run entry-point family, result contract, and cross-thread interruptibility | entropic_run_session_streaming, entropic_run_session_streaming_as |
| REQ-BRIDGE-001 | External bridge exposes the engine over a peer-authenticated unix socket | start_external_bridge |
| REQ-IDEN-001 | Tier resolution contract and per-tier loop overrides | entropic_run_session_streaming_as |
| REQ-LOOP-001 | Agent state machine with observable, dual-channel transitions | run_streaming, run_streaming |
| REQ-LOOP-008 | Family-aware streaming reasoning filter with UTF-8 alignment | run_streaming, run_streaming |

**Total: 5 requirement(s) affected, 8 function(s) changed**
