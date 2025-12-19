# Entropi — Architectural Proposal

> Local AI Coding Assistant powered by Qwen models and llama-cpp-python

**Version:** 1.0.0
**Status:** Approved for Implementation
**Target Platform:** Ubuntu 24.04, NVIDIA RTX PRO 4000 (16GB VRAM), Intel i9

---

## Executive Summary

Entropi is a local, terminal-based AI coding assistant modeled after Claude Code. It runs inference through quantized Qwen models with CUDA acceleration, uses the Model Context Protocol (MCP) for tool integration, and provides a rich terminal interface for interactive coding sessions.

### Key Differentiators
- **Fully Local** — No API costs, no data leaving your machine
- **Multi-Model Architecture** — Intelligent routing between 14B/7B/1.5B/0.5B models
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
│                     │   Model     │    │    MCP      │                     │
│                     │ Orchestrator│    │   Servers   │                     │
│                     └──────┬──────┘    └─────────────┘                     │
│                            │                                                │
│           ┌────────────────┼────────────────┐                              │
│           ▼                ▼                ▼                              │
│    ┌───────────┐    ┌───────────┐    ┌───────────┐                        │
│    │ Primary   │    │   Fast    │    │   Micro   │                        │
│    │   14B     │    │   1.5B    │    │   0.5B    │                        │
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
| **Terminal UI** | User interaction, streaming output, input handling | Rich, Prompt Toolkit |
| **Agentic Loop** | Plan → Act → Observe → Repeat cycle | asyncio |
| **Model Orchestrator** | Model loading, routing, multi-context management | llama-cpp-python |
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

| Role | Model | Quant | Size | VRAM w/ Context |
|------|-------|-------|------|-----------------|
| **Primary** | Qwen2.5-Coder-14B-Instruct | Q4_K_M | ~9 GB | ~10.5 GB (16K ctx) |
| **Workhorse** | Qwen2.5-Coder-7B-Instruct | Q4_K_M | ~4.7 GB | ~6 GB (16K ctx) |
| **Fast** | Qwen2.5-Coder-1.5B-Instruct | Q4_K_M | ~1 GB | ~1.3 GB (4K ctx) |
| **Micro** | Qwen2.5-Coder-0.5B-Instruct | Q8_0 | ~0.5 GB | ~0.6 GB (2K ctx) |

### 2.3 VRAM Budget

```
Configuration: 14B + 1.5B + 0.5B (Default)
┌─────────────────────────────────────────────────────────────┐
│ Qwen2.5-Coder-14B Q4_K_M weights         │  8.5 GB         │
│ Qwen2.5-Coder-1.5B Q4_K_M weights        │  1.0 GB         │
│ Qwen2.5-0.5B Q8_0 weights                │  0.5 GB         │
│ Primary KV cache (16K context)           │  2.0 GB         │
│ Fast/Micro KV caches                     │  0.5 GB         │
│ CUDA overhead                            │  0.5 GB         │
├─────────────────────────────────────────────────────────────┤
│ TOTAL                                    │ ~13.0 GB        │
│ HEADROOM                                 │ ~3.0 GB         │
└─────────────────────────────────────────────────────────────┘
```

### 2.4 Performance Expectations

| Model | Prompt Eval | Generation |
|-------|-------------|------------|
| 14B Q4_K_M | ~1400 t/s | ~45-60 t/s |
| 7B Q4_K_M | ~2500 t/s | ~90-110 t/s |
| 1.5B Q4_K_M | ~5000 t/s | ~250-350 t/s |
| 0.5B Q8_0 | ~8000 t/s | ~400-500 t/s |

---

## 3. Multi-Model Architecture

### 3.1 Model Roles

| Role | Model | Use Cases |
|------|-------|-----------|
| **Primary** | 14B | Complex reasoning, architecture decisions, code review, generation |
| **Workhorse** | 7B | Standard agentic tasks, tool chains (swap-in for speed) |
| **Fast** | 1.5B | Classification, parsing, summarization, drafting |
| **Micro** | 0.5B | Intent detection, routing decisions only |

### 3.2 Task Routing Matrix

