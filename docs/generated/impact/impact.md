## Change Impact Report

| REQ | Name | Functions Changed |
|-----|------|-------------------|
| REQ-INFER-005 | Every decode path honours cooperative cancellation within one token | generate_streaming |
| REQ-INFER-007 | Named grammars are registered, validated, and resolved by a fixed precedence | resolve_grammar_key |
| REQ-INFER-008 | Every declared grammar source reaches the sampler and exactly one wins | generate_streaming, finish_generation, record_generation |
| REQ-INFER-010 | One template-first / adapter-second rule parses every raw emission | diagnose_empty_content, explain_empty_content, warn_if_content_vanished |
| REQ-INFER-011 | Reasoning-block markers are declared once per model family | generate_streaming |

**Total: 5 requirement(s) affected, 9 function(s) changed**
