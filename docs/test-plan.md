# Entropi Test Plan

> Manual testing checklist for Entropi features

**Last Updated:** 2026-01-08

---

## Action Items

Before running these tests, complete the following:

1. [ ] **Build and install** - Run `./install.sh` or `pip install -e .`
2. [ ] **Configure models** - Ensure `~/.entropi/config.yaml` has valid model paths
3. [ ] **Test basic startup** - Run `entropi status` to verify configuration
4. [ ] **Run all tests below** - Mark each as Pass/Fail
5. [ ] **Document issues** - Note any failures in the Issues Found section

### Known Incomplete Features

The following features are still in planning/development:

- **LSP Integration** - Requires pyright and clangd installation
- **MCP Python Server** - Not yet implemented
- **MCP C Server** - Not yet implemented
- **Docker distribution** - Partially complete

---

## Prerequisites

```bash
cd /home/tvanfossen/Projects/entropi
source .venv/bin/activate

# Verify entropi works
entropi --version
entropi status
```

---

## Test Results Summary

| Feature | Tests | Passed | Failed | Notes |
|---------|-------|--------|--------|-------|
| Basic CLI | 0/3 | | | |
| Tool Calling | 0/3 | | | |
| Sessions | 0/8 | | | |
| Auto-Compaction | 0/2 | | | |
| Read-before-Write | 0/4 | | | |
| Agentic Todos | 0/2 | | | |
| **Total** | **0/22** | | | |

---

## Basic CLI

### Test: Version Command

```bash
entropi --version
```

**Expected:** Shows version number

**Result:** [ ] Pass  [ ] Fail

---

### Test: Status Command

```bash
entropi status
```

**Expected:** Shows model configuration and status

**Result:** [ ] Pass  [ ] Fail

---

### Test: Single Query

```bash
entropi ask "What is 2+2?"
```

**Expected:** Returns answer (should be "4")

**Result:** [ ] Pass  [ ] Fail

---

## Tool Calling

### Test: List Directory

```bash
entropi ask "List all files in the src/entropi directory"
```

**Expected:** Uses `filesystem.list_directory` tool, returns file listing

**Result:** [ ] Pass  [ ] Fail

---

### Test: Read File

```bash
entropi ask "Read the pyproject.toml file and tell me the project name"
```

**Expected:** Uses `filesystem.read_file` tool, reports project name as "entropi"

**Result:** [ ] Pass  [ ] Fail

---

### Test: Search Files

```bash
entropi ask "Find all Python files in src/ that contain the word 'async'"
```

**Expected:** Uses `filesystem.search_files` tool, returns matching files

**Result:** [ ] Pass  [ ] Fail

---

## Sessions

### Test: List Sessions

```bash
entropi
```

**In interactive mode:**
```
/sessions
```

**Expected:** Shows list of sessions (may be empty initially)

**Result:** [ ] Pass  [ ] Fail

---

### Test: Create New Session

```
/new TestSession1
```

**Expected:** Creates new session, switches to it

**Result:** [ ] Pass  [ ] Fail

---

### Test: Session Isolation

```
Remember this secret code: ALPHA-7749
```

Wait for response, then:

```
/new TestSession2
What's my secret code?
```

**Expected:** In TestSession2, model should NOT know the secret code

**Result:** [ ] Pass  [ ] Fail

---

### Test: Switch Session

```
/sessions
/session <TestSession1-ID>
What's my secret code?
```

**Expected:** After switching to TestSession1, model SHOULD remember ALPHA-7749

**Result:** [ ] Pass  [ ] Fail

---

### Test: Rename Session

```
/rename "My Renamed Session"
/sessions
```

**Expected:** Session shows with new name

**Result:** [ ] Pass  [ ] Fail

---

### Test: Export Session

```
/export
```

**Expected:** Outputs session as markdown

**Result:** [ ] Pass  [ ] Fail

---

### Test: Delete Session

```
/delete <TestSession2-ID>
/sessions
```

**Expected:** TestSession2 no longer appears

**Result:** [ ] Pass  [ ] Fail

---

### Test: Session Persistence

1. Exit entropi (Ctrl+C or /exit)
2. Restart: `entropi`
3. Run `/sessions`

**Expected:** Previously created sessions still exist

**Result:** [ ] Pass  [ ] Fail

---

## Auto-Compaction

### Test: Long Conversation Compaction

1. Start entropi: `entropi`
2. Ask 5-6 substantial questions that generate long responses
3. Observe logs in `.entropi/session.log`

**Expected:**
- After ~4000 tokens, compaction triggers
- Log shows "Compacted X -> Y tokens"
- Conversation continues normally

**Result:** [ ] Pass  [ ] Fail

---

### Test: Compaction Preserves Context

1. Fresh session: "My name is TestUser and I'm working on Project Alpha"
2. Continue conversation until compaction triggers
3. After compaction: "What's my name and what project am I working on?"

**Expected:** Model remembers name and project from compacted context

**Result:** [ ] Pass  [ ] Fail

---

## Read-before-Write

### Test: Write New File

```bash
rm -f /tmp/entropi_new_file.txt
entropi ask "Create a new file at /tmp/entropi_new_file.txt with the content 'Hello from Entropi'"
cat /tmp/entropi_new_file.txt
```

**Expected:** File created successfully (new files don't require prior read)

**Result:** [ ] Pass  [ ] Fail

---

### Test: Edit Existing File

```bash
entropi ask "Change 'Hello' to 'Greetings' in /tmp/entropi_new_file.txt"
cat /tmp/entropi_new_file.txt
```

**Expected:**
- Model reads file first
- Then edits with string replacement
- File contains "Greetings from Entropi"

**Result:** [ ] Pass  [ ] Fail

---

### Test: Edit Multiple Matches

```bash
echo "foo bar foo baz foo" > /tmp/multi_match.txt
entropi ask "Replace all occurrences of 'foo' with 'qux' in /tmp/multi_match.txt"
cat /tmp/multi_match.txt
```

**Expected:** All 3 instances replaced: "qux bar qux baz qux"

**Result:** [ ] Pass  [ ] Fail

---

### Test: Write Protection

```bash
echo "Original content" > /tmp/protected_file.txt
entropi ask "Overwrite /tmp/protected_file.txt with 'New content' without reading it first"
cat /tmp/protected_file.txt
```

**Expected:**
- Write fails because file wasn't read first
- Original content preserved

**Result:** [ ] Pass  [ ] Fail

---

## Agentic Todos

### Test: Multi-Step Task

In interactive mode:

```
List all Python files in src/entropi/mcp/, then read the first one and summarize what it does
```

**Expected:**
- Todo list appears showing planned steps
- Todos update as each step completes
- Final summary provided

**Result:** [ ] Pass  [ ] Fail

---

### Test: Complex Task Planning

```
I want to understand how the config system works. Find the config files, read them, and explain the configuration options available.
```

**Expected:**
- Creates todo list with exploration steps
- Systematically reads config-related files
- Provides comprehensive explanation

**Result:** [ ] Pass  [ ] Fail

---

## Cleanup

```bash
rm -f /tmp/entropi_new_file.txt /tmp/multi_match.txt /tmp/protected_file.txt
```

---

## Sign-off

| Field | Value |
|-------|-------|
| **Tester** | |
| **Date** | |
| **Overall Result** | [ ] All Passed  [ ] Some Failed |

---

## Issues Found

*Document any test failures or unexpected behavior here:*

1.
2.
3.

---

## Notes

*Additional observations or comments:*
