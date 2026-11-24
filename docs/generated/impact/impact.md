## Change Impact Report

| REQ | Name | Functions Changed |
|-----|------|-------------------|
| REQ-INFER-007 | Named grammars are registered, validated, and resolved by a fixed precedence | normalize_grammar_key |
| REQ-INFER-009 | Tool staging drives a native render whose parse context is captured | resolve_and_stage |
| REQ-INFER-019 | Model pool dedup and VRAM residency policy govern which tier is resident | log_fit_recommendation, resolve_auto_gpu_layers, config_admits, refuse_residency, release_backend, announce_eviction, resolve_vram_budget_bytes, ensure_vram_budget, estimate_footprint_bytes, derive_tier_auto_placement |
| REQ-INFER-020 | Tier routing, handoff rules and secondary model roles | extract_latest_user_message, ensure_secondary_roles |
| REQ-INFER-021 | Per-tier sampler configuration applies only where the caller did not decide | apply_if_default |
| REQ-INFER-023 | LoRA adapters hot-swap under a single-HOT-per-context constraint | preload_adapters_for_model |
| REQ-LOOP-010 | Session conversations are readable, writable, and losslessly round-trippable | forget_session_kv |

**Total: 7 requirement(s) affected, 17 function(s) changed**