| Task | Model | Rationale |
|------|-------|-----------|
| Code generation | Primary (14B) | Quality critical |
| Code review | Primary (14B) | Nuanced analysis |
| Architecture decisions | Primary (14B) | Complex reasoning |
| Multi-file refactoring | Primary (14B) | Context-heavy |
| Tool chain execution | Workhorse (7B) | Speed + quality balance |
| Bug fixing | Workhorse (7B) | Iterative, needs speed |
| Test writing | Workhorse (7B) | Formulaic but contextual |
| Simple Q&A | Fast (1.5B) | Low complexity |
| Commit messages | Fast (1.5B) | Template-ish |
| Tool result summarization | Fast (1.5B) | Compression task |
| Intent classification | Micro (0.5B) | Pure routing |
| Complexity detection | Micro (0.5B) | Binary decision |

### 3.3 Routing Strategy

```python
def route(message: str, context: Context) -> ModelTier:
    # 1. Heuristic rules first (0ms overhead)
    if is_simple_question(message):
        return ModelTier.FAST

    if requires_complex_reasoning(message):
        return ModelTier.PRIMARY

    # 2. Only classify ambiguous cases (~50ms with 0.5B)
    return classify_with_micro_model(message)
```

**Expected Distribution:**
- 40% routed by heuristics (0ms overhead)
- 40% routed to fast model (saves 2-3s per request)
- 20% classified then routed (~50ms overhead)

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

### 5.2 Built-in MCP Servers

| Server | Tools | Description |
|--------|-------|-------------|
| **filesystem** | read_file, write_file, list_directory, search_files, file_exists | File operations |
| **bash** | execute_command | Shell command execution with sandboxing |
| **git** | status, diff, commit, log, branch, checkout | Git operations |

### 5.3 Tool Definition Format

```json
{
  "name": "filesystem.read_file",
  "description": "Read the contents of a file",
  "inputSchema": {
    "type": "object",
    "properties": {
      "path": {
        "type": "string",
        "description": "Path to the file to read"
      }
    },
    "required": ["path"]
  }
}
```

---

## 6. Code Quality Enforcement

### 6.1 Enforcement Flow

```
Model generates code
        │
        ▼
┌───────────────┐
│ Quality Check │
└───────┬───────┘
        │
   Pass?├────Yes────► Accept code
        │
        No
        │
        ▼
┌───────────────┐
│ Generate      │
│ Feedback      │
└───────┬───────┘
        │
        ▼
Append feedback to prompt
        │
        ▼
Regenerate (max 3 attempts)
```

### 6.2 Quality Rules

```python
@dataclass
class CodeQualityRules:
    # Complexity
    max_cognitive_complexity: int = 15
    max_cyclomatic_complexity: int = 10

    # Size
    max_function_lines: int = 50
    max_file_lines: int = 500
    max_parameters: int = 5
    max_returns_per_function: int = 3

    # Structure
    require_type_hints: bool = True
    require_docstrings: bool = True
    require_return_type: bool = True

    # Style
    docstring_style: str = "google"  # or "numpy"
    enforce_snake_case_functions: bool = True
    enforce_pascal_case_classes: bool = True
```

### 6.3 Language Support

Quality rules are configurable per language:

```yaml
# .entropi/quality.yaml
python:
  max_cognitive_complexity: 15
  require_type_hints: true
  docstring_style: google

javascript:
  max_cognitive_complexity: 12
  require_jsdoc: true

rust:
  max_cognitive_complexity: 20
  require_doc_comments: true
```

---

## 7. Storage Design

### 7.1 SQLite Schema

```sql
-- Conversations
CREATE TABLE conversations (
    id TEXT PRIMARY KEY,
    title TEXT,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    project_path TEXT,
    model_id TEXT,
    metadata JSON
);

-- Messages
CREATE TABLE messages (
    id TEXT PRIMARY KEY,
    conversation_id TEXT REFERENCES conversations(id),
    role TEXT CHECK(role IN ('user', 'assistant', 'system', 'tool')),
    content TEXT,
    tool_calls JSON,
    tool_results JSON,
    token_count INTEGER,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    is_compacted BOOLEAN DEFAULT FALSE
);

-- Tool Executions
CREATE TABLE tool_executions (
    id TEXT PRIMARY KEY,
    message_id TEXT REFERENCES messages(id),
    server_name TEXT,
    tool_name TEXT,
    arguments JSON,
    result TEXT,
    duration_ms INTEGER,
    status TEXT CHECK(status IN ('success', 'error', 'timeout')),
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- Full-text search
CREATE VIRTUAL TABLE messages_fts USING fts5(
    content,
    content='messages',
    content_rowid='rowid'
);
```

