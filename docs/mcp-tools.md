# MCP Tools

> Built-in tools and how to use them

## Overview

Entropi uses the Model Context Protocol (MCP) for all tool integration. Tools are provided by MCP servers that run alongside the main application.

## Built-in Tools

### Filesystem Server

| Tool | Description | Parameters |
|------|-------------|------------|
| `filesystem.read_file` | Read file contents | `path` (required) |
| `filesystem.write_file` | Write content to file | `path`, `content` (required) |
| `filesystem.edit_file` | Edit file with string replacement | `path`, `old_string`, `new_string`, `replace_all` |
| `filesystem.list_directory` | List directory contents | `path` (required) |
| `filesystem.search_files` | Search for files by pattern | `path`, `pattern`, `recursive` |
| `filesystem.file_exists` | Check if file exists | `path` (required) |

### Bash Server

| Tool | Description | Parameters |
|------|-------------|------------|
| `bash.execute` | Execute shell command | `command` (required), `timeout` |

### Git Server

| Tool | Description | Parameters |
|------|-------------|------------|
| `git.status` | Show working tree status | - |
| `git.diff` | Show changes | `staged`, `path` |
| `git.log` | Show commit history | `count`, `oneline` |
| `git.add` | Stage files | `paths` (required) |
| `git.commit` | Create commit | `message` (required) |
| `git.branch` | List or create branches | `name`, `delete` |

## Tool Call Format

All tool calls use the `<tool_call>` tag format:

```xml
<tool_call>
{"name": "filesystem.read_file", "arguments": {"path": "src/main.py"}}
</tool_call>
```

## Examples

### Reading a File

```
User: What's in the README?

Entropi: I'll read the README file.
<tool_call>
{"name": "filesystem.read_file", "arguments": {"path": "README.md"}}
</tool_call>

[Tool returns file contents]

Entropi: The README contains...
```

### Listing a Directory

```
User: What files are in src/entropi?

Entropi: Let me list that directory.
<tool_call>
{"name": "filesystem.list_directory", "arguments": {"path": "src/entropi"}}
</tool_call>
```

### Editing a File

```
User: Change "Hello" to "Greetings" in message.txt

Entropi: I'll edit that file.
<tool_call>
{"name": "filesystem.edit_file", "arguments": {
  "path": "message.txt",
  "old_string": "Hello",
  "new_string": "Greetings",
  "replace_all": true
}}
</tool_call>
```

### Running a Command

```
User: Run the tests

Entropi: I'll run pytest.
<tool_call>
{"name": "bash.execute", "arguments": {"command": "pytest tests/ -v"}}
</tool_call>
```

### Git Operations

```
User: What's the git status?

Entropi: Let me check.
<tool_call>
{"name": "git.status", "arguments": {}}
</tool_call>
```

## Read-Before-Write Protection

For existing files, Entropi must read a file before writing to it. This prevents accidental overwrites and ensures the model understands the current content.

- **New files**: Can be created directly
- **Existing files**: Must be read first, then edited

## Permissions

Tool permissions can be configured in your config file:

```yaml
permissions:
  allow:
    - "filesystem.*"      # All filesystem operations
    - "git.status"        # Git status only
    - "git.diff"
  deny:
    - "bash.execute:rm -rf *"   # Block dangerous commands
    - "bash.execute:sudo *"
```

## Adding External MCP Servers

Entropi supports external MCP servers via configuration:

```yaml
mcp:
  external_servers:
    myserver:
      command: "python"
      args: ["-m", "myserver"]
```

External servers are discovered automatically and their tools become available to the model.
