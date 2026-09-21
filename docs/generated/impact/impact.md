## Change Impact Report

| REQ | Name | Functions Changed |
|-----|------|-------------------|
| REQ-DELEG-002 | Delegation lifecycle from preconditions to result fold-back | check_delegation_preconditions, execute_delegation, build_resumed_child_context, execute_resume_delegation, build_child_context, extract_summary |
| REQ-DELEG-004 | Sequential pipeline with forward carry and per-stage reporting | execute_pipeline, run_pipeline_stage |
| REQ-DELEG-005 | Opt-in delegation sandbox isolation, wired end to end | mint_delegation_id, restore_root_for |
| REQ-DELEG-006 | Explicit delegation context, required-context tiers, resume by target | format_context_block |

**Total: 4 requirement(s) affected, 11 function(s) changed**
