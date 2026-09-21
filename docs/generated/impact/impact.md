## Change Impact Report

| REQ | Name | Functions Changed |
|-----|------|-------------------|
| REQ-DELEG-006 | Explicit delegation context, required-context tiers, resume by target | DelegateTool, execute, pipeline_rejection, execute |
| REQ-MCP-011 | Every tool declares its required access level, defaulting to WRITE | register_introspection_tools |
| REQ-MCP-013 | Tool arguments are validated against the declared JSON schema before dispatch | DelegateTool, PipelineTool |
| REQ-MCP-024 | Engine-level entropic.* tools validate their arguments and emit typed directives | DelegateTool, execute, PipelineTool, pipeline_rejection, execute, execute, register_delegation_tools, register_introspection_tools |

**Total: 4 requirement(s) affected, 15 function(s) changed**
