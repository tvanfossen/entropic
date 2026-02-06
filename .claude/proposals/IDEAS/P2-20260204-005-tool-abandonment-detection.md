---
version: 1.0.0
type: proposal
schema_version: 1
id: P2-20260204-005
title: "Tool Abandonment Detection and Recovery"
priority: P2
component: core/engine
author: claude
author_email: noreply@anthropic.com
created: 2026-02-04
updated: 2026-02-04
tags: [reliability, tool-usage, error-recovery, code-generation]
completed_date: null
scoped_files:
  - src/entropi/core/engine.py
  - src/entropi/prompts/
depends_on: []
blocks: []
---

# Tool Abandonment Detection and Recovery

## Problem Statement

In session 72d15fe0, after 6 failed `filesystem.write_file` attempts, the model **abandoned tool usage entirely** and began outputting code in markdown fences:

```
[Turn 1-6]: Uses filesystem.write_file -> all rolled back
[Turn 7+]:  Outputs code as ```python blocks -> no file created
            User says "Proceed" -> more markdown code
            User says "Proceed" -> more markdown code
            User asks "where are the files?" -> files don't exist
```

The model switched from "agent doing work" to "assistant showing code" without informing the user or attempting recovery.

## Failure Pattern

```
┌─────────────────────────────────────────────────────────────┐
│ TOOL ABANDONMENT PATTERN                                    │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  Write attempt 1 ──► Rollback (errors)                      │
│  Write attempt 2 ──► Rollback (same errors)                 │
│  Write attempt 3 ──► Rollback (same errors)                 │
│  Write attempt 4 ──► Rollback (same errors)                 │
│  Write attempt 5 ──► Rollback (same errors)                 │
│  Write attempt 6 ──► Duplicate blocked                      │
│         │                                                   │
│         ▼                                                   │
│  Model gives up, outputs markdown                           │
│  User thinks files are being created                        │
│  Files never exist                                          │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

## Proposed Solution

### 1. Detect Tool Abandonment

Track when model stops using tools for tasks that require them:

```python
class ToolUsageMonitor:
    """Detect when model abandons tools inappropriately."""

    def __init__(self):
        self.recent_tool_failures: dict[str, list[ToolFailure]] = {}
        self.code_block_without_tool: int = 0

    def record_failure(self, tool: str, error: str):
        if tool not in self.recent_tool_failures:
            self.recent_tool_failures[tool] = []
        self.recent_tool_failures[tool].append(ToolFailure(error, datetime.now()))

    def record_code_output(self, language: str):
        """Called when model outputs code in markdown instead of using tool."""
        if language in ("python", "javascript", "typescript"):
            self.code_block_without_tool += 1

    def check_abandonment(self) -> AbandonmentStatus | None:
        """Detect if model has abandoned tools for prose."""
        # Pattern: Multiple failures followed by code blocks
        for tool, failures in self.recent_tool_failures.items():
            if len(failures) >= 3 and self.code_block_without_tool >= 1:
                return AbandonmentStatus(
                    tool=tool,
                    failure_count=len(failures),
                    last_error=failures[-1].error
                )
        return None
```

### 2. Intervene on Abandonment

When abandonment detected, inject guidance:

```
[TOOL USAGE ALERT]
You attempted filesystem.write_file 6 times - all failed.
You then output code in markdown instead of using tools.

This is problematic because:
- User expects files to be created, not shown
- Code in markdown doesn't persist

Options:
1. Diagnose the failure (missing dependency? syntax error?)
2. Use bash.execute to check environment
3. Ask user for help resolving the blocker
4. Explicitly tell user you cannot complete the task and why

Do NOT continue outputting code without addressing the tool failure.
```

### 3. Require Explicit Acknowledgment

If tool fails 3+ times, require model to explicitly state one of:
- "I'm switching to showing code because [reason]"
- "I need to diagnose why writes are failing"
- "I cannot complete this task because [blocker]"

## Acceptance Criteria

- [ ] System tracks consecutive tool failures by type
- [ ] System detects code blocks output after tool failures
- [ ] Abandonment triggers intervention message
- [ ] Model must acknowledge abandonment explicitly
- [ ] User is informed when files won't be created

## Implementation Plan

### Phase 1: Failure Tracking
Add ToolUsageMonitor to engine.

### Phase 2: Code Block Detection
Parse model output for code fences, track when they appear without tool calls.

### Phase 3: Intervention Injection
When abandonment detected, inject guidance before next model turn.

### Phase 4: Acknowledgment Requirement
Validate model response addresses the abandonment.

## Alternative Approaches Considered

| Approach | Pros | Cons |
|----------|------|------|
| Hard block markdown code | Forces tool usage | Too restrictive |
| Auto-retry with fixes | Might succeed | Could loop forever |
| **Soft intervention** | Guides without blocking | Model might ignore |

Chose soft intervention as it preserves flexibility while providing guardrails.

## Test Cases

```python
def test_abandonment_detected_after_failures():
    """Abandonment should be flagged when tools fail then code output."""
    monitor = ToolUsageMonitor()
    for _ in range(5):
        monitor.record_failure("filesystem.write_file", "diagnostics_failed")
    monitor.record_code_output("python")

    status = monitor.check_abandonment()
    assert status is not None
    assert status.tool == "filesystem.write_file"

def test_intervention_injected():
    """Abandonment should trigger intervention in next turn."""
    # Setup: Simulate abandonment pattern
    # Assert: Next system message includes intervention text
```

## Implementation Log

{Entries added as work progresses}

## References

- Session: 72d15fe0-2192-4bba-9e8b-a3fadb509296
- Abandonment started: Turn 7 (20:16:56)
- User confusion: Turn 12 (20:41:33)
