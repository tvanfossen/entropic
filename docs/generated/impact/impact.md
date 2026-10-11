## Change Impact Report

| REQ | Name | Functions Changed |
|-----|------|-------------------|
| REQ-ABI-002 | C++ exceptions never cross any .so boundary | entropic_workspace_create, entropic_session_bind_workspace, entropic_workspace_set_app_context |
| REQ-API-005 | Uniform precondition guard on every exported entry point | entropic_set_path_approval_callback, entropic_workspace_create, entropic_session_bind_workspace, entropic_workspace_set_app_context |
| REQ-API-010 | Observer and callback slots survive configure and fire uniformly | entropic_set_path_approval_callback |
| REQ-BRIDGE-001 | External bridge exposes the engine over a peer-authenticated unix socket | bridge_project_dir |
| REQ-CFG-001 | Layered configuration resolution with most-specific-layer-wins precedence | validate_workspace_app_context, load_layered |
| REQ-CFG-002 | Whole-block replace semantics for models and routing across layers | load_layered |
| REQ-CFG-007 | Usable configuration with no user config present | load_layered |
| REQ-IDEN-001 | Tier resolution contract and per-tier loop overrides | seed_system_prompt_for_tier, tri_resolve_tier |
| REQ-LOOP-001 | Agent state machine with observable, dual-channel transitions | run_turn, run_streaming |
| REQ-LOOP-008 | Family-aware streaming reasoning filter with UTF-8 alignment | run_streaming |
| REQ-MCP-021 | Filesystem tools are root-confined, read-before-write gated, and size-bounded | wire_outside_root_approver, entropic_set_path_approval_callback |
| REQ-MCP-025 | External servers are discovered safely, directive-stripped, and health-monitored | expected_bridge_socket |
| REQ-MCP-027 | Named workspaces bind a session to its own tool root and servers | set_workspace_prompts, clear_workspace_prompts, bind_session_workspace, session_system_prompt, apply_workspace_app_context, build_workspace, entropic_workspace_create, entropic_session_bind_workspace, entropic_workspace_set_app_context |

**Total: 13 requirement(s) affected, 30 function(s) changed**