### 7.2 Data Locations

```
~/.entropi/
├── config.yaml              # Global configuration
├── settings.json            # User preferences
├── history.db               # SQLite database
├── models/                  # Model symlinks or paths
├── prompts/                 # Custom system prompts
│   └── system/
│       └── default.md
└── commands/                # Global slash commands
    └── review.md
```

---

## 8. Configuration System

### 8.1 Hierarchy (Lowest to Highest Priority)

1. **Defaults** — Built into application
2. **Global** — `~/.entropi/config.yaml`
3. **Project** — `.entropi/config.yaml`
4. **Local** — `.entropi/config.local.yaml` (gitignored)
5. **Environment** — `ENTROPI_*` variables
6. **CLI** — Command-line arguments

### 8.2 Configuration Schema

```yaml
# config.yaml
models:
  primary:
    path: ~/models/gguf/Qwen2.5-Coder-14B-Instruct-Q4_K_M.gguf
    context_length: 16384
    gpu_layers: -1
  fast:
    path: ~/models/gguf/qwen2.5-coder-1.5b-instruct-q4_k_m.gguf
    context_length: 4096
    gpu_layers: -1
  micro:
    path: ~/models/gguf/qwen2.5-coder-0.5b-instruct-q8_0.gguf
    context_length: 2048
    gpu_layers: -1

routing:
  enabled: true
  use_heuristics: true
  fallback_model: primary

quality:
  enabled: true
  max_regeneration_attempts: 3
  rules:
    max_cognitive_complexity: 15
    max_returns_per_function: 3
    require_type_hints: true

permissions:
  allow:
    - "filesystem.*"
    - "git.*"
  deny:
    - "bash.execute:rm -rf *"

ui:
  theme: dark
  stream_output: true
  show_token_count: true
```

---

## 9. Command System

### 9.1 Built-in Commands

| Command | Description |
|---------|-------------|
| `/help` | Show available commands |
| `/clear` | Clear conversation history |
| `/compact` | Summarize and compress context |
| `/config` | View/edit configuration |
| `/status` | Show model and system status |
| `/model [name]` | Switch active model |
| `/save [name]` | Save conversation |
| `/load [name]` | Load conversation |

### 9.2 Custom Commands

Location: `.entropi/commands/*.md` or `~/.entropi/commands/*.md`

```markdown
---
name: review
description: Review code for issues
arguments:
  - name: file
    description: File to review
    required: true
---

Review the following file for:
1. Code quality issues
2. Potential bugs
3. Security vulnerabilities
4. Performance concerns

File: $ARGUMENTS

Provide specific line numbers and suggested fixes.
```

### 9.3 ENTROPI.md

Project context file, automatically loaded:

```markdown
# Project Context

## About
FastAPI REST API for user management.

## Structure
- `src/` - Source code
- `tests/` - Test files
- `docs/` - Documentation

## Commands
```bash
pytest tests/ -v      # Run tests
uvicorn main:app     # Start server
```

## Standards
- Python 3.12+
- Type hints required
- Google-style docstrings
```

---

## 10. Terminal UI

### 10.1 Layout

```
╭─ Entropi v0.1.0 ────────────────────────────────────────────╮
│ Model: 14B (16K ctx) │ VRAM: 13.0/16.0 GB │ Tokens: 2,341  │
╰─────────────────────────────────────────────────────────────╯

╭─ Assistant ─────────────────────────────────────────────────╮
│ I'll help you implement the authentication module.         │
│                                                             │
│ First, let me check the existing code structure...         │
│                                                             │
│ ┌─ Tool: filesystem.list_directory ───────────────────────┐│
│ │ path: src/                                              ││
│ │ result: ['main.py', 'auth/', 'models/', 'routes/']     ││
│ └─────────────────────────────────────────────────────────┘│
╰─────────────────────────────────────────────────────────────╯

╭─ Input ─────────────────────────────────────────────────────╮
│ > _                                                         │
╰─────────────────────────────────────────────────────────────╯
```

