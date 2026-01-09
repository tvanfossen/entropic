# Entropi — Architectural Proposal

> Local AI Coding Assistant powered by Qwen models and llama-cpp-python

**Version:** 1.1.0  
**Status:** Approved for Implementation  
**Target Platform:** Ubuntu 24.04, NVIDIA RTX PRO 4000 (16GB VRAM), Intel i9

---

## Executive Summary

Entropi is a local, terminal-based AI coding assistant modeled after Claude Code. It runs inference through quantized Qwen models with CUDA acceleration, uses the Model Context Protocol (MCP) for tool integration, and provides a rich terminal interface for interactive coding sessions.

### Key Differentiators
- **Fully Local** — No API costs, no data leaving your machine
- **Task-Specialized Routing** — Qwen3 for reasoning, Qwen2.5-Coder for code generation
- **Thinking Mode** — Toggle between fast (8B) and deep reasoning (14B)
- **Code Quality Enforcement** — Cognitive complexity, typing, docstrings enforced at generation time
- **MCP-First** — All tools via standard MCP protocol
- **Self-Maintaining** — Can maintain its own repository

---

## 1. System Architecture

### 1.1 High-Level Overview

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              ENTROPI                                        │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌─────────────┐    ┌─────────────┐    ┌─────────────┐    ┌─────────────┐  │
│  │   Terminal  │    │   Agentic   │    │    MCP      │    │   Storage   │  │
│  │     UI      │◄──►│    Loop     │◄──►│   Client    │◄──►│   (SQLite)  │  │
│  └─────────────┘    └──────┬──────┘    └──────┬──────┘    └─────────────┘  │
│                            │                  │                             │
│                            ▼                  ▼                             │
│                     ┌─────────────┐    ┌─────────────┐                     │
│                     │Task-Aware   │    │    MCP      │                     │
│                     │  Router     │    │   Servers   │                     │
│                     └──────┬──────┘    └─────────────┘                     │
│                            │                                                │
│           ┌────────────────┼────────────────┐                              │
│           ▼                ▼                ▼                              │
│    ┌───────────┐    ┌───────────┐    ┌───────────┐                        │
│    │  Qwen3    │    │Qwen2.5    │    │  Micro    │                        │
│    │ 14B / 8B  │    │ Coder-7B  │    │   0.5B    │                        │
│    │(Reasoning)│    │  (Code)   │    │ (Router)  │                        │
│    └───────────┘    └───────────┘    └───────────┘                        │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
                    ┌───────────────────────────────┐
                    │       llama-cpp-python        │
                    │         (CUDA Backend)        │
                    └───────────────────────────────┘
```

### 1.2 Component Overview

| Component | Responsibility | Key Dependencies |
|-----------|---------------|------------------|
| **Terminal UI** | User interaction, streaming output, thinking toggle | Rich, Prompt Toolkit |
| **Agentic Loop** | Plan → Act → Observe → Repeat cycle | asyncio |
| **Task-Aware Router** | Route to Qwen3 (reasoning) or Qwen2.5-Coder (code) | llama-cpp-python |
| **MCP Client** | Tool discovery and execution via MCP protocol | mcp (official SDK) |
| **MCP Servers** | Filesystem, bash, git tools | mcp |
| **Storage** | Conversation history, tool logs, configuration | aiosqlite |
| **Quality Enforcer** | Code validation at generation time | AST, custom analyzers |
| **Command System** | Slash commands, ENTROPI.md loading | Click |

---

## 2. Hardware Specifications

### 2.1 Target Hardware

| Component | Specification |
|-----------|---------------|
| **Platform** | Lenovo ThinkPad P16 |
| **OS** | Ubuntu 24.04 LTS |
| **CPU** | Intel Core i9 (mobile) |
| **GPU** | NVIDIA RTX PRO 4000 Ada |
| **GPU VRAM** | 16 GB |
| **System RAM** | 64 GB |

### 2.2 Model Configuration

| Role | Model | Quant | Size | Purpose |
|------|-------|-------|------|---------|
| **Thinking** | Qwen3-14B | Q4_K_M | ~8.5 GB | Deep reasoning (thinking mode ON) |
| **Normal** | Qwen3-8B | Q4_K_M | ~4.5 GB | General reasoning (thinking mode OFF) |
| **Code** | Qwen2.5-Coder-7B-Instruct | Q4_K_M | ~4.0 GB | All code generation |
| **Micro** | Qwen2.5-Coder-0.5B-Instruct | Q8_0 | ~0.5 GB | Routing, always loaded |

### 2.3 VRAM Budget

#### Thinking OFF (Normal Mode)

```
┌─────────────────────────────────────────────────────────────┐
│ Qwen3-8B Q4_K_M weights                 │  4.5 GB          │
│ Qwen2.5-Coder-7B Q4_K_M weights          │  4.0 GB          │
│ Qwen2.5-Coder-0.5B Q8_0 weights          │  0.5 GB          │
│ KV caches (8B: 16K, 7B: 8K)              │  2.0 GB          │
│ CUDA overhead                            │  0.5 GB          │
├─────────────────────────────────────────────────────────────┤
│ TOTAL                                    │ ~11.5 GB         │
│ HEADROOM                                 │ ~4.5 GB          │
└─────────────────────────────────────────────────────────────┘
```

#### Thinking ON (Deep Reasoning Mode)

```
┌─────────────────────────────────────────────────────────────┐
│ Qwen3-14B Q4_K_M weights                 │  8.5 GB          │
│ Qwen2.5-Coder-7B Q4_K_M weights          │  4.0 GB          │
│ Qwen2.5-Coder-0.5B Q8_0 weights          │  0.5 GB          │
│ KV caches (14B: 16K, 7B: 8K)             │  2.5 GB          │
│ CUDA overhead                            │  0.5 GB          │
├─────────────────────────────────────────────────────────────┤
│ TOTAL                                    │ ~16.0 GB         │
│ HEADROOM                                 │ ~0 GB            │
└─────────────────────────────────────────────────────────────┘

