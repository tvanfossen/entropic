Create a new file or fully overwrite an existing file.

**Requirements:**
- If file EXISTS: You must read it first with read_file
- If file is NEW: Can create directly

**When to use write_file:**
- Creating new files
- Major rewrites (changing >50% of file content)
- Splitting files: extract code to new file, rewrite original with remainder
- When edit_file fails with "no_match" on large strings

**File Splitting Pattern:**
1. `read_file` the source file
2. `write_file` to create the new file with extracted content
3. `write_file` to rewrite the original with remaining content

For partial modifications, use edit_file instead.

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
