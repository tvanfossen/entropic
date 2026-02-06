---
version: 1.0.0
type: proposal
schema_version: 1
id: P2-20260204-003
title: "Code Snippet Validation Hook"
priority: P2
component: core/engine
author: tvanfossen
author_email: vanfosst@gmail.com
created: 2026-02-04
updated: 2026-02-04
tags: [validation, accuracy, post-processing]
completed_date: null
scoped_files:
  - src/entropi/core/engine.py
  - src/entropi/mcp/servers/filesystem.py
depends_on: []
blocks: []
---

# Code Snippet Validation Hook

## Problem Statement

Even with prompt improvements, models may still fabricate code snippets. A defense-in-depth approach requires runtime validation of generated content against actual source files.

Example fabrication from session.log:
```python
# Model output contained:
if config.enable_filesystem:
    self._clients["filesystem"] = MCPClient(...)

# Actual source (line 57-62):
if mcp_config.enable_filesystem:
    self._clients["filesystem"] = MCPClient(
        name="filesystem",
        command="python",
        args=["-W", "ignore", "-m", "entropi.mcp.servers.filesystem"],
    )
```

## Proposed Solution

Implement a post-processing validation hook that:

1. Extracts code blocks from model output (```python ... ```)
2. Checks if snippets exist in recently-read source files
3. Flags mismatches with warning to user
4. Optionally auto-corrects by finding closest match

## Acceptance Criteria

- [ ] Code blocks in output are validated against source
- [ ] Mismatches logged with severity indicator
- [ ] User notified when fabrication detected
- [ ] File tracker provides recently-read file contents for validation
- [ ] Fuzzy matching handles minor whitespace differences

## Implementation Plan

### Phase 1: Code Block Extraction

```python
import re

def extract_code_blocks(content: str) -> list[tuple[str, str]]:
    """Extract (language, code) tuples from markdown."""
    pattern = r'```(\w+)?\n(.*?)```'
    return re.findall(pattern, content, re.DOTALL)
```

### Phase 2: Validation Against File Tracker

```python
class SnippetValidator:
    """Validate code snippets against source files."""

    def __init__(self, file_tracker: FileTracker):
        self.tracker = file_tracker

    def validate(self, snippet: str) -> ValidationResult:
        """Check if snippet exists in any tracked file."""
        # Normalize whitespace
        normalized = self._normalize(snippet)

        for path, content in self.tracker.get_recent_files():
            if normalized in self._normalize(content):
                return ValidationResult(valid=True, source=path)

        # Try fuzzy match
        closest = self._find_closest(normalized)
        return ValidationResult(
            valid=False,
            closest_match=closest,
            similarity=self._similarity(normalized, closest)
        )
```

### Phase 3: Engine Integration

Hook into engine after model output, before presenting to user:

```python
# In AgentEngine._process_response()
if self.config.validate_snippets:
    warnings = self._validator.check_output(response.content)
    if warnings:
        self._presenter.print_warning(
            f"Potential fabricated code detected: {warnings}"
        )
```

## Risks & Considerations

- Performance overhead for validation
- False positives if model legitimately generates new code
- Need to distinguish "quoting source" vs "writing new code"
- Fuzzy matching complexity

## Implementation Log

{Entries added as work progresses}

## References

- FileTracker already exists: `src/entropi/mcp/servers/file_tracker.py`
- Could leverage existing read tracking for validation context
