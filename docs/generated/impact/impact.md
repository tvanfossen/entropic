## Change Impact Report

| REQ | Name | Functions Changed |
|-----|------|-------------------|
| REQ-IDEN-001 | Tier resolution contract and per-tier loop overrides | build_params_json |
| REQ-LOOP-004 | Empty-turn allowance and tool-call correction nudge | restrict_tools |
| REQ-LOOP-006 | Interrupt and pause semantics across nested loops | spawn_cancel_poller |
| REQ-LOOP-007 | Engine-authored transcript shaping keeps the system prompt bit-stable | prepare_prompts, inject_engine_state_reminder |

**Total: 4 requirement(s) affected, 5 function(s) changed**