### 10.2 Key Bindings

| Key | Action |
|-----|--------|
| `Enter` | Send message |
| `Ctrl+C` | Interrupt generation |
| `Ctrl+D` | Exit |
| `Ctrl+L` | Clear screen |
| `Tab` | Autocomplete (files, commands) |
| `↑/↓` | History navigation |

---

## 11. Project Structure

```
entropi/
├── pyproject.toml
├── Dockerfile
├── docker-compose.yaml
├── .pre-commit-config.yaml
├── ENTROPI.md
├── README.md
├── LICENSE
│
├── src/
│   └── entropi/
│       ├── __init__.py
│       ├── __main__.py
│       ├── cli.py                 # CLI entry point
│       ├── app.py                 # Application orchestrator
│       │
│       ├── config/
│       │   ├── __init__.py
│       │   ├── schema.py          # Pydantic models
│       │   └── loader.py          # Config loading/merging
│       │
│       ├── core/
│       │   ├── __init__.py
│       │   ├── engine.py          # Agentic loop
│       │   ├── context.py         # Context management
│       │   ├── router.py          # Model routing
│       │   ├── compaction.py      # Context compaction
│       │   └── commands.py        # Slash command system
│       │
│       ├── inference/
│       │   ├── __init__.py
│       │   ├── backend.py         # Abstract backend interface
│       │   ├── llama_cpp.py       # llama-cpp-python implementation
│       │   ├── orchestrator.py    # Multi-model management
│       │   └── adapters/
│       │       ├── __init__.py
│       │       ├── base.py        # Base adapter
│       │       └── qwen.py        # Qwen-specific adapter
│       │
│       ├── mcp/
│       │   ├── __init__.py
│       │   ├── client.py          # MCP client
│       │   ├── manager.py         # Server lifecycle management
│       │   └── servers/
│       │       ├── __init__.py
│       │       ├── filesystem.py
│       │       ├── bash.py
│       │       └── git.py
│       │
│       ├── quality/
│       │   ├── __init__.py
│       │   ├── enforcer.py        # Quality enforcement coordinator
│       │   ├── rules.py           # Rule definitions
│       │   └── analyzers/
│       │       ├── __init__.py
│       │       ├── base.py
│       │       ├── complexity.py  # Cognitive complexity
│       │       ├── typing.py      # Type hint analysis
│       │       └── docstrings.py  # Docstring analysis
│       │
│       ├── storage/
│       │   ├── __init__.py
│       │   ├── database.py        # SQLite operations
│       │   ├── models.py          # Data models
│       │   └── migrations/
│       │       └── 001_initial.sql
│       │
│       ├── ui/
│       │   ├── __init__.py
│       │   ├── terminal.py        # Main terminal UI
│       │   ├── components.py      # Reusable UI components
│       │   ├── themes.py          # Color themes
│       │   └── keybindings.py     # Key binding definitions
│       │
│       └── prompts/
│           ├── __init__.py
│           ├── loader.py          # Prompt template loading
│           └── templates/
│               ├── system.md
│               ├── tool_format.md
│               └── compaction.md
│
├── tests/
│   ├── conftest.py
│   ├── features/
│   │   ├── basic_chat.feature
│   │   ├── tool_usage.feature
│   │   └── code_generation.feature
│   ├── step_defs/
│   │   └── ...
│   ├── unit/
│   │   └── ...
│   └── integration/
│       └── ...
│
└── docker/
    ├── Dockerfile
    ├── Dockerfile.dev
    └── docker-compose.yaml
```

---

## 12. Testing Strategy

### 12.1 Test Pyramid

```
          ╱╲
         ╱  ╲
        ╱ E2E╲         BDD tests against Docker container
       ╱──────╲
      ╱        ╲
     ╱Integration╲     MCP servers, model loading
    ╱──────────────╲
   ╱                ╲
  ╱      Unit        ╲  Config, routing, quality rules
 ╱────────────────────╲
```

### 12.2 BDD Test Example

