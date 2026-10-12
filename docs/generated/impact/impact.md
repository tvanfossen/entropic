## Change Impact Report

| REQ | Name | Functions Changed |
|-----|------|-------------------|
| REQ-API-012 | Final-answer selection from a serialized conversation | handle_ask_plain, handle_ask |
| REQ-BRIDGE-001 | External bridge exposes the engine over a peer-authenticated unix socket | handle_ask_plain, handle_ask, cancel_inflight_async_tasks, handle_clear, ask_in_turn, dispatch_ask, dispatch_tool, begin_turn_wait, end_turn_wait, socket_path_safe, bind_and_listen, create_listen_socket, peer_uid_matches, start, stop, run_async_ask, async_worker, unsubscribe, broadcast_notification |

**Total: 2 requirement(s) affected, 21 function(s) changed**
