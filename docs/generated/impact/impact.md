## Change Impact Report

| REQ | Name | Functions Changed |
|-----|------|-------------------|
| REQ-LOOP-006 | Interrupt and pause semantics across nested loops | poll_stop |
| REQ-MCP-023 | Bash and git servers gate on operator approval, not command pattern matching; bash enforces a timeout on the whole process group | poll_stop, collect_until_exit, run_shell, cancelled_error, shell_result, execute |

**Total: 2 requirement(s) affected, 7 function(s) changed**
