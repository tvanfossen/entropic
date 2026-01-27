# Interrupt and Inject During Generation

## Overview

This proposal adds the ability to interrupt model generation mid-stream and inject additional user input, similar to Claude Code's behavior. This enables real-time course correction during long-running generations.

## Current Behavior

```
User: "Implement a REST API for user management"
       │
       ▼
┌─────────────────────────────────────────────┐
│  Model generating... (streaming)             │
│  <think>                                     │
│  Let me design the API endpoints...          │
│  I'll start with a basic Flask setup...     │ ← User notices wrong direction
│  ...                                         │
│  </think>                                    │
│  Here's the implementation:                  │
│  ```python                                   │
│  from flask import Flask...                  │ ← Too late to redirect
└─────────────────────────────────────────────┘
```

**Current interrupt (Escape) behavior:**
- Sets `_interrupt_event` flag
- Loop exits on next iteration check
- Partial response may be lost
- No mechanism to inject corrections

## Proposed Behavior

```
User: "Implement a REST API for user management"
       │
       ▼
┌─────────────────────────────────────────────┐
│  Model generating... (streaming)             │
│  <think>                                     │
│  Let me design the API endpoints...          │
│  I'll start with a basic Flask setup...     │
└─────────────────────────────────────────────┘
       │
       │ [User presses Escape]
       ▼
┌─────────────────────────────────────────────┐
│  [Generation paused]                         │
│                                              │
│  Partial response saved. Options:            │
│  • Type to add context/correction            │
│  • Enter to continue generation              │
│  • Ctrl+C to cancel completely              │
│                                              │
│  > Use FastAPI instead of Flask, and use    │
│    SQLAlchemy for the database layer        │
└─────────────────────────────────────────────┘
       │
       ▼
┌─────────────────────────────────────────────┐
│  Model continuing with injected context...   │
│  <think>                                     │
│  The user wants FastAPI + SQLAlchemy...      │
│  Let me revise the approach...               │
│  </think>                                    │
│  Here's the updated implementation:          │
│  ```python                                   │
│  from fastapi import FastAPI...              │
└─────────────────────────────────────────────┘
```

## Architecture

### State Machine

```
                    ┌─────────────┐
                    │    IDLE     │
                    └──────┬──────┘
                           │ user_input
                           ▼
                    ┌─────────────┐
              ┌─────│  EXECUTING  │─────┐
              │     └──────┬──────┘     │
              │            │            │
         Escape            │ complete   │ error
              │            │            │
              ▼            ▼            ▼
       ┌─────────────┐ ┌─────────┐ ┌─────────┐
       │  PAUSED     │ │COMPLETE │ │  ERROR  │
       └──────┬──────┘ └─────────┘ └─────────┘
              │
    ┌─────────┼─────────┐
    │         │         │
   Enter   injection  Ctrl+C
    │         │         │
    ▼         ▼         ▼
┌─────────┐ ┌─────────────┐ ┌─────────────┐
│EXECUTING│ │  EXECUTING  │ │ INTERRUPTED │
│(resume) │ │ (w/context) │ │  (cancel)   │
└─────────┘ └─────────────┘ └─────────────┘
```

### New Agent State

```python
class AgentState(Enum):
    """Agent execution states."""
    IDLE = auto()
    PLANNING = auto()
    EXECUTING = auto()
    WAITING_TOOL = auto()
    VERIFYING = auto()
    COMPLETE = auto()
    ERROR = auto()
    INTERRUPTED = auto()
    PAUSED = auto()  # NEW: Generation paused, awaiting user input
```

### Interrupt Modes

```python
class InterruptMode(Enum):
    """How to handle generation interrupt."""
    CANCEL = "cancel"       # Discard partial response, stop
    PAUSE = "pause"         # Keep partial response, await input
    INJECT = "inject"       # Keep partial, inject context, continue
```

---

## Implementation

### 1. Engine Changes

