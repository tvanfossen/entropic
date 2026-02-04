Transfer the current task to a different model tier.

## When to Use

Use `system.handoff` when a task would be better handled by a different tier's capabilities. Assess honestly whether your current tier is the right fit.

## Available Tiers

| Tier | Best For |
|------|----------|
| `simple` | Quick responses, summaries, trivial tasks |
| `normal` | General coding tasks, multi-step workflows |
| `code` | Implementation, writing/editing code, high tool usage |
| `thinking` | Deep analysis, architecture, complex planning |

## Routing Rules

Not all handoffs are permitted:

- **simple** can hand to: normal, code, thinking
- **normal** can hand to: simple, code, thinking
- **code** can hand to: simple, normal
- **thinking** can hand to: normal, code

## Parameters

- `target_tier` (required): The tier to hand off to
- `reason` (required): Brief explanation of why handoff is needed
- `task_state` (required): One of:
  - `not_started` - Task hasn't begun
  - `in_progress` - Work has started but isn't complete
  - `blocked` - Cannot proceed without different capabilities
  - `plan_ready` - Analysis complete, ready for execution

## Examples

```json
{"target_tier": "code", "reason": "Implementation plan ready", "task_state": "plan_ready"}
```

```json
{"target_tier": "thinking", "reason": "Need architectural analysis", "task_state": "blocked"}
```

```json
{"target_tier": "normal", "reason": "Task complete, summarize for user", "task_state": "in_progress"}
```
