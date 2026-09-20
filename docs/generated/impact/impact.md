## Change Impact Report

| REQ | Name | Functions Changed |
|-----|------|-------------------|
| REQ-CFG-001 | Layered configuration resolution with most-specific-layer-wins precedence | parse_models_config |
| REQ-CFG-002 | Whole-block replace semantics for models and routing across layers | parse_models_config |
| REQ-CFG-003 | Bundled model registry resolution — keys, selectors, and search order | parse_model_config |
| REQ-CFG-005 | Per-tier knobs override global defaults without an all-or-nothing switch | parse_gpu_layers, parse_model_config, parse_tier_config |
| REQ-INFER-002 | Teardown releases every model, context and adapter handle | do_load_active, promote_to_active, load_and_activate, do_load_active |
| REQ-INFER-005 | Every decode path honours cooperative cancellation within one token | do_generate_streaming_text_only, generate_streaming |
| REQ-INFER-007 | Named grammars are registered, validated, and resolved by a fixed precedence | generate_streaming, resolve_grammar_key |
| REQ-INFER-008 | Every declared grammar source reaches the sampler and exactly one wins | active_tool_grammar, generate_streaming, finish_generation, record_generation, generation_records |
| REQ-INFER-009 | Tool staging drives a native render whose parse context is captured | resolve_and_stage |
| REQ-INFER-010 | One template-first / adapter-second rule parses every raw emission | warn_if_content_vanished |
| REQ-INFER-011 | Reasoning-block markers are declared once per model family | generate_streaming |
| REQ-INFER-019 | Model pool dedup and VRAM residency policy govern which tier is resident | host_memlock_limit_bytes, activate_default_tier, resolve_auto_gpu_layers, config_admits, refuse_residency, announce_eviction, mlock_refused, gpu_offload_refused |
| REQ-INFER-020 | Tier routing, handoff rules and secondary model roles | ensure_secondary_roles, last_routing_result, loaded_models, can_handoff, clear_all_prompt_caches |
| REQ-INFER-025 | Multimodal input is bounded, tier-gated and degrades gracefully | tier_declares_vision, has_vision_capable_tier, select_vision_tier |
| REQ-TYPE-005 | Config types declare their own defaults; optional means absent, not default | parse_gpu_layers, parse_model_config, parse_tier_config |

**Total: 15 requirement(s) affected, 41 function(s) changed**
