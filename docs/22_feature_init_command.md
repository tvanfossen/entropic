# Feature: Interactive /init Command

## Overview

Implement an interactive `/init` command similar to Claude Code that helps users set up ENTROPI.md with project-specific context by analyzing the codebase and asking questions.

## Current State

- Basic `entropi init` CLI command creates `.entropi/` folder with:
  - `config.yaml` (copied from global)
  - `config.local.yaml` (template)
  - `ENTROPI.md` (minimal placeholder)
  - `.gitignore`

## Proposed Enhancement

### Interactive Project Analysis

When running `/init` or `entropi init --interactive`:

1. **Detect Project Type**
   - Scan for common files: `package.json`, `pyproject.toml`, `Cargo.toml`, `go.mod`, etc.
   - Identify frameworks: React, Django, FastAPI, etc.
   - Detect test frameworks: pytest, jest, etc.

2. **Ask Clarifying Questions**
   - Project description/purpose
   - Key architectural decisions
   - Coding standards or style guides used
   - Important directories to focus on or ignore

3. **Generate ENTROPI.md**
   - Populate with detected information
   - Include user-provided context
   - Structure with standard sections

### ENTROPI.md Sections

```markdown
# Project: {name}

## Overview
{user-provided description}

## Tech Stack
- Language: {detected}
- Framework: {detected}
- Test Framework: {detected}

## Project Structure
{auto-detected key directories}

## Development Commands
{detected from package.json scripts, Makefile, etc.}

## Coding Standards
{user-provided or detected from config files}

## Important Context
{user-provided notes}
```

## Implementation Tasks

- [ ] Add project type detection utilities
- [ ] Implement interactive Q&A flow
- [ ] Add ENTROPI.md template generation
- [ ] Support `--interactive` flag on CLI
- [ ] Add `/init` slash command for interactive mode

## References

- Claude Code's `/init` command for inspiration
- Current implementation in `src/entropi/cli.py`
- Context loading in `src/entropi/core/context.py`
