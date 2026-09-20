## Change Impact Report

| REQ | Name | Functions Changed |
|-----|------|-------------------|
| REQ-MCP-021 | Filesystem tools are root-confined, read-before-write gated, and size-bounded | collect_glob_matches, execute |
| REQ-MCP-022 | Glob, grep, and read honour .gitignore plus .explorerignore semantics | collect_glob_matches, grep_search, execute, is_skipped_dir_name, compile_pattern, push_rule, add_pattern, load_file, load, load_nested_gitignores, visit_discovery_entry, prunes_discovery, is_ignored |

**Total: 2 requirement(s) affected, 15 function(s) changed**
