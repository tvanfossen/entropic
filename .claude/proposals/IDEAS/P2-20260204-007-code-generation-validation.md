---
version: 1.0.0
type: proposal
schema_version: 1
id: P2-20260204-007
title: "Pre-Write Code Validation for Generated Python"
priority: P2
component: inference/adapters
author: claude
author_email: noreply@anthropic.com
created: 2026-02-04
updated: 2026-02-04
tags: [code-generation, validation, syntax, python]
completed_date: null
scoped_files:
  - src/entropi/inference/adapters/qwen3.py
  - src/entropi/mcp/servers/filesystem.py
depends_on: []
blocks: []
---

# Pre-Write Code Validation for Generated Python

## Problem Statement

In session 72d15fe0, the model generated Python code with the same syntax error pattern **repeatedly**:

```python
# GENERATED (WRONG)
class Piece(ABC):
    @abstractmethod
def move(self, new_position):  # Missing indentation after decorator!
        pass
```

Correct form:
```python
class Piece(ABC):
    @abstractmethod
    def move(self, new_position):  # Properly indented
        pass
```

This error appeared in **every single code generation attempt**. The model:
1. Generated broken code
2. Submitted to filesystem.write_file
3. Received Pyright error about decorator
4. Generated the **exact same broken pattern** again

## Error Pattern Analysis

```
┌─────────────────────────────────────────────────────────────┐
│ REPEATED GENERATION ERROR                                   │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  Attempt 1:  @abstractmethod                                │
│              def move(...)      ← No indent after decorator │
│                                                             │
│  Pyright: "Expected function declaration after decorator"   │
│                                                             │
│  Attempt 2:  @abstractmethod                                │
│              def move(...)      ← SAME ERROR                │
│                                                             │
│  (repeated 6 times with identical mistake)                  │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

Root cause: Model's internal code generation may have indentation issues when constructing multi-line strings, especially after decorators.

## Proposed Solution

### 1. Pre-Write Syntax Validation

Validate Python syntax BEFORE calling filesystem.write_file:

```python
import ast

class CodeValidator:
    """Validate generated code before writing."""

    def validate_python(self, code: str) -> ValidationResult:
        """Check Python code for syntax errors."""
        try:
            ast.parse(code)
            return ValidationResult(valid=True)
        except SyntaxError as e:
            return ValidationResult(
                valid=False,
                error=f"Syntax error at line {e.lineno}: {e.msg}",
                line=e.lineno,
                offset=e.offset
            )

    def check_indentation(self, code: str) -> list[IndentationIssue]:
        """Check for common indentation problems."""
        issues = []
        lines = code.split('\n')

        for i, line in enumerate(lines):
            # Check decorator followed by unindented def
            if line.strip().startswith('@') and i + 1 < len(lines):
                next_line = lines[i + 1]
                if next_line.strip().startswith('def '):
                    current_indent = len(line) - len(line.lstrip())
                    next_indent = len(next_line) - len(next_line.lstrip())
                    if next_indent <= current_indent:
                        issues.append(IndentationIssue(
                            line=i + 2,  # 1-indexed
                            message="Function after decorator should have same or greater indentation"
                        ))
        return issues
```

### 2. Validation in Filesystem Server

Add validation step before write:

```python
async def write_file(self, path: str, content: str) -> WriteResult:
    """Write file with pre-validation."""

    # Detect file type
    if path.endswith('.py'):
        validation = self.validator.validate_python(content)
        if not validation.valid:
            return WriteResult(
                success=False,
                error="syntax_error",
                message=f"Generated code has syntax error: {validation.error}",
                tip="Fix the syntax error before writing. Common issues: indentation after decorators, missing colons."
            )

    # Proceed with write and diagnostics
    ...
```

### 3. Error-Specific Feedback

When specific patterns detected, provide targeted feedback:

```python
ERROR_PATTERNS = {
    "decorator_indent": {
        "pattern": r"@\w+\n\s*def ",  # Decorator followed by def with wrong indent
        "message": "Decorator and function definition must have matching indentation within class",
        "fix_hint": "Add 4 spaces before 'def' to match '@decorator' indentation"
    },
    "missing_colon": {
        "pattern": r"def \w+\([^)]*\)\s*\n",  # def without colon
        "message": "Function definition missing colon",
        "fix_hint": "Add ':' after the function signature"
    }
}
```

### 4. Regeneration with Hint

If validation fails, prompt model with specific fix:

```
[CODE VALIDATION FAILED]
Your generated Python code has a syntax error:
Line 11: Function after decorator should have same indentation

Your code:
    @abstractmethod
def move(self, new_position):

Should be:
    @abstractmethod
    def move(self, new_position):

Please regenerate with correct indentation.
```

## Acceptance Criteria

- [ ] Python syntax checked before filesystem write
- [ ] Decorator indentation specifically validated
- [ ] Validation errors provide specific fix hints
- [ ] Model receives targeted feedback for regeneration
- [ ] Same syntax error cannot repeat more than once

## Implementation Plan

### Phase 1: Basic Syntax Validation
Add ast.parse() check before writes.

### Phase 2: Pattern-Specific Checks
Add checks for known error patterns (decorators, colons, etc.).

### Phase 3: Targeted Feedback
Generate specific fix hints for each error type.

### Phase 4: Regeneration Loop
If validation fails, prompt model to fix specific issue.

## Common Python Generation Errors

| Error | Pattern | Fix |
|-------|---------|-----|
| Decorator indent | `@foo\ndef` | Add matching indent to def |
| Missing colon | `def foo()` | Add `:` after signature |
| Mixed tabs/spaces | Inconsistent | Normalize to 4 spaces |
| Unclosed string | `"foo...` | Close the string |
| Unmatched brackets | `func([)` | Balance brackets |

## Test Cases

```python
def test_decorator_indent_detected():
    """Wrong indentation after decorator should be caught."""
    code = '''
class Foo:
    @abstractmethod
def bar(self):
        pass
'''
    validator = CodeValidator()
    issues = validator.check_indentation(code)
    assert len(issues) == 1
    assert "decorator" in issues[0].message.lower()

def test_valid_code_passes():
    """Correctly indented code should pass."""
    code = '''
class Foo:
    @abstractmethod
    def bar(self):
        pass
'''
    validator = CodeValidator()
    result = validator.validate_python(code)
    assert result.valid

def test_feedback_includes_fix():
    """Validation failure should include fix hint."""
    # Setup: Generate code with decorator error
    # Assert: Feedback message includes correct indentation example
```

## Implementation Log

{Entries added as work progresses}

## References

- Session: 72d15fe0-2192-4bba-9e8b-a3fadb509296
- Error lines: 11, 14, 17, 32 (decorator issues in every attempt)
- Same error repeated 6 times without correction