Note: May require model swapping instead of co-loading in tight scenarios.
```

### 2.4 Performance Expectations

| Model | Prompt Eval | Generation |
|-------|-------------|------------|
| Qwen3-14B Q4_K_M | ~1200 t/s | ~40-55 t/s |
| Qwen3-8B Q4_K_M | ~2200 t/s | ~75-95 t/s |
| Qwen2.5-Coder-7B Q4_K_M | ~2500 t/s | ~90-110 t/s |
| Qwen2.5-Coder-0.5B Q8_0 | ~8000 t/s | ~400-500 t/s |

---

## 3. Task-Specialized Model Architecture

### 3.1 Core Principle

**Different models excel at different tasks:**

| Task Type | Best Model Family | Why |
|-----------|-------------------|-----|
| **Reasoning & Planning** | Qwen3 | Newer architecture, better instruction following |
| **Code Generation** | Qwen2.5-Coder | Specialized on 5.5T code tokens, 88%+ HumanEval |

### 3.2 Model Roles

| Role | Model | When Used |
|------|-------|-----------|
| **Thinking** | Qwen3-14B | Thinking mode ON — all reasoning tasks |
| **Normal** | Qwen3-8B | Thinking mode OFF — general reasoning |
| **Code** | Qwen2.5-Coder-7B | ANY code generation (regardless of thinking mode) |
| **Micro** | Qwen2.5-Coder-0.5B | Always loaded — routing decisions |

### 3.3 Task Routing Flow

```
User message
     │
     ▼
┌─────────────────┐
│  0.5B Router    │ ◄── Always loaded, code-aware
└────────┬────────┘
         │
         ├─── Is this a code generation task?
         │           │
         │           ├── YES ──► Qwen2.5-Coder-7B
         │           │
         │           └── NO ───► Qwen3 (8B or 14B based on thinking mode)
         │
         └─── Thinking mode?
                     │
                     ├── ON ───► Qwen3-14B (deep reasoning)
                     │
                     └── OFF ──► Qwen3-8B (fast reasoning)
```

### 3.4 Task Classification

| Task | Routes To | Rationale |
|------|-----------|-----------|
| "Write a function that..." | Qwen2.5-Coder-7B | Code generation |
| "Fix this bug..." | Qwen2.5-Coder-7B | Code modification |
| "Refactor this class..." | Qwen2.5-Coder-7B | Code transformation |
| "Add tests for..." | Qwen2.5-Coder-7B | Code generation |
| "Plan how to implement..." | Qwen3 (8B/14B) | Reasoning/planning |
| "What tools should I use?" | Qwen3 (8B/14B) | Tool selection |
| "Explain this error..." | Qwen3 (8B/14B) | Explanation |
| "Review this approach..." | Qwen3 (8B/14B) | Analysis |

### 3.5 Thinking Mode Toggle

```
/think on       Enable deep reasoning (loads Qwen3-14B)
/think off      Disable deep reasoning (loads Qwen3-8B)
/think status   Show current mode
Ctrl+T          Toggle thinking mode
```

**Status Bar Indicator:**
```
Normal mode:
╭─ Entropi ─────────────────────────────────────────╮
│ Mode: Normal │ VRAM: 11.5/16 GB │ ⚡ Fast        │
╰───────────────────────────────────────────────────╯

