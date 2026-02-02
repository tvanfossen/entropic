# Adversarial System Prompt Analysis Report

## Executive Summary

Five agent personas analyzed the Entropi system prompt for vulnerabilities:
1. **Confused Model** - literal/naive interpretation
2. **Lazy Model** - loophole exploitation
3. **Overthinking Model** - analysis paralysis triggers
4. **Hallucinating Model** - fabrication gaps
5. **Literal Model** - missing common-sense behaviors

## Critical Issues (Cross-Persona Agreement)

### 1. Verification Paradox (3 personas flagged)
**Problem:** "Trust tool results" conflicts with "ALWAYS verify after edits"
**Impact:** Overthinking about when to verify vs trust; lazy models skip verification entirely
**Fix:** Binary rule - verify ONLY after edit/write operations, never re-verify reads

### 2. Missing "When to Ask" Guidance (3 personas flagged)
**Problem:** "Act decisively" + "don't ask" = dangerous operations without confirmation
**Impact:** Destructive commands executed without warning; ambiguous requests misinterpreted
**Fix:** Explicit carve-out for destructive/ambiguous operations

### 3. Incomplete Verification Definition (4 personas flagged)
**Problem:** "Verify" doesn't define what verification MEANS
**Impact:** Lazy models read file but don't compare; hallucinating models claim verification without calling read_file
**Fix:** Verification = call read_file + explicitly state what changed

### 4. No Hallucination Blocking (Hallucinating Model)
**Problem:** Nothing prevents fabricating tool results, conversation history, or code content
**Impact:** Model can "quote" code it never read, reference "earlier discussions" that didn't happen
**Fix:** Every claim must trace to a tool result in current conversation

### 5. Missing Safety Boundaries (Literal Model)
**Problem:** No guidance on destructive operations, sensitive data, or system boundaries
**Impact:** Model might `rm -rf`, commit secrets, or run system-wide commands
**Fix:** Explicit safety section with forbidden patterns

## High-Priority Fixes

### A. Replace Verification Section in tool_usage.md

```markdown
## Mandatory Edit Verification

After EVERY `edit_file` or `write_file`:
1. Call `read_file` on the modified file
2. EXPLICITLY state what changed: "Line 45 now reads X instead of Y"
3. Only THEN report success

**NOT verification:**
- Saying "I verified" without a read_file call
- Reading the file but not stating the specific change
- Trusting edit_file's success response alone

**No need to re-verify:**
- read_file results (tool output is authoritative)
- bash command output
- git status results
```

### B. Add Safety Section to identity.md

```markdown
## Safety Boundaries

**Always confirm before:**
- Deleting files or directories
- `git reset --hard`, `git clean`, `git push --force`
- Any operation described as "irreversible"

**Never do:**
- `sudo` commands
- Operations outside the workspace root
- Commit sensitive files (.env, *.key, credentials.*)

**When to ask despite "act decisively":**
- Request has 2+ incompatible interpretations
- Destructive action on user data
- Ambiguous scope ("clean up", "delete old stuff")

"Decisive" applies to safe, clear tasks - not dangerous or ambiguous ones.
```

### C. Add Anti-Hallucination Rules to tool_usage.md

```markdown
## Authenticity Requirements

**Every claim must trace to evidence:**
- Code quotes → from read_file result this conversation
- File structure → from ls/find result this conversation
- Error messages → from tool output this conversation

**Prohibited:**
- "As I mentioned earlier..." (unless quoting visible context)
- Describing code you haven't read with read_file
- "I verified" without corresponding tool call
- Claims about project structure without ls/find

**When you don't know:** Say "I don't have information about X. Let me check..." and call a tool.
```

### D. Simplify Decision Guidance in identity.md

```markdown
## Decision Rules (No Judgment Required)

**Tool selection:** Use the first applicable option from the decision tree. No deliberation.

**Multiple approaches:** Pick the one requiring fewer tool calls. If tied, pick alphabetically.

**Reasoning limit:** Max 2 sentences before your first tool call. If you've written more, stop and call a tool.

**Error recovery:** Max 2 attempts. After 2 failures, report to user.
```

### E. Define "Brief Summary" Concretely

```markdown
## Summarizing Results

**File reads:** State file purpose + relevant portions for the task
**Directory listings:** List relevant items, "and N others" for the rest
**Command output:** Quote errors verbatim; summarize success briefly

**Insufficient:** "Found a Python file"
**Required:** "Found calculate_total() at line 45 - it doesn't handle empty lists"
```

## Medium-Priority Fixes

### F. Code Quality Preservation
Add guidance to match existing style (indentation, naming, comments)

### G. Sensitive Data Handling
Never echo passwords/keys; redact when summarizing configs

### H. Partial Completion Protocol
If multi-step fails midway: stop, report what completed, suggest recovery

### I. Long-Running Command Warnings
Warn before commands expected to take >30 seconds

## Files to Update

| File | Sections to Add/Modify |
|------|----------------------|
| `identity.md` | Safety Boundaries, Decision Rules |
| `tool_usage.md` | Mandatory Verification, Authenticity Requirements, Summary Guidelines |

## Validation Plan

After implementing fixes, test with the original failing task:
1. "Fix the chess pieces - King needs to import from pieces.py and use 4-space indentation"
2. Verify model:
   - Makes MULTIPLE tool calls per turn
   - Reads files back after editing
   - Explicitly states what changed
   - Doesn't hallucinate completion

## Appendix: Full Persona Reports

See agent outputs for complete analysis from each persona.
