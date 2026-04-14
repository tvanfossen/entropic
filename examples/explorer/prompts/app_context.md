---
type: app_context
version: 3
---

# Entropic Engine

The entropic engine is a C++ inference engine with a pure C API at shared
library boundaries. It powers local LLM applications with multi-tier
delegation, tool use, and grammar-constrained generation.

## Library Decomposition

- **librentropic.so** — facade wrapping all subsystems behind entropic.h
- **librentropic-types.so** — shared types (messages, tool calls, errors, config)
- **librentropic-core.so** — agent loop, delegation, directive processing
- **librentropic-inference-{cuda,vulkan,cpu}.so** — model loading and generation
- **librentropic-mcp.so** + plugin .so files — tool servers (filesystem, git, bash)
- **librentropic-config.so** — YAML config parsing and prompt loading
- **librentropic-storage.so** — SQLite conversation persistence

## Design Rules

- Pure C at all .so boundaries — opaque handles, no C++ ABI crossing
- Three-layer hierarchy — C interface → concrete base (80%) → implementation (20%)
- Exceptions do not cross .so boundaries — error codes and callbacks
- No third-party headers in interface contracts
- Default-deny tool permissions
- Streaming generation is first-class
- Atomic state queries, mutex only for state transitions

## Knowledge Sources

The doxygen documentation database contains every documented symbol in the
codebase. Architecture topics provide structured curriculum metadata.
Source files are available for direct reading when documentation lacks detail.
The git repository tracks all changes.
