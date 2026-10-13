## Change Impact Report

| REQ | Name | Functions Changed |
|-----|------|-------------------|
| REQ-BRIDGE-001 | External bridge exposes the engine over a peer-authenticated unix socket | stop, accept_loop, run_async_ask, broadcast_notification |
| REQ-LOOP-001 | Agent state machine with observable, dual-channel transitions | run_streaming, run_streaming |
| REQ-LOOP-008 | Family-aware streaming reasoning filter with UTF-8 alignment | RunStreamScope, run_streaming, run_streaming, stream_token_callback, emit_to_current_run_stream, RunStreamScope, ~RunStreamScope |

**Total: 3 requirement(s) affected, 13 function(s) changed**
