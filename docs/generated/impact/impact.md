## Change Impact Report

| REQ | Name | Functions Changed |
|-----|------|-------------------|
| REQ-INFER-002 | Teardown releases every model, context and adapter handle | teardown_mtp_draft |
| REQ-INFER-005 | Every decode path honours cooperative cancellation within one token | generate_after_prefill, do_generate_text_only, do_generate_streaming_text_only, generate_mtp |
| REQ-INFER-006 | Sampler chain construction has a fixed order and default-preserving gating | to_common_sampling |
| REQ-INFER-008 | Every declared grammar source reaches the sampler and exactly one wins | tool_grammar_for, to_common_sampling |
| REQ-INFER-009 | Tool staging drives a native render whose parse context is captured | to_common_chat, mcp_tools_to_common_chat, render_with_tools |
| REQ-INFER-015 | MTP speculative decode fails loud and never silently falls back | effective_n_draft, mtp_guard, generate_mtp |
| REQ-INFER-025 | Multimodal input is bounded, tier-gated and degrades gracefully | generate_multimodal |
| REQ-INFER-027 | Expert-tensor offload places routed experts on the host while attention and KV stay resident (EXPERIMENTAL) | build_load_mparams, model_expert_count, expert_offload_admits, load_gpu_model |
| REQ-TYPE-004 | Sentinel count members bound enum validity and force exhaustive wiring | do_supports |

**Total: 9 requirement(s) affected, 20 function(s) changed**