```python
# src/entropi/core/engine.py

@dataclass
class InterruptContext:
    """Context for interrupted generation."""
    partial_content: str
    partial_tool_calls: list[ToolCall]
    injection: str | None = None
    mode: InterruptMode = InterruptMode.PAUSE


class AgentEngine:
    def __init__(self, ...):
        # ... existing ...
        self._interrupt_event = asyncio.Event()
        self._pause_event = asyncio.Event()
        self._injection_queue: asyncio.Queue[str] = asyncio.Queue()
        self._interrupt_context: InterruptContext | None = None

    async def _generate_response(self, ctx: LoopContext) -> tuple[str, list[ToolCall]]:
        """Generate model response with interrupt support."""
        if self.loop_config.stream_output:
            content = ""

            async for chunk in self.orchestrator.generate_stream(
                ctx.messages, tier=ctx.locked_tier
            ):
                # Check for pause/interrupt during streaming
                if self._pause_event.is_set():
                    # Save partial state
                    self._interrupt_context = InterruptContext(
                        partial_content=content,
                        partial_tool_calls=[],
                        mode=InterruptMode.PAUSE,
                    )
                    self._set_state(ctx, AgentState.PAUSED)

                    # Wait for resume or injection
                    injection = await self._wait_for_resume_or_inject()

                    if injection:
                        # Inject user context and continue
                        self._interrupt_context.injection = injection
                        content = await self._continue_with_injection(
                            ctx, content, injection
                        )
                    else:
                        # Resume without injection
                        continue

                content += chunk

                if self._on_stream_chunk:
                    self._on_stream_chunk(chunk)

            # ... rest of method ...

    async def _wait_for_resume_or_inject(self) -> str | None:
        """Wait for user to resume or inject content."""
        try:
            # Wait for injection with timeout, or just resume on empty
            injection = await asyncio.wait_for(
                self._injection_queue.get(),
                timeout=None,  # Wait indefinitely
            )
            return injection if injection.strip() else None
        except asyncio.CancelledError:
            return None

    async def _continue_with_injection(
        self,
        ctx: LoopContext,
        partial_content: str,
        injection: str,
    ) -> str:
        """Continue generation with injected user context."""
        # Add partial response and injection to context
        if partial_content:
            ctx.messages.append(Message(
                role="assistant",
                content=partial_content + "\n[Generation paused by user]",
            ))

        ctx.messages.append(Message(
            role="user",
            content=f"[User interjection]: {injection}\n\nPlease continue with this context in mind.",
        ))

        # Clear pause state
        self._pause_event.clear()
        self._set_state(ctx, AgentState.EXECUTING)

        # Generate new response incorporating injection
        content = ""
        async for chunk in self.orchestrator.generate_stream(
            ctx.messages, tier=ctx.locked_tier
        ):
            content += chunk
            if self._on_stream_chunk:
                self._on_stream_chunk(chunk)

        return content

    def pause(self) -> None:
        """Pause generation (Escape key)."""
        self._pause_event.set()

    def inject(self, content: str) -> None:
        """Inject user content and resume."""
        self._injection_queue.put_nowait(content)

    def resume(self) -> None:
        """Resume without injection."""
        self._injection_queue.put_nowait("")
```

### 2. UI Changes

```python
# src/entropi/ui/terminal.py

class TerminalUI:
    def __init__(self, ...):
        # ... existing ...
        self._on_pause: Callable[[], None] | None = None
        self._on_inject: Callable[[str], None] | None = None
        self._generation_paused = False

    def _create_key_bindings(self) -> KeyBindings:
        kb = KeyBindings()

        @kb.add("escape")
        def _(event: Any) -> None:
            """Handle Escape - pause generation for potential injection."""
            if self._on_pause:
                self._on_pause()
                self._generation_paused = True

        @kb.add("c-c")
        def _(event: Any) -> None:
            """Handle Ctrl+C - interrupt/cancel."""
            if self._generation_paused:
                # Cancel completely during pause
                if self._on_interrupt:
                    self._on_interrupt()
                self._generation_paused = False
            else:
                # Normal interrupt behavior
                if self._on_interrupt:
                    self._on_interrupt()
                raise KeyboardInterrupt()

        return kb

    def set_pause_callback(self, callback: Callable[[], None]) -> None:
        """Set callback for pause handling."""
        self._on_pause = callback

    def set_inject_callback(self, callback: Callable[[str], None]) -> None:
        """Set callback for content injection."""
        self._on_inject = callback

    async def prompt_injection(self, partial_content: str) -> str | None:
        """
        Prompt user for injection content after pause.

        Args:
            partial_content: The partial response so far

        Returns:
            User's injection text, empty string to resume, None to cancel
        """
        self.console.print()
        self.console.print(
            f"[{self.theme.warning_color}]"
            f"━━━ Generation paused ━━━[/]"
        )
        self.console.print()

        # Show partial content preview
        if partial_content:
            preview = partial_content[:200]
            if len(partial_content) > 200:
                preview += "..."
            self.console.print(f"[dim]Partial response:[/]")
            self.console.print(f"[dim]{preview}[/]")
            self.console.print()

        self.console.print(
            f"[dim]Options:[/]\n"
            f"  • Type to add context/correction\n"
            f"  • Press [bold]Enter[/] to continue generation\n"
            f"  • Press [bold]Ctrl+C[/] to cancel\n"
        )

        try:
            response = await self.session.prompt_async(
                "inject> ",
                default="",
            )
            self._generation_paused = False
            return response
        except (EOFError, KeyboardInterrupt):
            self._generation_paused = False
            return None
```

### 3. App Integration

```python
# src/entropi/app.py

class Application:
    async def _run_generation(self, user_input: str, system_prompt: str | None) -> None:
        """Run generation with interrupt/inject support."""

        streaming_active = False

        def on_chunk(chunk: str) -> None:
            nonlocal streaming_active
            streaming_active = True
            self._ui.stream_text(chunk)

        def on_pause() -> None:
            """Handle pause from Escape key."""
            self._engine.pause()

        async def handle_paused_state() -> None:
            """Handle the PAUSED state - prompt for injection."""
            partial = ""
            if self._engine._interrupt_context:
                partial = self._engine._interrupt_context.partial_content

            # End current streaming line
            if streaming_active:
                self.console.print()

            # Prompt for injection
            injection = await self._ui.prompt_injection(partial)

            if injection is None:
                # User cancelled
                self._engine.interrupt()
            elif injection.strip():
                # User provided injection
                self._engine.inject(injection)
            else:
                # User pressed Enter to continue
                self._engine.resume()

        # Set up callbacks
        self._engine.set_callbacks(
            on_state_change=self._handle_state_change,
            on_tool_call=self._handle_tool_approval,
            on_stream_chunk=on_chunk,
            # ... other callbacks ...
        )

        self._ui.set_pause_callback(on_pause)

        # Run with state monitoring
        try:
            async for msg in self._engine.run(user_input, ...):
                # Check for paused state
                if self._engine.state == AgentState.PAUSED:
                    await handle_paused_state()
                    continue

                # Normal message handling
                # ...
```

