Edit a file using one of two modes.

**Requirements:** You must read the file first.

**When NOT to use:**
- Don't use with old_string containing entire file content - use the write tool instead
- Don't use for major rewrites (>50% of file) - use the write tool instead
- If "no_match" error on large old_string, switch to the write tool

## STR_REPLACE Mode
Provide `old_string` + `new_string` for exact string replacement.

```json
{"path": "file.py", "old_string": "def hello():", "new_string": "def greet():"}
```

- `old_string` must match EXACTLY including whitespace
- If multiple matches, add more context or set `replace_all: true`

## INSERT Mode
Provide `insert_line` + `new_string` to insert at a line number.

```json
{"path": "file.py", "insert_line": 5, "new_string": "    # New comment"}
```

- `insert_line: 0` = insert at beginning of file
- `insert_line: N` = insert AFTER line N

## Response Format

**STR_REPLACE success:**
```json
{
  "success": true,
  "path": "file.py",
  "mode": "str_replace",
  "changes": [{"line": 45, "before": "def hello():", "after": "def greet():"}]
}
```

**INSERT success:**
```json
{
  "success": true,
  "path": "file.py",
  "mode": "insert",
  "changes": [{"line": 5, "inserted": "from pieces import Piece"}]
}
```

**Error:**
```json
{"error": "no_match", "message": "String not found", "debug": "...", "tip": "..."}
```

## Common Errors
| Error | Fix |
|-------|-----|
| `read_required` | Read the file first |
| `file_changed` | File modified externally, re-read it |
| `no_match` | Check debug info, try insert mode |
| `multiple_matches` | Add more context to old_string |
