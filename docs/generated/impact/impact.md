## Change Impact Report

| REQ | Name | Functions Changed |
|-----|------|-------------------|
| REQ-DELEG-006 | Explicit delegation context, required-context tiers, resume by target | DelegateTool, execute, pipeline_rejection, execute |
| REQ-LOOP-009 | Per-session-key run concurrency, on by default, with one lock on the generation path | execute |
| REQ-MCP-001 | MCPServerBase holds the shared logic; concrete servers override only deltas | register_core_tools |
| REQ-MCP-011 | Every tool declares its required access level, defaulting to WRITE | register_introspection_tools |
| REQ-MCP-013 | Tool arguments are validated against the declared JSON schema before dispatch | DelegateTool, PipelineTool |
| REQ-MCP-024 | Engine-level entropic.* tools validate their arguments and emit typed directives | execute, DelegateTool, execute, PipelineTool, pipeline_rejection, execute, execute, execute, register_core_tools, register_delegation_tools, register_introspection_tools |

**Total: 6 requirement(s) affected, 20 function(s) changed**
