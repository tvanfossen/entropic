## Change Impact Report

| REQ | Name | Functions Changed |
|-----|------|-------------------|
| REQ-CFG-001 | Layered configuration resolution with most-specific-layer-wins precedence | parse_models_config, parse_config_string |
| REQ-CFG-002 | Whole-block replace semantics for models and routing across layers | parse_models_config |
| REQ-CFG-003 | Bundled model registry resolution — keys, selectors, and search order | resolve_model_path, parse_model_config |
| REQ-CFG-005 | Per-tier knobs override global defaults without an all-or-nothing switch | parse_gpu_layers, parse_model_config, parse_tier_config |
| REQ-CFG-006 | Fail-loud validation — reject bad or inert configuration at load time | resolve_model_path, parse_config_string, validate, validate_model_tiers, warn_unresolved_tier_grammars, expert_offload_conflict_reason |
| REQ-DELEG-005 | Opt-in delegation sandbox isolation, wired end to end | parse_delegation_config |
| REQ-INFER-002 | Teardown releases every model, context and adapter handle | do_load_active |
| REQ-INFER-005 | Every decode path honours cooperative cancellation within one token | generate_after_prefill, do_generate_streaming_text_only |
| REQ-INFER-007 | Named grammars are registered, validated, and resolved by a fixed precedence | grammar_search_paths, warn_unresolved_tier_grammars |
| REQ-INFER-008 | Every declared grammar source reaches the sampler and exactly one wins | active_tool_grammar |
| REQ-INFER-009 | Tool staging drives a native render whose parse context is captured | parse_response |
| REQ-INFER-026 | A prompt that cannot fit the tier context is refused, never decoded | refuse_over_context |
| REQ-INFER-027 | Expert-tensor offload places routed experts on the host while attention and KV stay resident (EXPERIMENTAL) | validate, expert_block_pattern, expert_offload_patterns, expert_offload_conflict_reason, expert_offload_load_refusal, data, do_load_active, expert_offload_admits, load_gpu_model |
| REQ-MCP-021 | Filesystem tools are root-confined, read-before-write gated, and size-bounded | parse_outside_root_access, parse_outside_root_list |
| REQ-MCP-023 | Bash and git servers gate on operator approval, not command pattern matching; bash enforces a timeout on the whole process group | parse_bash_config |
| REQ-TYPE-005 | Config types declare their own defaults; optional means absent, not default | parse_gpu_layers, parse_model_config, parse_tier_config |

**Total: 16 requirement(s) affected, 38 function(s) changed**
