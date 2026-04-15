# Library Consumer Guide

How to embed Entropic as an inference engine in your own application.

## Overview

Entropic is a C library (`librentropic.so`) that your application links against.
A Python wrapper (auto-generated ctypes bindings) is also available. The engine
handles inference, tool calling, delegation, context management, and session
logging — your app provides the config, prompts, and UI.

## Project Structure

```
my-app/
├── default_config.yaml              # Consumer defaults (shipped with app)
├── .myapp/                          # Runtime session directory (gitignored)
│   ├── config.local.yaml            # User-specific overrides
│   ├── session.log                  # Engine logs (per-session)
│   └── session_model.log            # Full user/model exchanges (per-session)
├── prompts/
│   ├── constitution.md              # Shared rules (all tiers)
│   ├── app_context.md               # App-specific context injected into system prompt
│   └── identity_{tier}.md           # Per-tier identity with frontmatter
├── data/
│   └── grammars/
│       └── {tier_name}.gbnf         # Per-tier grammar constraints
└── src/
    └── main.c (or main.cpp, main.py)
```

## Lifecycle

### C API

```c
#include <entropic/entropic.h>

// 1. Create handle
entropic_handle_t h = NULL;
entropic_create(&h);

// 2. Configure (layered: consumer defaults → global → project local → env)
entropic_configure_dir(h, ".myapp");

// 3. Register external tools (optional)
entropic_register_mcp_server(h, "chess",
    "{\"command\":\"python3\",\"args\":[\"chess_server.py\"]}");

// 4. Register grammars (optional, for structured output)
entropic_grammar_register_file(h, "my_grammar", "data/grammars/my_grammar.gbnf");

// 5. Run inference (conversation persists across calls)
entropic_run_streaming(h, "Hello", on_token, NULL, NULL);
entropic_run_streaming(h, "Tell me more", on_token, NULL, NULL);

// 6. Manage conversation
size_t count;
entropic_context_count(h, &count);
entropic_context_clear(h);  // New session

// 7. Clean up
entropic_destroy(h);
```

### Python Wrapper

```python
from entropic import EntropicEngine

engine = EntropicEngine()
engine.configure_dir(".myapp")

engine.run_streaming("Hello", on_token=lambda t: print(t, end=""))
engine.run_streaming("Tell me more", on_token=lambda t: print(t, end=""))

print(engine.context_count())
engine.context_clear()

engine.destroy()
```

## Configuration

### default_config.yaml (Consumer Defaults)

This file ships with your application. It defines your app's baseline config:

```yaml
# App context: injected into system prompt for all tiers
app_context: prompts/app_context.md

models:
  assistant:
    path: primary              # Resolves via bundled model registry
    adapter: qwen35
    context_length: 16384
    identity: false            # Disable bundled identity (app_context is the identity)
  default: assistant

routing:
  enabled: false
  fallback_tier: assistant

permissions:
  auto_approve: true           # Skip tool approval prompts

mcp:
  enable_entropic: true        # Internal tools (delegate, complete, etc.)
  enable_filesystem: false
  enable_bash: false
  enable_git: false
  enable_diagnostics: false

lsp:
  enabled: false
```

### config.local.yaml (User Overrides)

Users place this in the project directory to override defaults:

```yaml
# Override model path
models:
  assistant:
    path: ~/models/gguf/my-preferred-model.gguf
```

### Config Resolution Order

`entropic_configure_dir(".myapp")` loads config in this order:

