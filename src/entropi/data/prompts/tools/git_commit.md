Create a git commit.

**Parameters:**
- `message`: Commit message (required)
- `all`: If true, stage all modified files before committing

**Examples:**
- Commit staged files: `{"message": "Fix bug in parser"}`
- Stage and commit all: `{"message": "Update docs", "all": true}`

**Note:** Files must be staged first (use git.add) unless using `all: true`.
