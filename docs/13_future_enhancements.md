# Future Enhancements

> Post-baseline features and improvements for Entropi

**Status:** Planned for implementation after core baseline
**Priority:** Organized by impact and effort

---

## Overview

This document captures features discussed during planning that extend beyond the core baseline. These are valuable enhancements that will make Entropi more powerful but are not required for initial functionality.

---

## 1. Codebase Indexing (High Priority)

### Problem
Currently, Entropi relies on grep/ripgrep for code search, which burns context tokens by returning raw file contents. This is the same limitation Claude Code has (see GitHub issue #4556).

### Solution: Semantic Code Search MCP Server

**Architecture:**
```
┌──────────────────────────────────────────────────────────────────┐
│                    Codebase Indexer MCP Server                   │
├──────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌─────────────┐    ┌─────────────┐    ┌─────────────┐         │
│  │  AST Parser │───►│   Chunker   │───►│  Embedder   │         │
│  └─────────────┘    └─────────────┘    └──────┬──────┘         │
│                                               │                 │
│                                               ▼                 │
│                                        ┌─────────────┐         │
│                                        │ SQLite-vss  │         │
│                                        │  (vectors)  │         │
│                                        └─────────────┘         │
│                                                                  │
└──────────────────────────────────────────────────────────────────┘
```

**Key Components:**
- **SQLite + sqlite-vss**: Vector storage extension for SQLite
- **AST-based chunking**: Split by functions/classes, not arbitrary lines
- **0.5B embeddings**: Use micro model for embedding generation (fast, local)
- **Incremental updates**: Watch for file changes, re-index only modified

**MCP Tools:**
```python
tools = [
    {
        "name": "search_code",
        "description": "Semantic search across codebase",
        "parameters": {"query": "string", "limit": "int", "file_types": "list[str]"},
    },
    {
        "name": "get_related_files",
        "description": "Find files related to a given file",
        "parameters": {"file_path": "string", "limit": "int"},
    },
    {
        "name": "get_symbol_definition",
        "description": "Find where a symbol is defined",
        "parameters": {"symbol": "string"},
    },
    {
        "name": "index_status",
        "description": "Get indexing status and stats",
    },
]
```

**Expected Impact:**
- 40% reduction in tokens per search
- More relevant results
- Better understanding of project structure

**Estimated Effort:** 3-5 days

---

## 2. Extended Context via RAM Offloading

### Problem
32K token context limit is restrictive for large codebases.

### Solution: KV Cache RAM Offloading

**Configuration:**
```yaml
context:
  mode: fast  # fast | extended | max
  fast_tokens: 16384      # Full GPU, ~45-60 t/s
  extended_tokens: 65536  # KV in RAM, ~15-25 t/s
  max_tokens: 131072      # Mostly RAM, ~5-10 t/s
```

**Implementation:**
- llama-cpp-python supports `offload_kqv` parameter
- Runtime toggle via `/config context_mode extended`
- Automatic fallback when context exceeds threshold

**Slash Command:**
```
/context fast     # Switch to fast mode (16K)
/context extended # Switch to extended mode (64K)
/context max      # Switch to max mode (128K)
/context status   # Show current mode and usage
```

**Estimated Effort:** 1-2 days

---

## 3. Subagent Delegation System

### Problem
Complex tasks benefit from specialized sub-agents.

### Solution: Modular Delegator System

**Architecture:**
```python
class Delegator(Protocol):
    async def delegate(
        task: str,
        context: DelegationContext,
        config: DelegationConfig,
    ) -> DelegationResult: ...

# Delegation strategies:
strategies = {
    "subagent": SubagentDelegator,      # Spawn isolated instance
    "parallel": ParallelDelegator,      # Fan-out to multiple agents
    "specialist": SpecialistDelegator,  # Route to task-specific agent
    "human": HumanDelegator,            # Escalate to user
}
```

**Subagent Types:**
- **Code Review Agent**: Specialized in finding bugs and security issues
- **Test Writer Agent**: Focused on generating comprehensive tests
- **Documentation Agent**: Generates and updates documentation
- **Refactoring Agent**: Suggests and implements refactoring

**Depth Limiting:**
- Max subagent depth: 3
- Total subagent calls per task: 10
- Timeout per subagent: 60 seconds

**Estimated Effort:** 4-6 days

---

## 4. Web Search Integration

### Problem
No access to current documentation, libraries, or solutions.

### Solution: Web Search MCP Server

**Options:**
1. **SearXNG** (self-hosted, privacy-focused)
2. **Tavily API** (optimized for AI, requires key)
3. **Brave Search API** (good balance)

**MCP Tools:**
```python
tools = [
    {
        "name": "web_search",
        "description": "Search the web",
        "parameters": {"query": "string", "num_results": "int"},
    },
    {
        "name": "fetch_page",
        "description": "Fetch and extract content from URL",
        "parameters": {"url": "string"},
    },
    {
        "name": "search_docs",
        "description": "Search official documentation",
        "parameters": {"library": "string", "query": "string"},
    },
]
```

**Estimated Effort:** 2-3 days

---

## 5. Vision Support

### Problem
Can't analyze diagrams, screenshots, or UI designs.

### Solution: Vision-Capable Model Integration

**Approach:**
- Add Qwen2-VL or LLaVA model as optional component
- Route image-containing requests to vision model
- Fall back to text-only model when no images

**Use Cases:**
- Screenshot-to-code
- UI bug identification
- Diagram understanding
- Whiteboard transcription

**Configuration:**
```yaml
models:
  vision:
    path: ~/models/gguf/llava-v1.6-mistral-7b.Q4_K_M.gguf
    context_length: 4096
    enabled: true
```

**Estimated Effort:** 3-4 days

---

## 6. Voice Interface

### Problem
Keyboard-only interaction limits accessibility and convenience.

### Solution: Voice Input/Output

**Components:**
- **Whisper.cpp**: Local speech-to-text
- **Piper TTS**: Local text-to-speech
- **Wake word detection**: Optional "Hey Entropi"

**Configuration:**
```yaml
voice:
  enabled: false
  input:
    model: whisper-small
    language: en
  output:
    model: piper-en_US-lessac
    speed: 1.0
```

**Slash Commands:**
```
/voice on     # Enable voice mode
/voice off    # Disable voice mode
/speak        # Read last response aloud
```

**Estimated Effort:** 4-5 days

---

## 7. Plugin System

### Problem
Users want to extend functionality without modifying core.

### Solution: Plugin Architecture

**Plugin Structure:**
```
~/.entropi/plugins/
└── my-plugin/
    ├── plugin.yaml       # Metadata and config
    ├── commands/         # Custom slash commands
    │   └── deploy.md
    ├── tools/            # Custom MCP tools
    │   └── deploy_server.py
    └── prompts/          # Custom prompt templates
        └── review.md
```

**Plugin Manifest:**
```yaml
name: deployment-helper
version: 1.0.0
description: Deployment automation tools
author: user@example.com

requires:
  entropi: ">=0.2.0"

provides:
  commands:
    - deploy
    - rollback
  tools:
    - deploy_server
```

**Estimated Effort:** 5-7 days

---

## 8. Collaborative Features

### Problem
Single-user only, no sharing capabilities.

### Solution: Session Sharing

**Features:**
- Export conversation to shareable format
- Import shared conversations
- Collaborative editing via shared context
- Team knowledge base

**Export Format:**
```json
{
  "version": "1.0",
  "created": "2025-01-15T10:00:00Z",
  "messages": [...],
  "context": {
    "project": "...",
    "files_referenced": [...]
  }
}
```

**Estimated Effort:** 3-4 days

---

## 9. IDE Integration

### Problem
Switching between IDE and terminal is friction.

### Solution: IDE Extensions

**Targets:**
1. **VS Code Extension** (highest priority)
   - Inline chat
   - Code actions
   - Diagnostics integration

2. **Neovim Plugin**
   - Lua-based
   - Telescope integration

3. **JetBrains Plugin**
   - IntelliJ, PyCharm, etc.

**Communication:**
- Local HTTP API in Entropi
- WebSocket for streaming
- Language Server Protocol (LSP) for diagnostics

**Estimated Effort:** 5-10 days per IDE

---

## 10. Learning System

### Problem
Repeating preferences and patterns.

### Solution: Local Learning/Memory

**Components:**
- Extract patterns from successful interactions
- Store user preferences
- Learn project-specific conventions
- Suggest based on history

**Privacy:**
- All data stored locally
- No cloud sync
- User controls what's retained

**Implementation:**
```python
class LearningSystem:
    async def extract_patterns(self, conversation: Conversation) -> list[Pattern]:
        """Extract reusable patterns from successful conversations."""
        pass

    async def suggest_from_history(self, context: Context) -> list[Suggestion]:
        """Suggest based on similar past situations."""
        pass

    async def get_user_preferences(self, category: str) -> dict:
        """Get learned user preferences."""
        pass
```

**Estimated Effort:** 5-7 days

---

## 11. Performance Profiling

### Problem
Hard to identify performance bottlenecks.

### Solution: Built-in Profiling

**Metrics:**
- Token generation speed (t/s)
- Tool execution time
- Context utilization
- VRAM usage over time

**Visualization:**
```
/profile start    # Start profiling
/profile stop     # Stop and show report
/profile export   # Export to JSON/HTML
```

**Report:**
```
═══════════════════════════════════════════════════════════
                  PERFORMANCE REPORT
═══════════════════════════════════════════════════════════

Generation Stats:
  Total tokens: 4,521
  Average speed: 52.3 t/s
  Peak speed: 61.2 t/s

Tool Execution:
  filesystem.read_file: 15 calls, avg 12ms
  bash.execute: 8 calls, avg 245ms
  git.status: 3 calls, avg 8ms

Context Usage:
  Peak: 14,234 / 16,384 tokens (87%)
  Compactions: 0

VRAM:
  Peak: 12.8 GB
  Average: 11.2 GB
```

**Estimated Effort:** 2-3 days

---

## 12. Multi-Language Quality Rules

### Problem
Quality enforcement is Python-focused.

### Solution: Language-Specific Analyzers

**Languages to Support:**
1. JavaScript/TypeScript (high priority)
2. Rust
3. Go
4. Java

**Per-Language Configuration:**
```yaml
quality:
  language_overrides:
    javascript:
      max_cognitive_complexity: 12
      require_jsdoc: true
      prefer_const: true

    rust:
      max_cognitive_complexity: 20
      require_doc_comments: true
      check_clippy_lints: true

    go:
      max_cognitive_complexity: 15
      require_comments_exported: true
      check_go_vet: true
```

**Estimated Effort:** 2-3 days per language

---

## Implementation Roadmap

### Phase 1: High-Impact Quick Wins (Weeks 1-2)
- [ ] Extended context via RAM offloading
- [ ] Performance profiling
- [ ] Web search integration

### Phase 2: Developer Experience (Weeks 3-4)
- [ ] Codebase indexing
- [ ] VS Code extension (basic)
- [ ] Multi-language quality rules (JS/TS)

### Phase 3: Advanced Features (Weeks 5-8)
- [ ] Subagent delegation
- [ ] Plugin system
- [ ] Vision support

### Phase 4: Polish (Ongoing)
- [ ] Voice interface
- [ ] Learning system
- [ ] Collaborative features
- [ ] Additional IDE integrations

---

## Contributing

These features are open for community contribution. To propose implementing a feature:

1. Open an issue discussing the approach
2. Reference this document's specification
3. Submit PR against `develop` branch
4. Include tests and documentation

Priority will be given to PRs that:
- Follow existing code patterns
- Include comprehensive tests
- Have minimal dependencies
- Maintain backward compatibility
