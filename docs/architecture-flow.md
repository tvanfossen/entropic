# Entropic Architecture Flow

```
┌ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ┐
  YOUR CODE  (examples/hello-world/main.py)                                                                                                 │
│
  ┌───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐   │
│ │ from entropic import ConfigLoader, ModelOrchestrator, AgentEngine, EngineCallbacks, LoopConfig, setup_logging                        │
  └───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘   │
│
  STEP 1: LOAD CONFIG                  STEP 2: LOGGING          STEP 3: ORCHESTRATOR         STEP 4: ENGINE + CALLBACKS                     │
│ ┌──────────────────────────────┐     ┌─────────────────┐      ┌────────────────────────┐   ┌──────────────────────────────────────┐
  │ loader = ConfigLoader(       │     │ setup_logging(  │      │ orch = ModelOrch-      │   │ engine = AgentEngine(orch, config,  │        │
│ │   project_root=EXAMPLE_ROOT, │     │   config,       │      │   estrator(config)     │   │   loop_config=LoopConfig(           │
  │   app_dir_name=".hello-world"│     │   project_dir,  │      │                        │   │     max_iterations=5,               │        │
│ │   default_config_path=...,   │     │   app_dir_name  │      │ await orch.initialize()│   │     auto_approve_tools=True))       │
  │   global_config_dir=None,    │     │ )               │      │                        │   │                                     │        │
│ │ )                            │     └─────────────────┘      └────────────────────────┘   │ engine.set_callbacks(               │
  │ config = loader.load()       │                                                           │   EngineCallbacks(                  │        │
│ │                              │                                                           │     on_stream_chunk=...,            │
  │  Reads: config.yaml          │                                                           │     on_tier_selected=...,           │        │
│ │  Seeds: .hello-world/        │                                                           │ ))                                  │
  │    config.local.yaml         │                                                           └──────────────────────────────────────┘        │
│ └──────────────────────────────┘
                                                                                                                                            │
│ STEP 5: RUN LOOP
  ┌──────────────────────────────┐                                                                                                          │
│ │ prompt = input("You: ")      │
  │ async for _msg in            │─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ┐                                                          │
│ │   engine.run(prompt): pass   │     YOU CALL engine.run(prompt)
  └──────────────────────────────┘                                                │                                                          │
│
  STEP 6: SHUTDOWN                                                                │                                                          │
│ ┌──────────────────────────────┐
  │ await orch.shutdown()        │                                                │                                                          │
│ └──────────────────────────────┘
 ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ┼ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ┘
                                                                                  │
══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
                                                                                  │
┌ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─│─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ┐
  LIBRARY INTERNALS  (what happens when you call engine.run())                    │                                                          │
│                                                                                 ▼
                                                                                                                                            │
│ ┌─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
  │                                         AgentEngine.run(prompt)                                                                       │ │
│ │                                         src/entropic/core/engine.py                                                                   │
  │                                                                                                                                       │ │
│ │  1. Lazy-init ServerManager (if none provided)                                                                                        │
  │  2. Create LoopContext (messages, state, metrics, locked_tier)                                                                         │ │
│ │  3. Build base system prompt via ContextBuilder                                                                                       │
  │  4. List available tools from ServerManager                                                                                            │ │
│ │  5. Append user message, reinject context anchors                                                                                     │
  │  6. Enter _loop()                                                                                                                      │ │
│ └──────────────────────────────────────────────────────┬──────────────────────────────────────────────────────────────────────────────────┘
                                                         │                                                                                  │
│                                                        ▼
  ┌──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐│
│ │                                                                                                                                        │
  │                                                  _loop()                                                                               ││
│ │                                            (plan → act → observe)                                                                      │
  │                                                                                                                                        ││
│ │  while not _should_stop(ctx):                                                                                                          │
  │    iteration++                                                                                                                         ││
│ │    if interrupted: break                                                                                                               │
  │                                                                                                                                        ││
│ │    ┌──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐  │
  │    │                                        _execute_iteration(ctx)                                                                 │  ││
│ │    │                                                                                                                                │  │
  │    │  ┌──────────────┐     ┌──────────────────────┐     ┌──────────────────────┐     ┌───────────────────────┐                       │  ││
│ │    │  │ COMPACTION   │     │ ROUTE & LOCK TIER    │     │ GENERATE RESPONSE    │     │ PROCESS RESULTS      │                       │  │
  │    │  │ CHECK        │────►│ (first iter only)    │────►│                      │────►│                      │                       │  ││
│ │    │  │              │     │                      │     │                      │     │                      │                       │  │
  │    │  │ Token usage  │     │ See ROUTING below    │     │ See GENERATION below │     │ See TOOL EXECUTION   │                       │  ││
│ │    │  │ > 80%?       │     │                      │     │                      │     │ below if tool_calls  │                       │  │
  │    │  │ If yes:      │     │                      │     │                      │     │                      │                       │  ││
│ │    │  │ summarize    │     │                      │     │                      │     │ If no tool_calls:    │                       │  │
  │    │  │ old messages │     │                      │     │                      │     │ check finish_reason  │                       │  ││
│ │    │  │              │     │                      │     │                      │     │ "stop" → COMPLETE    │                       │  │
  │    │  │              │     │                      │     │                      │     │ "length" → continue  │                       │  ││
│ │    │  └──────────────┘     └──────────────────────┘     └──────────────────────┘     └───────────┬───────────┘                       │  │
  │    │                                                                                            │                                   │  ││
│ │    │                                                                                            │ if COMPLETE or ERROR → exit loop   │  │
  │    │                                                                                            │ else → next iteration              │  ││
│ │    └────────────────────────────────────────────────────────────────────────────────────────────┼──────────────────────────────────────┘  │
  │                                                                                                │                                       ││
│ │                                                     ◄──────────────────────────────────────────┘                                        │
  │                                                                                                                                        ││
│ └────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘
                                                                                                                                            │
│
 ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ┘


══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
  ROUTING DETAIL  (what happens inside _lock_tier_if_needed)
══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════

  ┌────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
  │                                      _lock_tier_if_needed(ctx)                                                                       │
  │                                      Called once per engine.run() — locks tier for entire loop                                        │
  │                                                                                                                                      │
  │                                                                                                                                      │
  │  ┌──────────────────────────────┐                                                                                                    │
  │  │ orchestrator.route(messages) │                                                                                                    │
  │  │ src/entropic/inference/      │                                                                                                    │
  │  │   orchestrator.py            │                                                                                                    │
  │  └──────────────┬───────────────┘                                                                                                    │
  │                 │                                                                                                                     │
  │                 ▼                                                                                                                     │
  │  ┌──────────────────────────────────────────────────────────────────────────┐                                                         │
  │  │ _classify_task(messages)                                                │                                                         │
  │  │                                                                         │                                                         │
  │  │  ┌─────────────────────────────────────────────────────────────┐        │                                                         │
  │  │  │ PromptManager.build_classification_prompt()                 │        │                                                         │
  │  │  │                                                             │        │                                                         │
  │  │  │ Auto-generates routing prompt from tier metadata:           │        │                                                         │
  │  │  │                                                             │        │                                                         │
  │  │  │   "Classify this request. Reply with ONLY a digit."        │        │                                                         │
  │  │  │   "1 = normal: general conversation, simple questions"     │        │                                                         │
  │  │  │   "2 = thinking: complex reasoning, analysis, planning"    │        │                                                         │
  │  │  │                                                             │        │                                                         │
  │  │  │ Focus/examples sourced from:                                │        │                                                         │
  │  │  │   1. TierConfig.focus in config.yaml            (explicit) │        │                                                         │
  │  │  │   2. identity_{tier}.md frontmatter             (fallback) │        │                                                         │
  │  │  └─────────────────────────────────────────────────────────────┘        │                                                         │
  │  │                          │                                              │                                                         │
  │  │                          ▼                                              │                                                         │
  │  │  ┌─────────────────────────────────────────────────────────────┐        │                                                         │
  │  │  │ Router Model (Qwen3-0.6B, always loaded, separate VRAM)    │        │      VRAM LAYOUT                                        │
  │  │  │                                                             │        │      ┌───────────────────────────────────┐               │
  │  │  │ Input:  classification prompt + user message                │        │      │ GPU VRAM                          │               │
  │  │  │ Output: single digit "1" or "2"                             │        │      │                                   │               │
  │  │  │ Prompt instructs model to reply with ONLY a digit          │        │      │ ┌───────────┐ ┌────────────────┐  │               │
  │  │  └───────────────────────────┬─────────────────────────────────┘        │      │ │ Router    │ │ ONE Main Tier  │  │               │
  │  │                              │                                          │      │ │ 0.6B      │ │ (normal OR     │  │               │
  │  │                              ▼                                          │      │ │ always    │ │  thinking)     │  │               │
  │  │  ┌─────────────────────────────────────────────────────────────┐        │      │ │ loaded    │ │                │  │               │
  │  │  │ tier_map lookup:  "1" → normal, "2" → thinking             │        │      │ │ ~0.6 GB   │ │ 5-10 GB        │  │               │
  │  │  │ (auto-derived from tier order, or explicit in config)      │        │      │ └───────────┘ └────────────────┘  │               │
  │  │  └───────────────────────────┬─────────────────────────────────┘        │      └───────────────────────────────────┘               │
  │  └──────────────────────────────┼──────────────────────────────────────────┘                                                         │
  │                                 │                                                                                                     │
  │                                 ▼                                                                                                     │
  │  ┌──────────────────────────────────────────────────────────────────────────┐                                                         │
  │  │ _get_model(tier) — VRAM swap under asyncio.Lock                         │                                                         │
  │  │                                                                         │                                                         │
  │  │   Is target tier already loaded?                                        │                                                         │
  │  │     YES ──► swap_action="none", return model                            │                                                         │
  │  │     NO  ──► Same GGUF file as loaded tier?                              │                                                         │
  │  │               YES ──► swap_action="reused", return loaded model         │                                                         │
  │  │               NO  ──► Unload current main tier from VRAM                │                                                         │
  │  │                       Load new tier GGUF into VRAM                      │                                                         │
  │  │                       swap_action="loaded"                              │                                                         │
  │  └──────────────────────────────────────────────────────────────────────────┘                                                         │
  │                                 │                                                                                                     │
  │                                 ▼                                                                                                     │
  │  ┌──────────────────────────────────────────────────────────────────────────┐                                                         │
  │  │ Rebuild system prompt for locked tier                                    │                                                         │
  │  │                                                                         │                                                         │
  │  │   adapter.format_system_prompt(base_system, tier_filtered_tools)         │                                                         │
  │  │                                                                         │                                                         │
  │  │   Assembled from PromptManager (see PROMPT ASSEMBLY below):             │                                                         │
  │  │     constitution.md  →  safety guardrails (bundled default)             │                                                         │
  │  │     identity_{tier}.md → tier persona/capability (bundled default)      │                                                         │
  │  │     app_context.md   →  consumer personality (hello-world custom)       │                                                         │
  │  │     + tool definitions (filtered to tier's allowed_tools)               │                                                         │
  │  └──────────────────────────────────────────────────────────────────────────┘                                                         │
  │                                 │                                                                                                     │
  │                                 ▼                                                                                                     │
  │  Fire callbacks:  on_tier_selected("normal")  →  your lambda prints "[routed to: normal]"                                             │
  │                   on_routing_complete(RoutingResult{tier, swap_action, routing_ms})                                                    │
  └────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘


══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
  GENERATION DETAIL  (what happens inside _generate_response)
══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════

  ┌────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
  │                                      _generate_response(ctx)                                                                          │
  │                                                                                                                                      │
  │  ┌──────────────────────────────────────────────────────────────────────────┐                                                         │
  │  │ orchestrator.generate_stream(messages, tier=locked_tier)                 │                                                         │
  │  │ src/entropic/inference/orchestrator.py                                   │                                                         │
  │  │                                                                         │                                                         │
  │  │   ┌──────────────────────────────────────────────────────────────┐      │                                                         │
  │  │   │ LlamaCppBackend.generate_stream(messages)                   │      │                                                         │
  │  │   │ src/entropic/inference/llama_cpp.py                         │      │                                                         │
  │  │   │                                                             │      │                                                         │
  │  │   │  1. adapter.format_system_prompt()                          │      │                                                         │
  │  │   │     ┌────────────────────────────────────────────┐          │      │                                                         │
  │  │   │     │ ChatAdapter (e.g. Qwen3Adapter)           │          │      │                                                         │
  │  │   │     │ src/entropic/inference/adapters/qwen3.py  │          │      │                                                         │
  │  │   │     │                                           │          │      │                                                         │
  │  │   │     │ Formats messages into model-specific      │          │      │                                                         │
  │  │   │     │ chat template with system prompt,         │          │      │                                                         │
  │  │   │     │ tool definitions in model's expected      │          │      │                                                         │
  │  │   │     │ format, and conversation history          │          │      │                                                         │
  │  │   │     └────────────────────────────────────────────┘          │      │                                                         │
  │  │   │                                                             │      │                                                         │
  │  │   │  2. llama-cpp-python Llama.create_chat_completion()         │      │                                                         │
  │  │   │     stream=True                                             │      │                                                         │
  │  │   │                                                             │      │                                                         │
  │  │   │  3. Yield token chunks ─────────────────────────────────────┼──────┼──► on_stream_chunk(chunk)                                │
  │  │   │     as they're generated                                    │      │    your lambda prints each chunk                          │
  │  │   │                                                             │      │                                                         │
  │  │   └─────────────────────────────────────────────────────────────┘      │                                                         │
  │  └──────────────────────────────────────────────────────────────────────────┘                                                         │
  │                                 │                                                                                                     │
  │                                 ▼ full response accumulated                                                                           │
  │  ┌──────────────────────────────────────────────────────────────────────────┐                                                         │
  │  │ adapter.parse_tool_calls(content)                                        │                                                         │
  │  │                                                                         │                                                         │
  │  │ Scans response for <tool_call>{"name": "...", "arguments": {...}}       │                                                         │
  │  │                      </tool_call> tags                                  │                                                         │
  │  │                                                                         │                                                         │
  │  │ Returns: (cleaned_content, list[ToolCall])                              │                                                         │
  │  │                                                                         │                                                         │
  │  │ Content-based parsing (not native function calling) due to              │                                                         │
  │  │ chatml-function-calling template issues in llama-cpp-python             │                                                         │
  │  └──────────────────────────────────────────────────────────────────────────┘                                                         │
  └────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘


══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
  TOOL EXECUTION DETAIL  (when model output contains tool calls)
══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════

  ┌────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
  │                                   _process_tool_calls(ctx, tool_calls)                                                                │
  │                                                                                                                                      │
  │  For each ToolCall:                                                                                                                   │
  │                                                                                                                                      │
  │  ┌────────────────────┐     ┌──────────────────────┐     ┌──────────────────────────────────────────────────────────────────────────┐  │
  │  │ Duplicate check    │     │ Permission check     │     │ ServerManager.execute_tool(tool_call)                                   │  │
  │  │                    │     │                      │     │ src/entropic/mcp/manager.py                                             │  │
  │  │ Same name + args   │     │ auto_approve=True    │     │                                                                        │  │
  │  │ as recent call?    │     │ in hello-world       │     │  ┌─────────────────────────────────────────────────────────────────┐    │  │
  │  │                    │     │                      │     │  │ Route to server by tool name prefix                            │    │  │
  │  │ 3x consecutive     │     │ Otherwise:           │     │  │                                                               │    │  │
  │  │ duplicates →       │     │ on_tool_call →       │     │  │ "filesystem.read_file"  → FilesystemServer                    │    │  │
  │  │ circuit breaker    │     │ prompt user for      │     │  │ "bash.execute"          → BashServer                          │    │  │
  │  │ (force stop)       │     │ Allow/Deny/          │     │  │ "git.status"            → GitServer                           │    │  │
  │  │                    │     │ Always Allow/         │     │  │ "diagnostics.health"    → DiagnosticsServer                   │    │  │
  │  └────────┬───────────┘     │ Always Deny          │     │  │ "entropic.delegate"      → EntropicServer                     │    │  │
  │           │ pass            └──────────┬───────────┘     │  │                                                               │    │  │
  │           └────────────────────────────┘                 │  │ All extend BaseMCPServer (src/entropic/mcp/servers/base.py)   │    │  │
  │                                        │                 │  └──────────────────────────────┬────────────────────────────────┘    │  │
  │                                        │                 │                                 │                                    │  │
  │                                        │ approved        │                                 ▼                                    │  │
  │                                        └─────────────────┤  ┌─────────────────────────────────────────────────────────────────┐ │  │
  │                                                          │  │ InProcessProvider.call_tool()                                  │ │  │
  │                                                          │  │ src/entropic/mcp/provider.py                                   │ │  │
  │                                                          │  │                                                               │ │  │
  │                                                          │  │ Returns: ServerResponse { result: str, directives: list }     │ │  │
  │                                                          │  └──────────────────────────────┬──────────────────────────────────┘ │  │
  │                                                          └─────────────────────────────────┼────────────────────────────────────┘  │
  │                                                                                            │                                      │
  │                                                                                            ▼                                      │
  │  ┌──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐   │
  │  │ DirectiveProcessor.process(directives)                                                                                      │   │
  │  │ src/entropic/core/directives.py                                                                                             │   │
  │  │                                                                                                                             │   │
  │  │ Directives are tool→engine signals returned in _directives key of tool results:                                             │   │
  │  │                                                                                                                             │   │
  │  │   StopProcessing  ──► halt agentic loop                                                                                     │   │
  │  │   Delegate        ──► spawn child tier in worktree, await result, return to parent                                               │   │
  │  │   InjectContext   ──► insert a message into conversation                                                                     │   │
  │  │   ContextAnchor   ──► persist a keyed message across run() calls                                                            │   │
  │  │   PruneMessages   ──► drop old tool results from context                                                                     │   │
  │  │   NotifyPresenter ──► fire UI-level event (status bar update, etc.)                                                          │   │
  │  └──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘   │
  │                                                                                            │                                      │
  │                                                                                            ▼                                      │
  │  Tool result injected as user message → model sees it on next iteration → loop continues                                          │
  └────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘


══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
  PROMPT ASSEMBLY  (what the model actually sees as its system prompt)
══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════

  PromptManager assembles prompts from three sources, each configurable per-consumer:

  ┌─────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
  │                                                                                                                │
  │   ┌───────────────────────────────────────────────────────────────────────────────┐                            │
  │   │ 1. CONSTITUTION  (safety guardrails — bundled with entropic-engine)           │                            │
  │   │    src/entropic/data/prompts/constitution.md                                  │                            │
  │   │                                                                               │                            │
  │   │    Frontmatter: type: constitution, version: 1                                │                            │
  │   │    Body: safety rules, behavioral constraints                                 │                            │
  │   │                                                                               │                            │
  │   │    Override: config.constitution = "path/to/custom.md" | false (disable)      │                            │
  │   │    Default: bundled (always present unless explicitly disabled)                │                            │
  │   └───────────────────────────────────────────────────────────────────────────────┘                            │
  │                                          │                                                                     │
  │                                          ▼                                                                     │
  │   ┌───────────────────────────────────────────────────────────────────────────────┐                            │
  │   │ 2. IDENTITY  (tier-specific persona — bundled defaults, overridable)          │                            │
  │   │    src/entropic/data/prompts/identity_normal.md                               │                            │
  │   │    src/entropic/data/prompts/identity_thinking.md                             │                            │
  │   │                                                                               │                            │
  │   │    Frontmatter: type: identity, version: 1, name: "Normal"                   │                            │
  │   │                 focus: ["general conversation", "simple questions"]            │  ◄── used by router to    │
  │   │                 examples: ["hello", "what is ..."]                             │      build classification │
  │   │                                                                               │      prompt automatically │
  │   │    Body: tier-specific behavioral instructions                                │                            │
  │   │                                                                               │                            │
  │   │    Override: models.tiers.normal.identity = "path/to/custom.md"               │                            │
  │   └───────────────────────────────────────────────────────────────────────────────┘                            │
  │                                          │                                                                     │
  │                                          ▼                                                                     │
  │   ┌───────────────────────────────────────────────────────────────────────────────┐                            │
  │   │ 3. APP CONTEXT  (consumer-specific — hello-world provides this)               │  ◄── YOUR CUSTOMIZATION   │
  │   │    examples/hello-world/prompts/app_context.md                                │      POINT                 │
  │   │                                                                               │                            │
  │   │    Frontmatter: type: app_context, version: 1                                │                            │
  │   │    Body: "You are running on Entropic, a local-first agentic inference        │                            │
  │   │           engine... In this example, you have two tiers: Normal, Thinking..." │                            │
  │   │                                                                               │                            │
  │   │    This is how consumers give the model project-specific awareness            │                            │
  │   │    Referenced in config.yaml: app_context: prompts/app_context.md             │                            │
  │   │    Default: disabled (None) — only active when consumer provides path         │                            │
  │   └───────────────────────────────────────────────────────────────────────────────┘                            │
  │                                          │                                                                     │
  │                                          ▼                                                                     │
  │   ┌───────────────────────────────────────────────────────────────────────────────┐                            │
  │   │ 4. TOOL DEFINITIONS  (filtered per-tier by allowed_tools config)              │                            │
  │   │                                                                               │                            │
  │   │    Injected by adapter into model-specific format                             │                            │
  │   │    e.g. Qwen3 uses <tools>[...json schemas...]</tools>                       │                            │
  │   │                                                                               │                            │
  │   │    In hello-world: all MCP servers disabled, so no tools injected             │                            │
  │   └───────────────────────────────────────────────────────────────────────────────┘                            │
  │                                          │                                                                     │
  │                                          ▼                                                                     │
  │                    ┌─────────────────────────────────────────────┐                                             │
  │                    │ Final system prompt =                       │                                             │
  │                    │   constitution + identity + app_context     │                                             │
  │                    │   + tool defs + ContextBuilder additions    │                                             │
  │                    │   (ENTROPIC.md project context if present)  │                                             │
  │                    └─────────────────────────────────────────────┘                                             │
  │                                                                                                                │
  └─────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘


══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
  CONFIGURATION LOADING  (what ConfigLoader does at startup)
══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════

  ┌────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
  │                                                                                                                                      │
  │  ConfigLoader(project_root, app_dir_name, default_config_path, global_config_dir)                                                    │
  │                                                                                                                                      │
  │  Merge order (last wins):                                                                                                            │
  │                                                                                                                                      │
  │  ┌─────────────────────────────┐                                                                                                     │
  │  │ 1. Code defaults            │  EntropyConfig() pydantic defaults                                                                  │
  │  │    (built-in)               │                                                                                                     │
  │  └─────────────┬───────────────┘                                                                                                     │
  │                ▼                                                                                                                      │
  │  ┌─────────────────────────────┐                                                                                                     │
  │  │ 2. Global config            │  ~/.entropic/config.yaml  (None in hello-world — skipped)                                           │
  │  │    (user-wide)              │                                                                                                     │
  │  └─────────────┬───────────────┘                                                                                                     │
  │                ▼                                                                                                                      │
  │  ┌─────────────────────────────┐                                                                                                     │
  │  │ 3. Default config           │  examples/hello-world/config.yaml  (tiers, router, routing, mcp)                                    │
  │  │    (shipped with example)   │                                                                                                     │
  │  └─────────────┬───────────────┘                                                                                                     │
  │                ▼                                                                                                                      │
  │  ┌─────────────────────────────┐                                                                                                     │
  │  │ 4. Project-local config     │  .hello-world/config.local.yaml  (seeded on first run, user edits model paths)                      │
  │  │    (gitignored, user edits) │                                                                                                     │
  │  └─────────────┬───────────────┘                                                                                                     │
  │                ▼                                                                                                                      │
  │  ┌─────────────────────────────┐                                                                                                     │
  │  │ 5. CLI overrides            │  loader.load(cli_overrides={...})  (not used in hello-world)                                        │
  │  │    (runtime)                │                                                                                                     │
  │  └─────────────┬───────────────┘                                                                                                     │
  │                ▼                                                                                                                      │
  │        EntropyConfig                                                                                                                  │
  │        (validated pydantic model with all settings resolved)                                                                          │
  │                                                                                                                                      │
  └────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘


══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
  COMPLETE HELLO-WORLD SEQUENCE  (end-to-end for: User types "hello")
══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════

  User types: "hello"
       │
       ▼
  engine.run("hello")
       │
       ├──► LoopContext created, system prompt built via ContextBuilder
       │    Messages: [system, user:"hello"]
       │
       ├──► _lock_tier_if_needed
       │       │
       │       ├──► orchestrator.route([system, user:"hello"])
       │       │       │
       │       │       ├──► build_classification_prompt() from tier focus metadata
       │       │       │    "1 = normal: general conversation ...  2 = thinking: complex reasoning ..."
       │       │       │
       │       │       ├──► Router (Qwen3-0.6B) generates: "1"
       │       │       │
       │       │       ├──► tier_map["1"] → normal
       │       │       │
       │       │       └──► _get_model(normal): already loaded → swap_action="none"
       │       │
       │       ├──► Rebuild system prompt with Qwen3Adapter.format_system_prompt()
       │       │    (constitution + identity_normal.md + app_context.md + no tools)
       │       │
       │       └──► on_tier_selected("normal") → prints "[routed to: normal]"
       │
       ├──► _generate_streaming
       │       │
       │       ├──► orchestrator.generate_stream(messages, tier=normal)
       │       │       │
       │       │       └──► LlamaCppBackend (Qwen3-8B) streams tokens
       │       │
       │       ├──► Each chunk → on_stream_chunk(chunk) → prints to terminal
       │       │
       │       └──► adapter.parse_tool_calls(full_response)
       │            Returns: ("Hi there! How can I help?", [])  ← no tool calls
       │
       ├──► No tool_calls + finish_reason="stop" → AgentState.COMPLETE
       │
       └──► Loop exits, yields assistant Message


  User types: "design a REST API for a todo app"
       │
       ▼
  engine.run("design a REST API for a todo app")
       │
       ├──► LoopContext created (fresh — new run() call)
       │
       ├──► _lock_tier_if_needed
       │       │
       │       ├──► orchestrator.route(...)
       │       │       │
       │       │       ├──► Router (Qwen3-0.6B) generates: "2"
       │       │       │
       │       │       ├──► tier_map["2"] → thinking
       │       │       │
       │       │       └──► _get_model(thinking):
       │       │            normal currently loaded → unload normal → load thinking
       │       │            swap_action="loaded"
       │       │
       │       └──► on_tier_selected("thinking") → prints "[routed to: thinking]"
       │
       ├──► _generate_streaming
       │       │
       │       └──► LlamaCppBackend (Qwen3-14B) streams tokens
       │            (may include <think>...</think> reasoning blocks)
       │
       └──► Complete
```
