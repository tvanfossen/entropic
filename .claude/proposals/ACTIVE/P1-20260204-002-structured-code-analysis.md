---
version: 1.0.0
type: proposal
schema_version: 1
id: P1-20260204-002
title: "Structured Code Analysis Output Format"
priority: P1
component: inference/analysis
author: tvanfossen
author_email: vanfosst@gmail.com
created: 2026-02-04
updated: 2026-02-04
tags: [code-analysis, output-format, accuracy]
completed_date: null
scoped_files:
  - src/entropi/prompts/
  - src/entropi/core/engine.py
depends_on: []
blocks: []
---

# Structured Code Analysis Output Format

## Problem Statement

Free-form code analysis allows the model to fabricate details. When asked to analyze `manager.py`, the model:

1. Invented simplified code snippets
2. Misnamed variables (`config` vs `mcp_config`)
3. Expanded MCP incorrectly

Unstructured output provides too much latitude for hallucination.

## Proposed Solution

Implement a two-phase structured analysis approach:

**Phase 1 - Extraction (Verifiable)**
Extract concrete facts from the file:
- Class/function names with line numbers
- Import statements
- Method signatures
- Docstrings (verbatim)

**Phase 2 - Synthesis (Constrained)**
Summarize based ONLY on extracted facts:
- Purpose derived from docstrings
- Relationships between classes
- Notable patterns

Output follows a strict template anchored to line numbers.

## Acceptance Criteria

- [ ] Analysis output follows defined schema
- [ ] All code references include line numbers
- [ ] Class/function lists are verifiable against source
- [ ] No fabricated code snippets in output
- [ ] Synthesis section clearly marked as interpretation

## Implementation Plan

### Phase 1: Define Output Schema

```markdown
## File Analysis: {path}

### Metadata
- Lines: {count}
- Imports: {list with line numbers}

### Classes
| Class | Line | Docstring |
|-------|------|-----------|
| {name} | {line} | {first line of docstring} |

### Functions/Methods
| Function | Line | Signature |
|----------|------|-----------|
| {name} | {line} | {params} -> {return} |

### Summary
{2-3 sentences derived from docstrings and structure}

### Key Patterns
- {pattern observed at line X}
```

### Phase 2: Extraction Tool
Create a tool or prompt that extracts structured data:

```python
class CodeAnalysisExtractor:
    """Extract verifiable facts from source code."""

    def extract(self, content: str) -> AnalysisData:
        """Parse and extract classes, functions, imports."""
        ...
```

### Phase 3: Synthesis Prompt
Prompt that takes extracted data and produces summary:

```
Given these extracted facts about {file}:
{extracted_data}

Summarize the file's purpose and patterns.
Do NOT add information not present in the extraction.
```

## Risks & Considerations

- More rigid output may feel less natural
- Extraction phase adds latency
- Schema needs to handle edge cases (nested classes, decorators, etc.)

## Implementation Log

{Entries added as work progresses}

## References

- Session log: 96 seconds for free-form analysis that contained errors
- Structured approach trades flexibility for accuracy
