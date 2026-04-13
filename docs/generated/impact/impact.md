## Change Impact Report

| REQ | Name | Functions Changed |
|-----|------|-------------------|
| REQ-API-002 | Synchronous agentic run | entropic_run |
| REQ-CFG-001 | Layered config merge | parse_config_file |
| REQ-CFG-003 | Bundled model path resolution | BundledModels |
| REQ-INFER-003 | Streaming generation with cancel | entropic_inference_generate, entropic_inference_generate_streaming |
| REQ-INFER-004 | Raw text completion | entropic_inference_complete |
| REQ-INFER-017 | Backend state machine | entropic_inference_load, entropic_inference_activate, entropic_inference_deactivate, entropic_inference_unload, entropic_inference_destroy |
| REQ-INFER-018 | Atomic lock-free state query | entropic_inference_state |
| REQ-INFER-019 | Token counting across states | entropic_inference_count_tokens |
| REQ-VALID-004 | Per-identity validation override | set_global_enabled |

**Total: 9 requirement(s) affected, 14 function(s) changed**