Thinking mode:
╭─ Entropi ─────────────────────────────────────────╮
│ Mode: 🧠 Thinking │ VRAM: 16/16 GB │ Deep        │
╰───────────────────────────────────────────────────╯
```

---

## 4. Agentic Loop Design

### 4.1 Core Loop

```
┌─────────────────────────────────────────────────────────────┐
│                      AGENTIC LOOP                           │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│   ┌─────────┐     ┌─────────┐     ┌─────────┐             │
│   │ PLANNING│────►│EXECUTING│────►│VERIFYING│             │
│   └────┬────┘     └────┬────┘     └────┬────┘             │
│        │               │               │                   │
│        │               ▼               │                   │
│        │         ┌─────────┐          │                   │
│        │         │  TOOLS  │          │                   │
│        │         └─────────┘          │                   │
│        │                              │                   │
│        └──────────────────────────────┘                   │
│                    (loop on failure)                       │
│                                                             │
│   Terminal States: COMPLETE, ERROR, USER_INTERRUPT         │
└─────────────────────────────────────────────────────────────┘
```

### 4.2 Loop Configuration

```python
@dataclass
class AgentLoopConfig:
    max_iterations: int = 15
    max_consecutive_errors: int = 3
    max_tool_calls_per_turn: int = 10
    idle_timeout_seconds: int = 300
    require_plan_for_complex: bool = True
    stream_output: bool = True
```

### 4.3 Tool Execution Flow

```
1. Model generates response (streaming)
2. Parse for tool calls (JSON or XML format)
3. For each tool call:
   a. Validate against permissions
   b. Execute via MCP client
   c. Collect result
4. Inject results into context
5. If more tool calls needed → goto 1
6. If stop condition → display final response
```

### 4.4 Code Generation Detection

During the agentic loop, the router continuously monitors for code generation requests:

```python
async def process_turn(self, message: str) -> AsyncIterator[str]:
    # Classify the task
    task_type = await self.router.classify(message)
    
    if task_type == TaskType.CODE_GENERATION:
        # Always use specialized coder model
        model = self.coder_model  # Qwen2.5-Coder-7B
    else:
        # Use reasoning model based on thinking mode
        model = self.thinking_model if self.thinking_enabled else self.normal_model
    
    async for chunk in model.generate_stream(message):
        yield chunk
```

---

## 5. MCP Integration

### 5.1 Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                      MCP CLIENT                             │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐        │
│  │  Filesystem │  │    Bash     │  │     Git     │        │
│  │   Server    │  │   Server    │  │   Server    │        │
│  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘        │
│         │                │                │                │
│         └────────────────┼────────────────┘                │
│                          │                                 │
│                          ▼                                 │
│                   ┌─────────────┐                          │
│                   │  Server     │                          │
│                   │  Manager    │                          │
│                   └─────────────┘                          │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### 5.2 Built-in Tools

| Server | Tools |
|--------|-------|
| **Filesystem** | read_file, write_file, list_directory, search_files, file_exists |
| **Bash** | execute (with safety checks) |
| **Git** | status, diff, log, commit, branch, checkout |

### 5.3 Permission System

```yaml
permissions:
  allow:
    - "filesystem.*"
    - "git.status"
    - "git.diff"
    - "git.log"
  deny:
    - "bash.execute:rm -rf *"
    - "bash.execute:sudo *"
  require_confirmation:
    - "git.commit"
    - "filesystem.write_file:*"
```

---

## 6. Code Quality Enforcement

### 6.1 Pre-Generation Validation

Before any code is written to disk:

| Check | Threshold | Action |
|-------|-----------|--------|
| Cognitive complexity | ≤15 per function | Reject + feedback |
| Cyclomatic complexity | ≤10 per function | Warn |
| Type hints | Required on public API | Reject + feedback |
| Docstrings | Required on public API | Reject + feedback |
| Max function length | ≤50 lines | Warn |
| Max returns per function | ≤4 | Reject + feedback |

### 6.2 Regeneration Loop

```python
async def enforce_quality(code: str) -> str:
    for attempt in range(max_attempts):
        report = analyzer.analyze(code)
        
        if report.passed:
            return code
        
        feedback = report.format_feedback()
        code = await model.regenerate(code, feedback)
    
    raise QualityError("Max regeneration attempts exceeded")
