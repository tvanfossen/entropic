---
version: 1.0.0
type: proposal
schema_version: 1
id: P2-20260204-008
title: "Duplicate Tool Call Learning and Prevention"
priority: P2
component: core/engine
author: claude
author_email: noreply@anthropic.com
created: 2026-02-04
updated: 2026-02-04
tags: [tool-usage, error-recovery, learning, efficiency]
completed_date: null
scoped_files:
  - src/entropi/core/engine.py
depends_on: []
blocks: []
---

# Duplicate Tool Call Learning and Prevention

## Problem Statement

In session 72d15fe0, the system correctly detected duplicate tool calls:

```
2026-02-04 20:16:12 [WARNING] entropi.core.engine: Duplicate tool call #1: filesystem.write_file
2026-02-04 20:16:12 [INFO] entropi.core.engine: [FEEDBACK] Duplicate message sent: Tool `filesystem.write_file` was already called with the same arguments.
```

Despite receiving this feedback, the model:
1. Did not adjust its approach
2. Continued with the same strategy
3. Never addressed WHY it was repeating itself

The duplicate detection works, but it doesn't change model behavior.

## Current Behavior

```
┌─────────────────────────────────────────────────────────────┐
│ DUPLICATE DETECTION FLOW (current)                          │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  Model: filesystem.write_file(path="x.py", content="...")   │
│                   │                                         │
│                   ▼                                         │
│  Engine: Detects duplicate                                  │
│  Engine: Sends feedback "Duplicate message sent..."         │
│                   │                                         │
│                   ▼                                         │
│  Model: Continues without acknowledging                     │
│  Model: May try same call again later                       │
│                                                             │
│  PROBLEM: No learning occurs                                │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

## Proposed Solution

### 1. Enhanced Duplicate Feedback

Provide actionable feedback, not just notification:

```python
def format_duplicate_feedback(self, tool: str, args: dict, previous_result: str) -> str:
    """Create actionable feedback for duplicate calls."""

    feedback = f"""[DUPLICATE TOOL CALL BLOCKED]
Tool: {tool}
Arguments: {json.dumps(args, indent=2)[:500]}

Previous result for this exact call:
{previous_result[:1000]}

This call was blocked because:
- Repeating the same call will produce the same result
- The previous attempt already failed/succeeded

To make progress, you must CHANGE your approach:
1. If previous call failed: Fix the underlying issue first
2. If previous call succeeded: Move to the next step
3. If stuck: Ask the user for help or try an alternative

What will you do differently?
"""
    return feedback
```

### 2. Require Acknowledgment

After duplicate feedback, require model to state what it will do differently:

```python
class DuplicateHandler:
    """Handle duplicate tool calls with required acknowledgment."""

    def __init__(self):
        self.pending_acknowledgment = False
        self.blocked_calls: list[BlockedCall] = []

    def handle_duplicate(self, tool: str, args: dict, prev_result: str) -> str:
        self.pending_acknowledgment = True
        self.blocked_calls.append(BlockedCall(tool, args, datetime.now()))
        return self.format_duplicate_feedback(tool, args, prev_result)

    def validate_response(self, response: str) -> bool:
        """Check if model acknowledged and stated new approach."""
        if not self.pending_acknowledgment:
            return True

        # Look for acknowledgment patterns
        acknowledgment_patterns = [
            r"instead.*I will",
            r"different approach",
            r"let me try",
            r"I'll (check|verify|diagnose)",
            r"the issue is",
        ]

        for pattern in acknowledgment_patterns:
            if re.search(pattern, response, re.IGNORECASE):
                self.pending_acknowledgment = False
                return True

        return False
```

### 3. Escalating Intervention

If duplicates continue, escalate intervention:

```
Duplicate #1: "This call was blocked. What will you do differently?"
Duplicate #2: "You've now tried this twice. The approach isn't working. Options: [A] [B] [C]"
Duplicate #3: "Stopping automatic execution. Please tell me what you need help with."
```

### 4. Call Signature Hashing

Track not just exact duplicates but semantically similar calls:

```python
def is_semantically_duplicate(self, call1: ToolCall, call2: ToolCall) -> bool:
    """Check if calls are semantically similar even if not identical."""

    if call1.tool != call2.tool:
        return False

    # For filesystem.write_file, check if content is similar
    if call1.tool == "filesystem.write_file":
        # Same path + similar content = duplicate
        if call1.args["path"] == call2.args["path"]:
            similarity = self.content_similarity(
                call1.args["content"],
                call2.args["content"]
            )
            if similarity > 0.9:  # 90% similar
                return True

    return False
```

## Acceptance Criteria

- [ ] Duplicate feedback includes previous result
- [ ] Feedback asks "what will you do differently?"
- [ ] Model must acknowledge before continuing
- [ ] Escalating intervention on repeated duplicates
- [ ] Semantic similarity detection for near-duplicates

## Implementation Plan

### Phase 1: Enhanced Feedback
Include previous result and action prompt in duplicate message.

### Phase 2: Acknowledgment Check
Validate model response contains new approach.

### Phase 3: Escalation
Implement escalating intervention levels.

### Phase 4: Semantic Detection
Add similarity checking for near-duplicate calls.

## Escalation Levels

| Level | Trigger | Action |
|-------|---------|--------|
| 1 | First duplicate | Ask what will be different |
| 2 | Second duplicate | Present explicit options |
| 3 | Third duplicate | Stop and require user input |
| 4 | Fourth duplicate | End task with explanation |

## Test Cases

```python
def test_duplicate_feedback_includes_previous_result():
    """Duplicate feedback should show what happened before."""
    handler = DuplicateHandler()
    feedback = handler.handle_duplicate(
        tool="filesystem.write_file",
        args={"path": "x.py", "content": "..."},
        prev_result='{"error": "diagnostics_failed"}'
    )
    assert "diagnostics_failed" in feedback
    assert "What will you do differently" in feedback

def test_acknowledgment_required():
    """Model must acknowledge duplicate before continuing."""
    handler = DuplicateHandler()
    handler.handle_duplicate("tool", {}, "result")

    # Response without acknowledgment
    assert not handler.validate_response("Let me try writing the file again")

    # Response with acknowledgment
    assert handler.validate_response("Instead, I will check if pygame is installed first")

def test_escalation_on_repeated_duplicates():
    """Intervention should escalate with repeated duplicates."""
    # Setup: Trigger multiple duplicates
    # Assert: Each duplicate gets stronger intervention
```

## Implementation Log

{Entries added as work progresses}

## References

- Session: 72d15fe0-2192-4bba-9e8b-a3fadb509296
- Duplicate detected: 20:16:12
- Model continued without change
