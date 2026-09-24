## Change Impact Report

| REQ | Name | Functions Changed |
|-----|------|-------------------|
| REQ-CFG-006 | Fail-loud validation — reject bad or inert configuration at load time | expert_offload_conflict_reason |
| REQ-INFER-019 | Model pool dedup and VRAM residency policy govern which tier is resident | derive_auto_placement, read_gguf_shape, resolve_auto_gpu_layers, estimate_vram_footprint |
| REQ-INFER-027 | Expert-tensor offload places routed experts on the host while attention and KV stay resident (EXPERIMENTAL) | expert_offload_conflict_reason, expert_offload_load_refusal |

**Total: 3 requirement(s) affected, 7 function(s) changed**
