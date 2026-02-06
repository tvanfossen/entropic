---
version: 1.0.0
type: proposal
schema_version: 1
id: P2-20260204-004
title: "Tool Result Honesty - Prevent False Success Claims"
priority: P2
component: core/engine
author: claude
author_email: noreply@anthropic.com
created: 2026-02-04
updated: 2026-02-04
tags: [reliability, honesty, tool-results, hallucination]
completed_date: null
scoped_files:
  - src/entropi/core/engine.py
  - src/entropi/mcp/servers/filesystem.py
depends_on: []
blocks: []
---

# Tool Result Honesty - Prevent False Success Claims

## Problem Statement

In session 72d15fe0, the model made 6 attempts to write `checkers_game.py`. All were rolled back by diagnostics due to Pyright errors. When the user asked "where exactly did you write those files to?", the model responded:

> "The code was written to the file `checkers_game.py` in your current working directory"

This was **false**. The model either:
1. Lost track of tool results through context
2. Confabulated success from intent rather than outcome

This violates user trust fundamentally.

## Root Cause Analysis

```
┌─────────────────────────────────────────────────────────────┐
│ User: "where did you write those files?"                    │
├─────────────────────────────────────────────────────────────┤
│ Model reasoning (from thinking trace):                      │
│ > "the assistant has been using 'filesystem.write_file'     │
│ > tool, the default path would be the current working       │
│ > directory"                                                │
├─────────────────────────────────────────────────────────────┤
│ FAILURE: Model inferred from INTENT, not RESULT             │
│ - Did not check actual tool results                         │
│ - Did not recall rollback errors                            │
│ - Confabulated success                                      │
└─────────────────────────────────────────────────────────────┘
```

## Proposed Solution

### 1. Tool Result Tracking State

Maintain explicit state tracking for file operations:

```python
class FileOperationTracker:
    """Track file operation outcomes for accurate reporting."""

    def __init__(self):
        self.operations: list[FileOperation] = []

    def record(self, path: str, operation: str, success: bool, error: str | None = None):
        self.operations.append(FileOperation(
            path=path,
            operation=operation,
            success=success,
            error=error,
            timestamp=datetime.now()
        ))

    def get_written_files(self) -> list[str]:
        """Return only files that were SUCCESSFULLY written."""
        return [
            op.path for op in self.operations
            if op.operation == "write" and op.success
        ]

    def get_failed_writes(self) -> list[tuple[str, str]]:
        """Return failed writes with error messages."""
        return [
            (op.path, op.error) for op in self.operations
            if op.operation == "write" and not op.success
        ]
```

### 2. Inject State into System Prompt on File Questions

When user asks about files (detected via keywords: "where", "file", "wrote", "created"), inject current state:

```
[FILE OPERATION STATUS]
Successfully written files: (none)
Failed writes:
  - checkers_game.py: rolled back (5 Pyright errors)
  - checkers_game.py: rolled back (1 Pyright error)
  - checkers_game.py: duplicate call blocked
```

### 3. Model Instruction Update

Add to system prompt:

```
When reporting on file operations:
- ONLY claim files exist if write operations succeeded
- If writes were rolled back, explicitly state this
- When uncertain, use filesystem tools to verify before claiming
```

## Acceptance Criteria

- [ ] File operation tracker records success/failure for all writes
- [ ] Failed writes are clearly marked in tracker state
- [ ] Model can query tracker before making claims about files
- [ ] System prompt injection for file-related questions
- [ ] Model never claims file success when tracker shows failure

## Implementation Plan

### Phase 1: Operation Tracker
Add FileOperationTracker to engine state.

### Phase 2: MCP Server Integration
Filesystem server reports success/failure to tracker after each operation.

### Phase 3: Query Detection
Detect file-related questions and inject tracker state.

### Phase 4: Prompt Update
Add instruction about checking tracker before file claims.

## Risks & Considerations

- Tracker state grows with long sessions (need pruning strategy)
- Context injection adds tokens
- Model might still ignore injected state (needs testing)

## Test Cases

```python
def test_failed_write_not_claimed_as_success():
    """Model should not claim file exists after rollback."""
    # Setup: Attempt write that triggers diagnostics failure
    # Action: Ask "where is the file?"
    # Assert: Response mentions failure, not success

def test_successful_write_correctly_reported():
    """Model should accurately report successful writes."""
    # Setup: Write file that passes diagnostics
    # Action: Ask "where is the file?"
    # Assert: Response includes correct path
```

## Implementation Log

{Entries added as work progresses}

## References

- Session: 72d15fe0-2192-4bba-9e8b-a3fadb509296
- Failure point: 20:42:11 - model claims success after 6 rollbacks
