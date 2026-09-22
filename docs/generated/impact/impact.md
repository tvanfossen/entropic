## Change Impact Report

| REQ | Name | Functions Changed |
|-----|------|-------------------|
| REQ-CFG-001 | Layered configuration resolution with most-specific-layer-wins precedence | parse_models_config, load_layered |
| REQ-CFG-002 | Whole-block replace semantics for models and routing across layers | parse_models_config, load_layered |
| REQ-CFG-003 | Bundled model registry resolution — keys, selectors, and search order | parse_model_config |
| REQ-CFG-005 | Per-tier knobs override global defaults without an all-or-nothing switch | parse_gpu_layers, parse_model_config, parse_tier_config, parse_speculative_config |
| REQ-CFG-007 | Usable configuration with no user config present | load_layered |
| REQ-DELEG-005 | Opt-in delegation sandbox isolation, wired end to end | parse_delegation_config, set_working_dir_all, enter_working_dir, leave_working_dir, tools_without_readonly_hint |
| REQ-LOOP-009 | Per-session-key run concurrency, on by default, with one lock on the generation path | set_working_dir_all, enter_working_dir, leave_working_dir, release_session, route_tool_call |
| REQ-MCP-001 | MCPServerBase holds the shared logic; concrete servers override only deltas | init_builtins, set_working_dir_all |
| REQ-MCP-007 | Tool calls route by server prefix uniformly across all three server kinds | init_builtins, append_external_tools, tools_without_readonly_hint, external_tools_without_readonly_hint, get_server, route_tool_call |
| REQ-MCP-021 | Filesystem tools are root-confined, read-before-write gated, and size-bounded | parse_outside_root_access, parse_outside_root_list, set_outside_root_approver, release_session |
| REQ-MCP-023 | Bash and git servers gate on operator approval, not command pattern matching; bash enforces a timeout on the whole process group | parse_bash_config, init_builtins, spawn_shell, pump, shell_exited, collect_until_exit, drain, reap, run_shell, timeout_error, execute, timeout |
| REQ-MCP-025 | External servers are discovered safely, directive-stripped, and health-monitored | make_transport |
| REQ-TYPE-005 | Config types declare their own defaults; optional means absent, not default | parse_gpu_layers, parse_model_config, parse_tier_config, parse_speculative_config |

**Total: 13 requirement(s) affected, 49 function(s) changed**
