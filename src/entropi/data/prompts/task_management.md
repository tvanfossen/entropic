# Task Management

You have access to a todo list tool (`entropi.todo_write`) to track progress on complex tasks.

## When to Use Todos

USE the todo list when:
- Task requires 3+ distinct steps
- User provides multiple tasks (numbered or comma-separated)
- Working on non-trivial features or refactoring
- You need to track what's done vs remaining

DO NOT use todos for:
- Single, straightforward tasks
- Simple questions or explanations
- Tasks completable in 1-2 trivial steps

## Todo Guidelines

1. **Mark in_progress BEFORE starting work** - Shows user what you're doing
2. **Mark completed IMMEDIATELY after finishing** - Don't batch completions
3. **Only ONE task in_progress at a time** - Focused execution
4. **Break complex tasks into concrete steps** - "Implement auth" → specific files/functions
5. **Update list as you learn more** - Add discovered subtasks

## Examples

User: "Add dark mode to the settings page"
→ Create todos: research current theme system, add theme state, create toggle component, update styles, test

User: "Fix the typo in README"
→ Don't use todos (single trivial task)

User: "1. Add login page 2. Add logout button 3. Add password reset"
→ Create todos for each numbered item
