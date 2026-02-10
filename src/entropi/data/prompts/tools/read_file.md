Read a file's contents. Returns JSON with line numbers as keys.

You MUST read a file before editing or overwriting.

**Response format:**
```json
{"path": "...", "total": N, "lines": {"1": "line content", "2": "...", ...}}
```

**Error format:**
```json
{"error": "not_found", "message": "File not found: path"}
```

Use line numbers with the edit tool's insert mode when string matching fails.
