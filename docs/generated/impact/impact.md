## Change Impact Report

| REQ | Name | Functions Changed |
|-----|------|-------------------|
| REQ-LOOP-009 | Per-session-key run concurrency, on by default, with one lock on the generation path | record_read, was_read |
| REQ-MCP-001 | MCPServerBase holds the shared logic; concrete servers override only deltas | FilesystemServer |
| REQ-MCP-021 | Filesystem tools are root-confined, read-before-write gated, and size-bounded | record_read, was_read, path_within, first_containing, outside_root_refusal, refuse_denied, refuse_escape, collect_glob_matches, check_read_gates, execute, execute, execute, execute, FilesystemServer, set_working_dir, max_read_bytes, resolve_path, authorize_outside_root, ask_outside_root_approver, set_outside_root_approver, log_outside_root_policy |
| REQ-MCP-022 | Glob, grep, and read honour .gitignore plus .explorerignore semantics | collect_glob_matches, check_read_gates, grep_search, execute, FilesystemServer, set_working_dir |

**Total: 4 requirement(s) affected, 30 function(s) changed**
