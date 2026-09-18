## Change Impact Report

| REQ | Name | Functions Changed |
|-----|------|-------------------|
| REQ-API-004 | Three configure entry points, one shared body, one-shot per handle | validate_prompt_sources, configure_common |
| REQ-CFG-006 | Fail-loud validation — reject bad or inert configuration at load time | validate_tier_grammars |
| REQ-INFER-005 | Every decode path honours cooperative cancellation within one token | generate_streaming |
| REQ-INFER-007 | Named grammars are registered, validated, and resolved by a fixed precedence | grammar_search_paths, validate_tier_grammars, validate_tier_grammars_step, resolve_grammar_key |
| REQ-INFER-008 | Every declared grammar source reaches the sampler and exactly one wins | active_tool_grammar, resolve_grammar_source, is_request_grammar, grammar_source_name, describe_grammar, apply_grammar_source, active_tool_grammar, generate_streaming, finish_generation, record_generation, generation_records |
| REQ-INFER-011 | Reasoning-block markers are declared once per model family | generate_streaming |
| REQ-TYPE-004 | Sentinel count members bound enum validity and force exhaustive wiring | resolve_grammar_source |

**Total: 7 requirement(s) affected, 21 function(s) changed**
