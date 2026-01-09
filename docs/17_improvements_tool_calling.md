# Tool Calling Improvements

This document covers identified issues and improvement opportunities for Entropi's tool calling system.

## Current State (Updated)

The tool calling infrastructure is now significantly improved:
- 14 tools available (filesystem, bash, git)
- Tool calls are parsed from model output (XML and markdown formats)
- Tools execute via MCP servers
- Results are fed back to the model
- **NEW**: System prompt includes explicit guidelines to prevent repeated calls
- **NEW**: Duplicate tool call detection prevents re-execution of identical calls
- **NEW**: Tool execution status shows real-time feedback (start/complete)
- **NEW**: Interactive tool approval for sensitive operations
- **NEW**: Structured error recovery with suggestions

---

## Issue 1: Model Doesn't Use XML Tool Call Format

### Problem
The system prompt instructs the model to use XML-style markers:
```
<tool_call>
{"name": "tool_name", "arguments": {"arg1": "value1"}}
</tool_call>
```

But the model generates markdown code blocks instead:
```json
{"name": "tool_name", "arguments": {"arg1": "value1"}}
```

### Current Workaround
The parser (`inference/adapters/qwen.py`) now recognizes both formats:

```python
# Pattern 1: XML-style <tool_call> markers
pattern = re.compile(
    rf"{re.escape(self.TOOL_CALL_START)}\s*(.*?)\s*{re.escape(self.TOOL_CALL_END)}",
    re.DOTALL,
)

# Pattern 2: Markdown code blocks with JSON containing "name" field
markdown_pattern = re.compile(
    r"```(?:json)?\s*\n?\s*(\{[^`]*\"name\"\s*:\s*\"[^\"]+\"[^`]*\})\s*\n?\s*```",
    re.DOTALL,
)
```

### Proper Fix Options

1. **Fine-tune model for tool use format**
   - Create training data with proper `<tool_call>` format
   - Fine-tune Qwen models on tool use examples

2. **Use native tool calling (if supported)**
   - Qwen2.5 models may support native function calling
   - Investigate llama.cpp function calling support

3. **Improve system prompt**
   - Add more explicit examples
   - Add "IMPORTANT: Use exactly this format" emphasis
   - Show negative examples of what NOT to do

---

## Issue 2: Model Calls Tools Repeatedly Instead of Synthesizing

### Problem
When asked "list files in current directory", the model:
1. Calls `filesystem.list_directory` ✓
2. Gets result (list of files) ✓
3. **Calls the same tool again** ✗
4. Gets same result ✗
5. Repeats until max iterations ✗

The model should synthesize the result after the first successful call.

### Root Cause
The model doesn't understand that:
- It already has the answer
- It should present the result to the user
- No more tool calls are needed

### Improvement Options

#### Option A: Improve System Prompt
Add explicit instructions about when to stop:

```markdown
## Tool Use Guidelines

1. Call a tool when you need information you don't have
2. After receiving a tool result, PRESENT IT TO THE USER
3. Do NOT call the same tool again unless the result was an error
4. Once you have the information, respond with your answer

WRONG:
- User asks for files → call list_directory → call list_directory again

RIGHT:
- User asks for files → call list_directory → "Here are the files: ..."
```

#### Option B: Add Tool Result Detection
In the engine, detect when a tool result was just received and add a hint:

```python
# After tool result, add context hint
if last_message_was_tool_result:
    ctx.messages.append(Message(
        role="system",
        content="Tool result received. Now respond to the user with this information."
    ))
```

#### Option C: Limit Same-Tool Calls
Track tool calls and prevent duplicate calls:

```python
class LoopContext:
    recent_tool_calls: set[str] = field(default_factory=set)

def _should_skip_tool(self, tool_call: ToolCall) -> bool:
    key = f"{tool_call.name}:{hash(frozenset(tool_call.arguments.items()))}"
    if key in self.recent_tool_calls:
        return True
    self.recent_tool_calls.add(key)
    return False
```

#### Option D: Early Termination Heuristics
Add heuristics to detect when to stop the loop:

```python
def _should_stop_early(self, ctx: LoopContext, content: str) -> bool:
    # If response mentions the tool result, we're done
    if "files" in content.lower() and ctx.metrics.tool_calls > 0:
        return True
    # If model says "here is/are", likely presenting results
    if re.search(r"here (is|are)", content.lower()):
        return True
    return False
```

