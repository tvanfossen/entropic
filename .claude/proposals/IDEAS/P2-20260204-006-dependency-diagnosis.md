---
version: 1.0.0
type: proposal
schema_version: 1
id: P2-20260204-006
title: "Automatic Dependency Diagnosis on Import Errors"
priority: P2
component: core/engine
author: claude
author_email: noreply@anthropic.com
created: 2026-02-04
updated: 2026-02-04
tags: [error-recovery, diagnostics, dependencies, environment]
completed_date: null
scoped_files:
  - src/entropi/core/engine.py
  - src/entropi/mcp/servers/diagnostics.py
depends_on: []
blocks: []
---

# Automatic Dependency Diagnosis on Import Errors

## Problem Statement

In session 72d15fe0, all file writes failed with the same Pyright error:

```
Import "pygame" could not be resolved
```

This appeared in **every single failed write attempt** (6 times). The model:
1. Noted the error in thinking traces
2. Suggested user run `pip install pygame`
3. **Never used `bash.execute` to verify or fix the issue**
4. Kept attempting the same write with the same unresolved import

The model had tools to diagnose and potentially fix the problem but didn't use them.

## Failure Analysis

```
┌─────────────────────────────────────────────────────────────┐
│ ERROR PATTERN                                               │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  Error: Import "pygame" could not be resolved               │
│                   │                                         │
│                   ▼                                         │
│  Model thinking: "maybe user needs to install pygame"       │
│                   │                                         │
│                   ▼                                         │
│  Model action: Try writing file again (same error)          │
│                   │                                         │
│                   ▼                                         │
│  MISSED: bash.execute("pip list | grep pygame")             │
│  MISSED: bash.execute("pip install pygame")                 │
│  MISSED: Ask user about environment                         │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

## Proposed Solution

### 1. Import Error Detection

Parse diagnostics for import resolution failures:

```python
class ImportErrorDetector:
    """Detect and categorize import resolution errors."""

    IMPORT_ERROR_PATTERNS = [
        r'Import "([^"]+)" could not be resolved',
        r'No module named \'([^\']+)\'',
        r'ModuleNotFoundError: ([^\s]+)',
    ]

    def detect(self, diagnostics: list[Diagnostic]) -> list[MissingImport]:
        missing = []
        for diag in diagnostics:
            for pattern in self.IMPORT_ERROR_PATTERNS:
                match = re.search(pattern, diag.message)
                if match:
                    missing.append(MissingImport(
                        module=match.group(1),
                        source=diag.source,
                        line=diag.line
                    ))
        return missing
```

### 2. Automatic Diagnosis Trigger

When import errors detected, inject diagnosis prompt:

```
[IMPORT ERROR DETECTED]
Module "pygame" could not be resolved.

Before retrying the write, you should:
1. Check if the module is installed: bash.execute("pip list | grep -i pygame")
2. If not installed, either:
   a. Install it: bash.execute("pip install pygame")
   b. Ask user if they want it installed
   c. Use an alternative library that IS installed

Do NOT retry the same write without addressing the import error.
```

### 3. Available Alternatives Database

For common packages, suggest alternatives:

```python
ALTERNATIVES = {
    "pygame": ["tkinter (built-in)", "pyglet", "arcade"],
    "numpy": ["list comprehensions", "math module"],
    "requests": ["urllib.request (built-in)", "httpx"],
    "pandas": ["csv module (built-in)", "sqlite3"],
}
```

When pygame unavailable, prompt could suggest:
```
Alternative: tkinter is built-in and doesn't require installation.
Would you like to implement the GUI using tkinter instead?
```

### 4. Environment Check Tool

Add a dedicated diagnostic tool:

```python
async def check_python_environment(self) -> EnvironmentInfo:
    """Check Python environment for common issues."""
    result = EnvironmentInfo()

    # Check Python version
    result.python_version = await self.bash("python --version")

    # Check installed packages
    result.installed_packages = await self.bash("pip list --format=json")

    # Check if in venv
    result.in_venv = await self.bash("echo $VIRTUAL_ENV")

    return result
```

## Acceptance Criteria

- [ ] Import errors are detected from diagnostics
- [ ] Model is prompted to diagnose before retrying
- [ ] bash.execute is suggested for verification
- [ ] Alternatives are suggested for common packages
- [ ] Model cannot retry same failing write without action

## Implementation Plan

### Phase 1: Error Detection
Add ImportErrorDetector to diagnostics processing.

### Phase 2: Diagnosis Injection
When import errors found, inject diagnosis prompt.

### Phase 3: Alternatives Database
Build database of common packages and their alternatives.

### Phase 4: Environment Tool
Add dedicated environment check capability.

## Decision Tree

```
┌─────────────────────────────────────────────────────────────┐
│ Import "X" could not be resolved                            │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  Is X a standard library module?                            │
│  ├── YES: Likely Python version issue                       │
│  │        → Check Python version                            │
│  │        → Suggest version-appropriate code                │
│  │                                                          │
│  └── NO: Third-party package                                │
│          │                                                  │
│          ▼                                                  │
│      Is X installed? (pip list | grep X)                    │
│      ├── YES: Path/venv issue                               │
│      │        → Check VIRTUAL_ENV                           │
│      │        → Check sys.path                              │
│      │                                                      │
│      └── NO: Package not installed                          │
│              │                                              │
│              ▼                                              │
│          Can we install it?                                 │
│          ├── YES: pip install X                             │
│          │        (with user permission)                    │
│          │                                                  │
│          └── NO: Suggest alternative                        │
│                  from ALTERNATIVES table                    │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

## Test Cases

```python
def test_import_error_detected():
    """Import errors should be extracted from diagnostics."""
    diagnostics = [
        Diagnostic(line=1, message='Import "pygame" could not be resolved', source="Pyright")
    ]
    detector = ImportErrorDetector()
    missing = detector.detect(diagnostics)
    assert len(missing) == 1
    assert missing[0].module == "pygame"

def test_diagnosis_prompt_injected():
    """Import error should trigger diagnosis prompt."""
    # Setup: Write attempt with import error
    # Assert: Next turn includes diagnosis injection

def test_alternative_suggested():
    """Alternative should be suggested for known packages."""
    # Setup: pygame import error
    # Assert: tkinter suggested as alternative
```

## Implementation Log

{Entries added as work progresses}

## References

- Session: 72d15fe0-2192-4bba-9e8b-a3fadb509296
- Error appeared: 6 times across all write attempts
- Model never used bash.execute despite having access