```gherkin
Feature: Code Generation
  As a developer
  I want entropi to generate quality code
  So that I can build features faster

  Scenario: Generate a function with quality checks
    Given entropi is running with quality enforcement enabled
    When I request "Create a function to validate email addresses"
    Then the response should contain a Python function
    And the function should have type hints
    And the function should have a docstring
    And the cognitive complexity should be less than 15
```

### 12.3 Chess Game Test Suite

A complete Chess game implementation will serve as the canonical test:
- Known complexity
- Multiple files
- Clear test cases
- Demonstrates full capabilities

---

## 13. Distribution

### 13.1 Docker-Only Release

```dockerfile
FROM nvidia/cuda:12.4-runtime-ubuntu24.04

# Pre-built llama-cpp-python wheel
COPY wheels/ /wheels/
RUN pip install /wheels/*.whl

# Install entropi
COPY . /app
RUN pip install /app

# Models mounted at runtime
VOLUME /models

ENTRYPOINT ["entropi"]
```

### 13.2 Installation

```bash
# Pull and run
docker pull ghcr.io/user/entropi:latest
docker run -it --gpus all \
  -v ~/models:/models \
  -v $(pwd):/workspace \
  ghcr.io/user/entropi:latest
```

---

## 14. Security Considerations

### 14.1 Sandboxing

- Bash commands restricted by permission patterns
- File access limited to project directory by default
- Network access disabled in container by default

### 14.2 Permissions

```yaml
permissions:
  allow:
    - "filesystem.read_file:*"
    - "filesystem.write_file:src/**"
    - "bash.execute:pytest *"
    - "bash.execute:python *"
  deny:
    - "bash.execute:rm -rf *"
    - "filesystem.write_file:/etc/**"
```

---

## 15. Success Criteria

### 15.1 Core Functionality
- [ ] Load and run inference on all 4 model tiers
- [ ] Execute tools via MCP (filesystem, bash, git)
- [ ] Maintain conversation history with SQLite
- [ ] Enforce code quality at generation time
- [ ] Provide rich terminal UI with streaming

### 15.2 User Experience
- [ ] Response time < 100ms for routing decisions
- [ ] Streaming output for all generations
- [ ] Clear error messages and recovery
- [ ] Intuitive slash commands

### 15.3 Self-Maintenance
- [ ] Can create new repositories from scratch
- [ ] Can modify its own codebase
- [ ] Passes its own quality enforcement
- [ ] Can run its own test suite

---

## 16. Risks and Mitigations

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| Model quality insufficient | Medium | High | Model swapping, prompt tuning |
| Tool call parsing failures | High | Medium | Retry logic, fallback to raw output |
| VRAM exhaustion | Low | High | Dynamic context sizing, aggressive compaction |
| MCP server crashes | Medium | Medium | Server health monitoring, auto-restart |

---

## 17. Implementation Phases

| Phase | Focus | Duration |
|-------|-------|----------|
| 1 | Foundation (config, inference, basic UI) | 3-5 days |
| 2 | MCP Client | 2-3 days |
| 3 | MCP Servers (filesystem, bash, git) | 3-4 days |
| 4 | Agentic Loop | 3-4 days |
| 5 | Terminal UI | 2-3 days |
| 6 | Storage | 2 days |
| 7 | Commands & Context | 2-3 days |
| 8 | Quality Enforcement | 2-3 days |
| 9 | Docker & Distribution | 2 days |
| 10 | Testing & Polish | 3-5 days |

**Total: ~25-35 days with Claude Code**

---

## Appendix A: Glossary

| Term | Definition |
|------|------------|
| **MCP** | Model Context Protocol — standard for tool integration |
| **GGUF** | GPT-Generated Unified Format — model file format |
| **KV Cache** | Key-Value cache for transformer attention |
| **Cognitive Complexity** | Measure of code understandability |
| **Agentic Loop** | Autonomous plan-act-observe cycle |

---

## Appendix B: References

- [llama-cpp-python](https://github.com/abetlen/llama-cpp-python)
- [MCP Specification](https://modelcontextprotocol.io/)
- [Qwen2.5-Coder](https://huggingface.co/Qwen)
- [Rich](https://rich.readthedocs.io/)
- [Prompt Toolkit](https://python-prompt-toolkit.readthedocs.io/)