---

## User Experience

### Visual Feedback

```
You: Implement a REST API for user management

A: <think>
Let me design the API endpoints...
I'll start with a basic Flask setup with the following routes:
- GET /users - list all users
- POST /users - create user
- GET /users/{id} - get user
- PUT /users/{id} - update user
- DELETE /users/{id} - delete user

Now let me implement

━━━ Generation paused ━━━

Partial response:
<think>
Let me design the API endpoints...
I'll start with a basic Flask setup with the following routes...

Options:
  • Type to add context/correction
  • Press Enter to continue generation
  • Press Ctrl+C to cancel

inject> Use FastAPI with async/await and Pydantic models for validation

A: </think>

[Continuing with user context: Use FastAPI with async/await and Pydantic models for validation]

<think>
The user wants FastAPI instead of Flask. Let me revise...
FastAPI is better for async operations and has built-in Pydantic support.
I'll also add proper validation models.
</think>

Here's the updated implementation using FastAPI:

```python
from fastapi import FastAPI, HTTPException
from pydantic import BaseModel, EmailStr
...
```
```

### Keyboard Shortcuts

| Key | During Generation | During Pause |
|-----|-------------------|--------------|
| `Escape` | Pause generation | (no effect) |
| `Enter` | (input) | Resume generation |
| `Ctrl+C` | Interrupt + cancel | Cancel completely |
| Any text | (input) | Injection content |

---

## Configuration

```yaml
# .entropi/config.yaml

generation:
  # Enable interrupt/inject feature
  interrupt_inject: true

  # Auto-save partial responses on interrupt
  save_partial: true

  # Show thinking content in partial preview
  show_thinking_in_preview: true

  # Maximum preview length for partial content
  preview_max_length: 500
```

---

## Edge Cases

### 1. Interrupt During Tool Execution

```
Scenario: User presses Escape while a tool is executing

Behavior:
- Tool execution completes (can't interrupt mid-execution)
- Pause takes effect before next generation
- User can inject context about tool results
```

### 2. Interrupt During Think Block

```
Scenario: User presses Escape during <think> block

Behavior:
- Thinking is paused
- User sees partial thinking (if enabled)
- Injection can redirect thinking
```

### 3. Multiple Rapid Escapes

```
Scenario: User presses Escape multiple times quickly

Behavior:
- First Escape triggers pause
- Subsequent Escapes ignored while paused
- Debounce of 500ms between pause triggers
```

### 4. Injection Causes Tool Call

```
Scenario: User injection causes model to call a new tool

Behavior:
- Normal tool execution flow
- Tool approval still required
- Loop continues with injected context
```

---

## Implementation Phases

### Phase 1: Basic Pause/Resume
- Escape pauses generation
- Enter resumes
- Ctrl+C cancels
- No injection yet

**Estimated effort:** 1 day

### Phase 2: Content Injection
- Prompt for injection text
- Inject into conversation
- Resume with context

**Estimated effort:** 1-2 days

### Phase 3: UI Polish
- Partial content preview
- Visual feedback
- Progress indicator during pause

**Estimated effort:** 1 day

### Phase 4: Edge Cases & Testing
- Tool execution handling
- Multiple interrupt handling
- State machine validation

**Estimated effort:** 1 day

---

## Comparison with Claude Code

| Feature | Claude Code | Entropi (Proposed) |
|---------|-------------|-------------------|
| Pause generation | Escape | Escape |
| Cancel | Ctrl+C | Ctrl+C |
| Inject context | Type + Enter | Type + Enter |
| Resume without inject | Enter | Enter |
| Show partial | Yes | Yes |
| Edit partial | No | No (future?) |
| Undo injection | No | No |

---

## Future Enhancements

1. **Edit Partial Response**: Allow editing the partial response before resuming
2. **Injection History**: Track and replay previous injections
3. **Auto-Pause**: Automatically pause on certain patterns (e.g., wrong direction detected)
4. **Branching**: Create conversation branches from pause points
5. **Partial Save**: Save partial responses for later continuation

---

## Summary

This feature enables real-time course correction during model generation, significantly improving the interactive experience. By allowing users to pause, inject context, and resume, they can guide the model more effectively without waiting for complete (potentially wrong) responses.

**Key benefits:**
- Faster iteration on complex tasks
- Reduced wasted tokens on wrong approaches
- More conversational, interactive feel
- Matches Claude Code UX expectations
