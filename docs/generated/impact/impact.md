## Change Impact Report

| REQ | Name | Functions Changed |
|-----|------|-------------------|
| REQ-API-005 | Uniform precondition guard on every exported entry point | entropic_release_model |
| REQ-API-009 | Run entry-point family, result contract, and cross-thread interruptibility | claim_exclusive |
| REQ-INFER-019 | Model pool dedup and VRAM residency policy govern which tier is resident | entropic_release_model |
| REQ-MCP-020 | A bounded, thread-safe ring buffer retains recent tool calls for introspection | to_json, truncate_result |

**Total: 4 requirement(s) affected, 5 function(s) changed**
