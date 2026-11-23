## Change Impact Report

| REQ | Name | Functions Changed |
|-----|------|-------------------|
| REQ-API-004 | Three configure entry points, one shared body, one-shot per handle | validate_prompt_sources |
| REQ-API-005 | Uniform precondition guard on every exported entry point | entropic_workspace_set_app_context |
| REQ-API-012 | Final-answer selection from a serialized conversation | turn_start, substantive_assistant_text |
| REQ-API-014 | Header is the generator input for the Python wrapper; drift is a commit gate | parse_enum, parse_struct, parse_functions, render_bindings, render_manifest |
| REQ-BRIDGE-001 | External bridge exposes the engine over a peer-authenticated unix socket | tool_definitions, session_tool_names, mark_tasks_cancelling, any_cancelling_left, turn_timeout_result, bind_session_workspace, tool_handlers, initialize_result, sentinel_suffix_for_status, configure_server, bind_loopback |
| REQ-CFG-001 | Layered configuration resolution with most-specific-layer-wins precedence | apply_env, apply_mcp_external_env |
| REQ-CFG-005 | Per-tier knobs override global defaults without an all-or-nothing switch | parse_gpu_layers |
| REQ-COMPACT-001 | Threshold compaction preserving task-bearing messages, snapshot first | partition_messages, assemble_compacted, structured_summary, format_summary, serialize_messages_json |
| REQ-DELEG-005 | Opt-in delegation sandbox isolation, wired end to end | parse_delegation_config |
| REQ-HOOK-001 | Thread-safe hook dispatch with priority, chaining and isolation | is_valid_point, is_valid_point |
| REQ-HOOK-002 | Engine fires hooks at every lifecycle boundary, including a vetoable completion | fire_hook_info, build_tool_manifest, build_tool_evidence, build_tool_results_json |
| REQ-IDEN-002 | Identity lifecycle separates immutable static from dynamic identities | fire_cache_invalidator, fire_cache_invalidator |
| REQ-INFER-002 | Teardown releases every model, context and adapter handle | promote_to_active, promote_to_active, load_and_activate, teardown_mtp_draft |
| REQ-INFER-007 | Named grammars are registered, validated, and resolved by a fixed precedence | refuse_unresolved_tier_grammar, refuse_unresolved_batch_grammars, holds_grammars, warn_tier_grammars_step, build_grammar_entry, normalize_grammar_key |
| REQ-INFER-008 | Every declared grammar source reaches the sampler and exactly one wins | apply_grammar_source |
| REQ-INFER-009 | Tool staging drives a native render whose parse context is captured | to_common_chat, mcp_tools_to_common_chat |
| REQ-INFER-010 | One template-first / adapter-second rule parses every raw emission | apply_fenced_fallbacks, one_call_satisfies_schema |
| REQ-INFER-012 | Adapter registry resolves a family parser with a safe generic fallback | action_envelope_to_call, coerce_call_string_args, parse_recovered_tool_call, regex_recovered_tool_call, open_tag_pattern, truncate_at_nested_function, find_close_tag, emit_arg |
| REQ-INFER-015 | MTP speculative decode fails loud and never silently falls back | effective_n_draft |
| REQ-INFER-016 | Speculative decoding is gated on compatibility and must not change the output | build_compat_diagnostic |
| REQ-INFER-019 | Model pool dedup and VRAM residency policy govern which tier is resident | estimate_footprint_bytes, log_fit_recommendation, resolve_vram_budget_bytes, ensure_vram_budget, residency_admits, resolve_auto_gpu_layers, config_admits, refuse_residency, release_backend, announce_eviction, require_ready_backend, try_expert_offload, try_fewer_layers, try_smaller_ubatch, log_fit_recommendation, resolve_auto_gpu_layers, config_admits, refuse_residency, release_backend, announce_eviction, resolve_vram_budget_bytes, ensure_vram_budget, footprint_inputs_for, estimate_footprint_bytes, touch, forget, free_slot, lru_key |
| REQ-INFER-020 | Tier routing, handoff rules and secondary model roles | ensure_secondary_roles, extract_latest_user_message, ensure_secondary_roles |
| REQ-INFER-021 | Per-tier sampler configuration applies only where the caller did not decide | apply_if_default |
| REQ-INFER-023 | LoRA adapters hot-swap under a single-HOT-per-context constraint | preload_adapters_for_model, apply_adapter, preload_adapters_for_model |
| REQ-INFER-025 | Multimodal input is bounded, tier-gated and degrades gracefully | bilinear_resize, read_image_bytes, decode, mtmd_prefill |
| REQ-INFER-026 | A prompt that cannot fit the tier context is refused, never decoded | context_fit_breakdown |
| REQ-INFER-027 | Expert-tensor offload places routed experts on the host while attention and KV stay resident (EXPERIMENTAL) | build_load_mparams, model_expert_count, expert_offload_admits, load_gpu_model |
| REQ-LOOP-006 | Interrupt and pause semantics across nested loops | this_thread_run, this_thread_run, spawn_cancel_poller |
| REQ-LOOP-008 | Family-aware streaming reasoning filter with UTF-8 alignment | utf8_char_len, process_byte |
| REQ-LOOP-010 | Session conversations are readable, writable, and losslessly round-trippable | set_session_messages |
| REQ-MCP-010 | Per-identity MCP key authorization is opt-in but default-deny once enabled | server_wildcard, server_wildcard |
| REQ-MCP-012 | Tool calls pass an ordered precondition pipeline with typed rejection kinds | should_stop_batch |
| REQ-MCP-013 | Tool arguments are validated against the declared JSON schema before dispatch | normalize_args |
| REQ-MCP-016 | Anti-spiral tracking warns softly then hard-blocks repeated identical tools | check_repeated_failure |
| REQ-MCP-021 | Filesystem tools are root-confined, read-before-write gated, and size-bounded | read_file_contents, write_file_contents, apply_str_replace, read_and_track, list_page |
| REQ-MCP-022 | Glob, grep, and read honour .gitignore plus .explorerignore semantics | should_skip_dir, any_glob_match, emit_one, strip_flags, normalize_base, make_base_prefix, collect_buckets, pop_highest, rule_matches |
| REQ-MCP-023 | Bash and git servers gate on operator approval, not command pattern matching; bash enforces a timeout on the whole process group | spawn_git, drain_and_close |
| REQ-MCP-024 | Engine-level entropic.* tools validate their arguments and emit typed directives | followup_results |
| REQ-MCP-025 | External servers are discovered safely, directive-stripped, and health-monitored | connect_declared_servers, connect_discovered_servers, watch, start, has_blocked_prefix, connect_declared_servers, connect_discovered_servers |
| REQ-MCP-027 | Named workspaces bind a session to its own tool root and servers | entropic_workspace_set_app_context, workspace_root_refusal |
| REQ-PROD-002 | mcp-bridge is a byte-transparent stdio-to-socket relay, never an engine | connect_unix_socket, service_revents |
| REQ-PROD-003 | Every release publishes CPU and CUDA tarballs, each with a .sha256 companion | _detect_backend, _sha256_of, _expected_sha256, _verify_existing |
| REQ-SAFE-001 | Untrusted bytes are sanitized at ingress, never at egress | valid_seq_len, su8_seq_len |
| REQ-STOR-002 | Serialized, parameter-bound statement execution | prepare |
| REQ-STOR-003 | Conversation and message persistence with generated identity | build_message_row, bind_message_insert, conversation_row_to_json, message_row_to_json |
| REQ-STOR-005 | Delegation record lifecycle with parent-conversation guard | bind_delegation_insert, guard_parent_conversation |
| REQ-STOR-008 | Append-only JSONL audit log with rotation and untruncated payloads | write_line, extract_parent_id, parse_result_fields, write_line, populate_from_hook_context |
| REQ-TYPE-005 | Config types declare their own defaults; optional means absent, not default | parse_gpu_layers |
| REQ-TYPE-006 | Multimodal message vocabulary keeps text readable to text-only callers | parse_one_message |
| REQ-VALID-002 | Critique-and-revise loop with typed verdicts and content safety valve | append_turn_evidence, build_revision_messages, extract_compliant_field, extract_violations, extract_revised_field, append_turn_evidence, build_revision_messages, extract_compliant_field, extract_violations, extract_revised_field |

**Total: 50 requirement(s) affected, 175 function(s) changed**