```

---

## 7. Storage Layer

### 7.1 SQLite Schema

```sql
-- Conversations
CREATE TABLE conversations (
    id TEXT PRIMARY KEY,
    title TEXT,
    created_at TIMESTAMP,
    updated_at TIMESTAMP,
    project_path TEXT,
    metadata JSON
);

-- Messages
CREATE TABLE messages (
    id TEXT PRIMARY KEY,
    conversation_id TEXT REFERENCES conversations(id),
    role TEXT,  -- 'user', 'assistant', 'system', 'tool'
    content TEXT,
    token_count INTEGER,
    model_used TEXT,
    created_at TIMESTAMP
);

-- Tool executions
CREATE TABLE tool_executions (
    id TEXT PRIMARY KEY,
    message_id TEXT REFERENCES messages(id),
    tool_name TEXT,
    arguments JSON,
    result TEXT,
    duration_ms INTEGER,
    success BOOLEAN
);
```

### 7.2 Full-Text Search

```sql
CREATE VIRTUAL TABLE messages_fts USING fts5(content, conversation_id);
```

---

## 8. Configuration System

### 8.1 Hierarchy

```
1. Defaults (code)
2. Global (~/.entropi/config.yaml)
3. Project (.entropi/config.yaml)
4. Local (.entropi/config.local.yaml)
5. Environment (ENTROPI_*)
6. CLI arguments
```

### 8.2 Core Configuration

```yaml
# ~/.entropi/config.yaml
models:
  thinking:
    path: ~/models/gguf/qwen3-14b-q4_k_m.gguf
    context_length: 16384
    
  normal:
    path: ~/models/gguf/qwen3-8b-q4_k_m.gguf
    context_length: 16384
    
  code:
    path: ~/models/gguf/Qwen2.5-Coder-7B-Instruct-Q4_K_M.gguf
    context_length: 16384
    
  micro:
    path: ~/models/gguf/qwen2.5-coder-0.5b-instruct-q8_0.gguf
    context_length: 2048

thinking:
  default: false
  auto_enable_keywords:
    - "architect"
    - "design"
    - "think through"
    - "complex"
  swap_timeout_seconds: 10

routing:
  code_detection_keywords:
    - "write"
    - "implement"
    - "create function"
    - "fix bug"
    - "refactor"
    - "add test"

quality:
  enabled: true
  max_cognitive_complexity: 15
  max_cyclomatic_complexity: 10
  require_type_hints: true
  require_docstrings: true

permissions:
  allow:
    - "filesystem.*"
    - "git.*"
    - "bash.execute"
  deny:
    - "bash.execute:rm -rf *"

ui:
  theme: dark
  stream_output: true
  show_token_counts: true
  show_thinking_indicator: true
```

---

## 9. Project Structure

```
entropi/
├── src/
│   └── entropi/
│       ├── __init__.py
│       ├── __main__.py
│       ├── cli.py              # CLI entry point
│       ├── app.py              # Application orchestrator
│       │
│       ├── config/
│       │   ├── __init__.py
│       │   ├── schema.py       # Pydantic models
│       │   └── loader.py       # Configuration loading
│       │
│       ├── core/
│       │   ├── __init__.py
│       │   ├── base.py         # Abstract base classes
│       │   ├── state.py        # State machine
│       │   ├── engine.py       # Agentic loop
│       │   ├── context.py      # Context management
│       │   ├── commands.py     # Slash commands
│       │   └── logging.py      # Logging setup
│       │
│       ├── inference/
│       │   ├── __init__.py
│       │   ├── backend.py      # Abstract interface
│       │   ├── llama_cpp.py    # llama-cpp-python wrapper
│       │   ├── router.py       # Task-aware routing
│       │   ├── orchestrator.py # Multi-model management
│       │   └── adapters/
│       │       ├── __init__.py
│       │       ├── base.py     # Adapter interface
│       │       └── qwen.py     # Qwen chat template
│       │
│       ├── mcp/
│       │   ├── __init__.py
│       │   ├── client.py       # MCP client
│       │   ├── manager.py      # Server management
│       │   └── servers/
│       │       ├── __init__.py
│       │       ├── base.py
│       │       ├── filesystem.py
│       │       ├── bash.py
│       │       └── git.py
│       │
│       ├── quality/
│       │   ├── __init__.py
│       │   ├── enforcer.py     # Quality coordinator
│       │   └── analyzers/
│       │       ├── __init__.py
│       │       ├── base.py
│       │       ├── complexity.py
│       │       ├── typing.py
│       │       ├── docstrings.py
│       │       └── structure.py
│       │
│       ├── storage/
│       │   ├── __init__.py
│       │   ├── backend.py      # Storage interface
│       │   └── sqlite.py       # SQLite implementation
│       │
│       ├── ui/
│       │   ├── __init__.py
│       │   ├── terminal.py     # Rich terminal UI
│       │   ├── themes.py       # Color themes
│       │   └── components.py   # UI components
│       │
│       └── prompts/
│           ├── system.md
│           └── templates/
│
├── tests/
│   ├── unit/
│   ├── integration/
│   └── bdd/
│
├── docker/
│   ├── Dockerfile
│   └── Dockerfile.dev
│
├── pyproject.toml
├── .pre-commit-config.yaml
└── README.md
```

---

## 10. Testing Strategy

### 10.1 Canonical Test: Chess Game

To validate the complete system, use Entropi to create a Chess game:

```bash
mkdir chess-game && cd chess-game
entropi init
entropi