---

## Issue 3: No Streaming of Tool Execution Status

### Problem
During tool execution, the user sees nothing until the model responds.

### Improvement
Add real-time status updates:

```python
async def _execute_tool(self, ctx: LoopContext, tool_call: ToolCall) -> Message:
    if self._on_tool_start:
        self._on_tool_start(tool_call)  # "Executing filesystem.list_directory..."

    result = await self.server_manager.execute(tool_call)

    if self._on_tool_complete:
        self._on_tool_complete(tool_call, result)  # "Done (0.2s)"

    return result
```

UI could show:
```
🔧 Calling filesystem.list_directory...
✓ Got 10 files (0.2s)
```

---

## Issue 4: Tool Permission UX

### Problem
Tool permissions are configured in YAML but there's no interactive approval flow.

### Current State
```python
def _handle_tool_approval(self, tool_call: Any) -> bool:
    # For now, auto-approve based on config
    # TODO: Add interactive approval
    return self.config.permissions.auto_approve
```

### Improvement
Add interactive approval for sensitive operations:

```python
async def _handle_tool_approval(self, tool_call: ToolCall) -> bool:
    if self._is_safe_tool(tool_call):
        return True

    # Prompt user
    self._ui.print_warning(f"Tool: {tool_call.name}")
    self._ui.print_info(f"Args: {tool_call.arguments}")
    response = await self._ui.get_input("Allow? [y/N]: ")

    return response.lower() == 'y'
```

---

## Issue 5: Error Recovery

### Problem
When a tool fails, the model may not handle it gracefully.

### Improvement
Add structured error handling:

```python
def format_tool_error(self, tool_call: ToolCall, error: str) -> Message:
    return Message(
        role="tool",
        content=f"""<tool_error>
Tool: {tool_call.name}
Error: {error}
Suggestion: Try a different approach or check the arguments.
</tool_error>""",
    )
```

---

## Implementation Priority

| Priority | Issue | Effort | Impact | Status |
|----------|-------|--------|--------|--------|
| 1 | Model calls tools repeatedly | Medium | High | **DONE** |
| 2 | Tool execution status | Low | Medium | **DONE** |
| 3 | Interactive tool approval | Medium | Medium | **DONE** |
| 4 | Better error recovery | Low | Medium | **DONE** |
| 5 | Native tool calling format | High | High | Deferred |

### Implementation Details

**Issue 1 - Repeated Tool Calls** (FIXED):
- Added explicit guidelines in system prompt: "NEVER call the same tool twice"
- Added duplicate detection in `engine.py:_check_duplicate_tool_call()`
- Tracks tool calls by name + arguments hash
- Returns cached result instead of re-executing

**Issue 2 - Tool Execution Status** (FIXED):
- Added `on_tool_start` and `on_tool_complete` callbacks
- UI shows: `Executing filesystem.list_directory(path='/workspace')...`
- UI shows: `Done filesystem.list_directory (45ms, 1024 chars)`

**Issue 3 - Interactive Tool Approval** (FIXED):
- Added async `prompt_tool_approval()` in TerminalUI
- Sensitive tools (bash, write, delete, git push) require approval when not in auto-approve mode
- Detects dangerous patterns in arguments (rm, sudo, etc.)

**Issue 4 - Error Recovery** (FIXED):
- Added `_create_error_message()` with structured error format
- Includes recovery suggestions for the model
- Prevents re-raising exceptions, allows model to adapt

---

## Testing Checklist

After implementing improvements:

- [ ] `entropi ask "list files"` calls tool once, presents result
- [ ] `entropi ask "read README.md"` reads file, shows content
- [ ] `entropi ask "what's the git status"` runs git.status, explains changes
- [ ] Tool errors are handled gracefully
- [ ] Sensitive tools prompt for approval (when not auto-approve)
- [ ] Status shown during long-running tools

---

## Related Files

- `src/entropi/core/engine.py` - Agentic loop, tool execution
- `src/entropi/inference/adapters/qwen.py` - Tool call parsing
- `src/entropi/inference/adapters/base.py` - Base adapter with format_system_prompt
- `src/entropi/mcp/manager.py` - Tool discovery and execution
- `src/entropi/core/context.py` - System prompt building
