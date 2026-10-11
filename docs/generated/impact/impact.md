## Change Impact Report

| REQ | Name | Functions Changed |
|-----|------|-------------------|
| REQ-ABI-001 | Pure C at every .so boundary — opaque handles and explicit ownership | entropic_destroy |
| REQ-API-002 | Handle lifecycle — create, configure, destroy, NULL-safe teardown | entropic_destroy |
| REQ-API-003 | Multiple handles are fully independent within one process | entropic_destroy |
| REQ-API-004 | Three configure entry points, one shared body, one-shot per handle | configure_common |
| REQ-API-009 | Run entry-point family, result contract, and cross-thread interruptibility | entropic_run_session_as, entropic_run_session_streaming |
| REQ-API-012 | Final-answer selection from a serialized conversation | handle_ask_plain, handle_ask |
| REQ-BRIDGE-001 | External bridge exposes the engine over a peer-authenticated unix socket | start_http_listener, handle_ask_plain, handle_ask, cancel_inflight_async_tasks, handle_clear, ask_in_turn, dispatch_ask, dispatch_tool, begin_turn_wait, end_turn_wait, bind_and_listen, create_listen_socket, start, stop, run_async_ask, async_worker, unsubscribe, broadcast_notification, blocked, record_failure, ~SseJob, ~HttpListener, publish_token, start, await_running, stop |
| REQ-CFG-001 | Layered configuration resolution with most-specific-layer-wins precedence | load_layered |
| REQ-CFG-002 | Whole-block replace semantics for models and routing across layers | load_layered |
| REQ-CFG-007 | Usable configuration with no user config present | load_layered |
| REQ-IDEN-001 | Tier resolution contract and per-tier loop overrides | entropic_run_session_as |
| REQ-MCP-023 | Bash and git servers gate on operator approval, not command pattern matching; bash enforces a timeout on the whole process group | parse_bash_config |

**Total: 12 requirement(s) affected, 39 function(s) changed**
