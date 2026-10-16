## Change Impact Report

| REQ | Name | Functions Changed |
|-----|------|-------------------|
| REQ-CFG-001 | Layered configuration resolution with most-specific-layer-wins precedence | parse_config_file, parse_config_string, load_config_from_string |
| REQ-CFG-002 | Whole-block replace semantics for models and routing across layers | yaml_has_key |
| REQ-CFG-003 | Bundled model registry resolution — keys, selectors, and search order | load |
| REQ-CFG-006 | Fail-loud validation — reject bad or inert configuration at load time | parse_config_file, parse_config_string, load_config_from_string |
| REQ-MCP-012 | Tool calls pass an ordered precondition pipeline with typed rejection kinds | check_tier_allowed |
| REQ-MCP-014 | A locked tier's allowed_tools list is enforced at dispatch time | check_tier_allowed |
| REQ-MCP-021 | Filesystem tools are root-confined, read-before-write gated, and size-bounded | parse_absolute_path_list |
| REQ-MCP-023 | Bash and git servers gate on operator approval, not command pattern matching; bash enforces a timeout on the whole process group | run_git |
| REQ-MCP-027 | Named workspaces bind a session to its own tool root and servers | validate_workspace_args |
| REQ-TYPE-005 | Config types declare their own defaults; optional means absent, not default | parse_config_file, validate_configured_prompts |

**Total: 10 requirement(s) affected, 15 function(s) changed**
