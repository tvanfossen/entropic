## Change Impact Report

| REQ | Name | Functions Changed |
|-----|------|-------------------|
| REQ-ABI-001 | Pure C at every .so boundary — opaque handles and explicit ownership | entropic_state_save, entropic_state_load |
| REQ-ABI-002 | C++ exceptions never cross any .so boundary | entropic_state_save, entropic_state_load, entropic_adapter_load, entropic_get_logprobs, entropic_compute_perplexity, entropic_model_has_vision |
| REQ-API-004 | Three configure entry points, one shared body, one-shot per handle | validate_prompt_sources, configure_common |
| REQ-API-005 | Uniform precondition guard on every exported entry point | entropic_state_save, entropic_state_load, entropic_adapter_load, entropic_get_logprobs, entropic_compute_perplexity, entropic_model_has_vision |
| REQ-API-008 | Single cross-boundary allocator pair and explicit ownership transfer | entropic_get_logprobs |
| REQ-CFG-001 | Layered configuration resolution with most-specific-layer-wins precedence | parse_models_config |
| REQ-CFG-002 | Whole-block replace semantics for models and routing across layers | parse_models_config |
| REQ-INFER-005 | Every decode path honours cooperative cancellation within one token | generate_streaming |
| REQ-INFER-007 | Named grammars are registered, validated, and resolved by a fixed precedence | warn_tier_grammars_step, generate_streaming, resolve_grammar_key, refuse_unresolved_tier_grammar |
| REQ-INFER-008 | Every declared grammar source reaches the sampler and exactly one wins | generate_streaming, finish_generation, record_generation, generation_records |
| REQ-INFER-010 | One template-first / adapter-second rule parses every raw emission | apply_adapter_parse, warn_if_content_vanished, warn_turn_diagnostics |
| REQ-INFER-011 | Reasoning-block markers are declared once per model family | generate_streaming |
| REQ-INFER-019 | Model pool dedup and VRAM residency policy govern which tier is resident | require_ready_backend, activate_default_tier, ensure_model |
| REQ-INFER-023 | LoRA adapters hot-swap under a single-HOT-per-context constraint | entropic_adapter_load, preload_adapters_for_model |
| REQ-INFER-024 | Backends declare capabilities and expose state and log-probability introspection | entropic_get_logprobs, entropic_compute_perplexity |
| REQ-INFER-025 | Multimodal input is bounded, tier-gated and degrades gracefully | entropic_model_has_vision, tier_declares_vision |

**Total: 16 requirement(s) affected, 41 function(s) changed**
