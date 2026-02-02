Execute a shell command. **Preferred for exploration and discovery.**

**Parameters:**
- `command`: Shell command to execute (required)
- `timeout`: Maximum execution time in seconds (default: 30)

**Common Uses:**
- List files: `{"command": "ls -la"}` or `{"command": "ls *.py"}`
- Find files: `{"command": "find . -name '*.py' -type f"}`
- Check existence: `{"command": "test -f file.py && echo exists"}`
- Run tests: `{"command": "pytest tests/"}`
- Install packages: `{"command": "pip install requests"}`

**Path Notes:**
- Commands run from workspace root
- Use relative paths: `ls src/` not `ls /home/user/project/src/`

**Permissions:**
Commands are checked against config.local.yaml allow/deny patterns.
Denied commands will fail with permission error.

**When to use bash vs filesystem tools:**
- Bash: exploration, discovery, running commands, quick checks
- Filesystem: reading/editing files (provides structured output + safety)
