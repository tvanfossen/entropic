---
version: 1.0.0
type: proposal
schema_version: 1
id: P2-20260204-001
title: "Anti-Hallucination System Prompt Rules"
priority: P2
component: inference/prompts
author: tvanfossen
author_email: vanfosst@gmail.com
created: 2026-02-04
updated: 2026-02-04
tags: [prompt-engineering, accuracy, code-analysis]
completed_date: null
scoped_files:
  - src/entropi/prompts/
  - src/entropi/inference/adapters/
depends_on: []
blocks: []
---

# Anti-Hallucination System Prompt Rules

## Problem Statement

When analyzing code, the model fabricates "simplified" code snippets instead of quoting actual source. Observed in session.log:

```python
# Model wrote (fabricated):
if config.enable_filesystem:

# Actual code (line 57):
if mcp_config.enable_filesystem:
```

The model also invented acronym expansions ("MCP = Machine Control Protocol" when it's actually Model Context Protocol).

These hallucinations erode trust and can mislead users about their own codebase.

## Proposed Solution

Add explicit anti-hallucination rules to the system prompt template used for code analysis tasks. Rules should:

1. Prohibit paraphrasing code - quote verbatim or describe in prose
2. Require line number citations when referencing specific code
3. Forbid expanding acronyms unless expansion appears in source
4. Mandate "I don't know" when uncertain rather than fabricating

## Acceptance Criteria

- [ ] System prompt includes anti-hallucination section
- [ ] Code analysis responses cite line numbers
- [ ] No fabricated code snippets in analysis output
- [ ] Acronyms only expanded when source contains expansion
- [ ] Unit tests verify prompt includes required sections

## Implementation Plan

### Phase 1: Prompt Template Update
Add to base system prompt or code-analysis-specific prompt:

```markdown
## Code Reference Rules
- NEVER paraphrase or simplify code when quoting - use exact source text
- ALWAYS cite line numbers: "line 57: `if mcp_config.enable_filesystem:`"
- NEVER expand acronyms unless the expansion appears in the codebase
- If unsure about implementation details, say so - do not fabricate
- Prefer prose descriptions ("the permission check denies if...") over fake code
```

### Phase 2: Adapter Integration
Ensure adapters (qwen3, falcon, etc.) preserve these instructions through chat template formatting.

## Risks & Considerations

- Longer prompts consume context tokens
- May reduce response fluency if over-constrained
- Need to balance accuracy vs. helpfulness

## Implementation Log

{Entries added as work progresses}

## References

- Session log analysis: `.entropi/session.log` (2026-02-04 19:29:54)
- manager.py hallucination: fabricated `config.enable_filesystem` vs actual `mcp_config.enable_filesystem`