1. Compiled defaults (struct initializers built into the engine)
2. `default_config.yaml` (CWD — your app's shipped defaults)
3. `~/.entropic/config.yaml` (global user preferences)
4. `.myapp/config.local.yaml` (project-local overrides)
5. `ENTROPIC_*` environment variables

Higher priority layers override lower ones. Fields not present in a layer
retain their value from the previous layer.

## Prompts

### Constitution (prompts/constitution.md)

Shared rules applied to ALL tiers. Defines safety, honesty, and behavioral
boundaries. Optional — disabled with `constitution: false` in config.

```markdown
---
type: constitution
version: 1
---

# My App Rules

- Be helpful and concise
- Never execute destructive commands without confirmation
```

### App Context (prompts/app_context.md)

Consumer-specific context injected into the system prompt. Tells the model
what application it's running in and how to behave.

```markdown
---
type: app_context
version: 1
---

# My App

You are running inside My App, a local code review tool.
You analyze code diffs and suggest improvements.
```

### Identity (prompts/identity_{tier}.md)

Per-tier identity with frontmatter metadata. Defines the model's role,
focus areas, and examples when operating as this tier.

```markdown
---
type: identity
version: 1
name: reviewer
focus:
  - "code review and quality analysis"
---

# Code Reviewer

You review code changes for bugs, style issues, and security concerns.
```

## Multi-Tier Delegation

For applications that need multiple AI personas (e.g., planner + executor):

```yaml
models:
  planner:
    path: primary
    adapter: qwen35
    identity: prompts/identity_planner.md
    grammar: data/grammars/planner.gbnf
    allowed_tools:
      - entropic.delegate

  executor:
    path: lightweight
    adapter: qwen35
    identity: prompts/identity_executor.md
    allowed_tools:
      - my_tool.execute
      - entropic.complete

  default: planner

permissions:
  auto_approve: true

mcp:
  enable_entropic: true
```

The planner analyzes and delegates to the executor. The executor performs
the action and calls `entropic.complete` to signal completion. Control
returns to the parent.

## External MCP Servers

Your app can register custom tool servers that the model calls:

```c
// Register a Python MCP server over stdio
entropic_register_mcp_server(h, "my_tools",
    "{\"command\":\"python3\",\"args\":[\"my_server.py\"]}");
```

The server speaks JSON-RPC 2.0 over stdio. See `examples/pychess/chess_server.py`
for a minimal implementation.

## Session Logging

`entropic_configure_dir()` automatically enables file logging:

- **session.log**: All engine spdlog output — config loading, tier routing,
  generation timing, tool execution, errors. Truncated per session.
- **session_model.log**: Raw user/assistant exchanges streamed in real time.
  `--- USER ---` and `--- ASSISTANT ---` markers separate turns.

Enable llama.cpp/ggml logging (optional, verbose):
```yaml
ggml_logging: true  # Creates llama_ggml.log
```

## Building Your App

### C/C++ (CMake)

```cmake
cmake_minimum_required(VERSION 3.21)
project(my-app C)

add_executable(my-app main.c)

target_include_directories(my-app PRIVATE
    ${ENTROPIC_INCLUDE_DIR}
    ${ENTROPIC_BUILD_INCLUDE_DIR})

target_link_directories(my-app PRIVATE
    ${ENTROPIC_LIB_DIR}
    ${ENTROPIC_DEPS_LIB_DIRS})

target_link_libraries(my-app PRIVATE rentropic rentropic-types)
```

Build against the entropic build tree:

```bash
cmake -B build \
    -DENTROPIC_INCLUDE_DIR=/path/to/entropic/include \
    -DENTROPIC_BUILD_INCLUDE_DIR=/path/to/entropic/build/full/include \
    -DENTROPIC_LIB_DIR=/path/to/entropic/build/full/src/facade \
    -DENTROPIC_DEPS_LIB_DIRS="/path/to/entropic/build/full/src/types;..."
cmake --build build
```

Or use the `inv example` pattern which handles all paths automatically.

### Python

```bash
ENTROPIC_LIB_PATH=/path/to/build/full/src/facade/librentropic.so \
    python my_app.py
```

## Reference Examples

| Example | What It Shows |
|---------|--------------|
| `examples/hello-world/` | Minimal single-tier streaming chat |
| `examples/pychess/` | Multi-tier delegation, grammar constraints, external MCP server |

Both examples ship as C/C++ and Python wrapper versions.
