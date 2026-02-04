# Entropi Development Guidelines

## Testing Requirements

**ALL TESTS MUST PASS BEFORE COMMIT**

- Unit tests with mocked behavior for isolated logic verification
- Integration tests with real models for actual system behavior
- Never assume a failing test is "pre-existing" - all failures must be resolved
- Pre-commit hooks run full test suite including real model tests

## Architecture Principles

### 1. Object-Oriented Design (ABC Classes)
- Use abstract base classes to define interfaces
- Concrete implementations inherit from ABCs
- Dependency injection for testability
- Composition over inheritance where appropriate

### 2. DRY (Don't Repeat Yourself)
- Single source of truth for all logic
- No duplicated business logic across modules
- Shared utilities for common operations
- Configuration-driven behavior where possible

### 3. Separation of Concerns
- Business logic separate from view/UI code
- Data access isolated from business rules
- Clear module boundaries with defined responsibilities

### 4. KISS (Keep It Simple, Stupid)
- KISS must NEVER violate OO principles
- KISS must NEVER violate DRY principles
- Simplicity in implementation, not in design rigor
- Avoid premature optimization, but maintain clean architecture

## Git Workflow

### Branch Strategy
```
main          <- User handles merges from develop (stable releases)
  └── develop <- Claude merges completed feature branches here
        └── feature/xyz <- Claude creates branches for each task
```

- **main**: Stable branch, user-managed merges only
- **develop**: Integration branch, Claude merges completed work here
- **feature/\***: Task-specific branches created from develop

### Creating a Feature Branch
```bash
git checkout develop
git pull origin develop
git checkout -b feature/descriptive-name
```

### Completing Work
1. Ensure all tests pass (pre-commit hooks)
2. Commit with clear message
3. Merge to develop: `git checkout develop && git merge feature/xyz`
4. Delete feature branch: `git branch -d feature/xyz`
5. Never push - user reviews and pushes

### Commits
- Commit early and often on feature branches
- Small, focused commits with clear messages
- Never skip pre-commit hooks - always resolve failures
- Never push without explicit user approval

### Branches
- Each branch = single encapsulated change
- Plans requiring multiple changes = multiple branches
- Test verification at each stage before proceeding
- Clear traceability from plan to implementation

### Pre-commit Hooks
- Run full test suite (unit + integration + real models)
- Generate verification report for each commit
- Track when/where failures start occurring
- MUST pass before commit is accepted

## Test Structure

```
tests/
├── unit/           # Mocked dependencies, fast, isolated
├── integration/    # Real components, may need setup
└── model/          # Real model inference tests
```

### Unit Tests
- Mock external dependencies (models, file system, network)
- Test business logic in isolation
- Fast execution for rapid feedback

### Model Tests
- Exercise real model loading/inference
- Verify actual system behavior
- Run as part of pre-commit verification

### Headless TUI Tests
- Test TUI logic without visual interface
- Verify real orchestrator behavior
- Catch regressions in user-facing flows
