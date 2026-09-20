## Change Impact Report

| REQ | Name | Functions Changed |
|-----|------|-------------------|
| REQ-API-004 | Three configure entry points, one shared body, one-shot per handle | validate_prompt_sources, configure_common |
| REQ-CFG-006 | Fail-loud validation — reject bad or inert configuration at load time | warn_unresolved_tier_grammars |
| REQ-INFER-005 | Every decode path honours cooperative cancellation within one token | generate_streaming |
| REQ-INFER-007 | Named grammars are registered, validated, and resolved by a fixed precedence | grammar_search_paths, warn_unresolved_tier_grammars, warn_tier_grammars_step, tier_grammar_unresolved, generate_streaming, resolve_grammar_key, refuse_unresolved_tier_grammar, refuse_unresolved_batch_grammars |
| REQ-INFER-008 | Every declared grammar source reaches the sampler and exactly one wins | resolve_grammar_source, is_request_grammar, grammar_source_name, grammar_sources_collide, describe_grammar, generate_streaming, finish_generation, record_generation |
| REQ-INFER-009 | Tool staging drives a native render whose parse context is captured | resolve_and_stage |
| REQ-INFER-010 | One template-first / adapter-second rule parses every raw emission | warn_if_content_vanished |
| REQ-INFER-011 | Reasoning-block markers are declared once per model family | generate_streaming |
| REQ-INFER-025 | Multimodal input is bounded, tier-gated and degrades gracefully | select_vision_tier |
| REQ-TYPE-004 | Sentinel count members bound enum validity and force exhaustive wiring | resolve_grammar_source |

**Total: 10 requirement(s) affected, 25 function(s) changed**