> Create a complete Chess game in Python with:
> - Board representation
> - Move validation
> - Check/checkmate detection
> - Simple ASCII display
> - Game loop for two players
```

This exercises:
- Task routing (planning → Qwen3, code → Qwen2.5-Coder)
- Multi-file generation
- Quality enforcement
- Tool chains (filesystem, bash for tests)
- Context management

### 10.2 Test Layers

| Layer | Framework | Focus |
|-------|-----------|-------|
| Unit | pytest | Individual components |
| Integration | pytest | Component interaction |
| BDD | pytest-bdd | User scenarios |
| E2E | Manual + script | Full workflows |

---

## 11. Implementation Timeline

| Phase | Focus | Duration |
|-------|-------|----------|
| 01 | Foundation (config, CLI, base classes) | 3-5 hours |
| 02 | Inference Engine (models, task-aware routing) | 4-6 hours |
| 03 | MCP Client | 2-3 hours |
| 04 | MCP Servers | 3-4 hours |
| 05 | Agentic Loop | 3-4 hours |
| 06 | Terminal UI + Thinking Toggle | 2-3 hours |
| 07 | Storage | 2-3 hours |
| 08 | Commands & Context | 2-3 hours |
| 09 | Quality Enforcement | 2-3 hours |
| 10 | Docker & Distribution | 2-3 hours |

**Total: ~26-37 hours**

---

## 12. Model Downloads

```bash
# Qwen3-14B (Thinking mode)
huggingface-cli download Qwen/Qwen3-14B-GGUF \
  qwen3-14b-q4_k_m.gguf --local-dir ~/models/gguf

# Qwen3-8B (Normal mode)
huggingface-cli download Qwen/Qwen3-8B-GGUF \
  qwen3-8b-q4_k_m.gguf --local-dir ~/models/gguf

# Qwen2.5-Coder-7B (All code generation)
huggingface-cli download bartowski/Qwen2.5-Coder-7B-Instruct-GGUF \
  --include "Qwen2.5-Coder-7B-Instruct-Q4_K_M.gguf" \
  --local-dir ~/models/gguf

# Qwen2.5-Coder-0.5B (Router - always loaded)
huggingface-cli download Qwen/Qwen2.5-Coder-0.5B-Instruct-GGUF \
  qwen2.5-coder-0.5b-instruct-q8_0.gguf --local-dir ~/models/gguf
```

**Total Download: ~18 GB (4 models)**

---

## Appendix A: Why Task-Specialized Models?

### Benchmark Comparison

| Benchmark | Qwen2.5-Coder-7B | Qwen3-8B |
|-----------|------------------|----------|
| **HumanEval** | **88.4%** | ~75-78% |
| **MBPP** | **83%+** | ~70% |
| Training data | 5.5T code tokens | General |
| Specialization | Code-focused | General reasoning |

**Conclusion**: For code generation, the specialized Qwen2.5-Coder-7B significantly outperforms the general-purpose Qwen3-8B despite similar size. Using specialized models for their intended tasks yields better results.

### Qwen3 Advantages

| Capability | Qwen3 Strength |
|------------|----------------|
| Instruction following | Better than Qwen2.5 |
| Planning | Better reasoning |
| Tool selection | Better understanding |
| Explanation | Clearer output |

**Conclusion**: For reasoning, planning, and tool use, Qwen3's newer architecture provides better results.
