Create a new file or fully overwrite an existing file.

**Requirements:**
- If file EXISTS: You must read it first
- If file is NEW: Can create directly

**When to use:**
- Creating new files
- Major rewrites (changing >50% of file content)
- Splitting files: extract code to new file, rewrite original with remainder
- When the edit tool fails with "no_match" on large strings

**File Splitting Pattern:**
1. Read the source file
2. Write the new file with extracted content
3. Rewrite the original with remaining content

For partial modifications, use the edit tool instead.

**Parameters:**
- `path`: File path to create/overwrite
- `content`: Full file content

**Success response:**
```json
{"success": true, "bytes_written": 1234, "path": "file.py"}
```

**Error responses:**
```json
{"error": "read_required", "message": "Cannot write: file must be read first"}
{"error": "file_changed", "message": "File modified since read. Read again first."}
```
