# Entropi Manual Test Plan

**Purpose:** Manual CLI testing of all implemented features
**Last Updated:** 2026-01-07

---

## Prerequisites

```bash
cd /home/tvanfossen/Projects/entropi

# Build and run via Docker
./install.sh

# Verify LSP servers are available in container
pyright --version
clangd --version
```

---

## Test Results Summary

| Feature | Tests | Passed | Failed | Notes |
|---------|-------|--------|--------|-------|
| Auto-Compaction | 0/2 | | | |
| LSP Integration (Python) | 0/4 | | | |
| LSP Integration (C) | 0/4 | | | |
| Sessions | 0/8 | | | |
| Tool Calling | 0/3 | | | |
| Agentic Todos | 0/2 | | | |
| Read-before-Write | 0/4 | | | |
| **Total** | **0/27** | | | |

---

## Auto-Compaction

### Test: Long Conversation Compaction

**Steps:**
1. Start entropi: `entropi`
2. Ask 5-6 substantial questions that generate long responses
3. After ~4000 tokens, compaction should trigger
4. Observe logs in `.entropi/session.log`

**Expected:**
- Earlier turns get compacted into summary
- Recent turns (last 2) preserved verbatim
- Conversation continues normally
- Log shows "Compacted X -> Y tokens"

**Result:** [ ] Pass  [ ] Fail
**Notes:**

---

### Test: Compaction Preserves Context

**Steps:**
1. Fresh session: "My name is TestUser and I'm working on Project Alpha"
2. Continue conversation until compaction triggers
3. After compaction: "What's my name and what project am I working on?"

**Expected:** Model remembers name and project from compacted context

**Result:** [ ] Pass  [ ] Fail
**Notes:**

---

## LSP Integration (Python)

### Test: Python Type Error Detection

**Setup:**
```bash
cat > /tmp/test_python_lsp.py << 'EOF'
def add_numbers(a: int, b: int) -> int:
    return a + b

result: str = add_numbers(1, 2)  # Type error
EOF
```

**Test:**
```bash
entropi ask "Check /tmp/test_python_lsp.py for type errors"
```

**Expected:** Identifies type mismatch on line 4

**Result:** [ ] Pass  [ ] Fail
**Notes:**

---

### Test: Python Undefined Variable

**Setup:**
```bash
cat > /tmp/test_undefined.py << 'EOF'
def process():
    x = undefined_variable + 1
    return x
EOF
```

**Test:**
```bash
entropi ask "What errors are in /tmp/test_undefined.py?"
```

**Expected:** Identifies `undefined_variable` is not defined

**Result:** [ ] Pass  [ ] Fail
**Notes:**

---

### Test: Python Multiple Files

**Setup:**
```bash
mkdir -p /tmp/multi_py_test
echo 'x: int = "not an int"' > /tmp/multi_py_test/a.py
echo 'from typing import List
items: List[str] = [1, 2, 3]' > /tmp/multi_py_test/b.py
```

**Test:**
```bash
entropi ask "Check all Python files in /tmp/multi_py_test for errors"
```

**Expected:** Reports errors in both a.py and b.py

**Result:** [ ] Pass  [ ] Fail
**Notes:**

---

### Test: Python Clean File

**Setup:**
```bash
cat > /tmp/clean_python.py << 'EOF'
def greet(name: str) -> str:
    return f"Hello, {name}!"

message = greet("World")
print(message)
EOF
```

**Test:**
```bash
entropi ask "Are there any errors in /tmp/clean_python.py?"
```

**Expected:** Reports no errors

**Result:** [ ] Pass  [ ] Fail
**Notes:**

---

## LSP Integration (C)

### Test: C Syntax Error

**Setup:**
```bash
cat > /tmp/test_c_syntax.c << 'EOF'
#include <stdio.h>

int main() {
    printf("Hello World\n")  // Missing semicolon
    return 0;
}
EOF
```

**Test:**
```bash
entropi ask "Check /tmp/test_c_syntax.c for errors"
```

**Expected:** Identifies missing semicolon

**Result:** [ ] Pass  [ ] Fail
**Notes:**

---

### Test: C Type Error

**Setup:**
```bash
cat > /tmp/test_c_type.c << 'EOF'
#include <stdio.h>

int main() {
    int x = "this is a string";
    printf("%d\n", x);
    return 0;
}
EOF
```

**Test:**
```bash
entropi ask "What type errors are in /tmp/test_c_type.c?"
```

**Expected:** Identifies incompatible pointer to integer conversion

**Result:** [ ] Pass  [ ] Fail
**Notes:**

---

### Test: C Undefined Function

**Setup:**
```bash
cat > /tmp/test_c_undef.c << 'EOF'
int main() {
    int result = undefined_function(5);
    return result;
}
EOF
```

**Test:**
```bash
entropi ask "Check /tmp/test_c_undef.c for problems"
```

**Expected:** Identifies `undefined_function` is not declared

**Result:** [ ] Pass  [ ] Fail
**Notes:**

---

### Test: C Clean File

**Setup:**
```bash
cat > /tmp/clean_c.c << 'EOF'
#include <stdio.h>

int add(int a, int b) {
    return a + b;
}

int main() {
    int result = add(3, 4);
    printf("Result: %d\n", result);
    return 0;
}
EOF
```

**Test:**
```bash
entropi ask "Does /tmp/clean_c.c have any errors?"
```

**Expected:** Reports no errors

**Result:** [ ] Pass  [ ] Fail
**Notes:**

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

**Expected:** Shows list of sessions (may be empty initially, or shows existing sessions)

**Result:** [ ] Pass  [ ] Fail
**Notes:**

---

### Test: Create New Session

**In interactive mode:**
```
/new TestSession1
```

**Expected:** Creates new session, switches to it

**Verify:**
```
/sessions
```

**Result:** [ ] Pass  [ ] Fail
**Notes:**

---

### Test: Session Isolation

**In interactive mode:**
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
**Notes:**

---

### Test: Switch Session

**In interactive mode:**
```
/sessions
/session <TestSession1-ID>
What's my secret code?
```

**Expected:** After switching to TestSession1, model SHOULD remember ALPHA-7749

**Result:** [ ] Pass  [ ] Fail
**Notes:**

---

### Test: Rename Session

**In interactive mode:**
```
/rename "My Renamed Session"
/sessions
```

**Expected:** Session shows with new name

**Result:** [ ] Pass  [ ] Fail
**Notes:**

---

### Test: Export Session

**In interactive mode:**
```
/export
```

**Expected:** Outputs session as markdown with all messages

**Result:** [ ] Pass  [ ] Fail
**Notes:**

---

### Test: Delete Session

**In interactive mode:**
```
/sessions
/delete <TestSession2-ID>
/sessions
```

**Expected:** TestSession2 no longer appears

**Result:** [ ] Pass  [ ] Fail
**Notes:**

---

### Test: Session Persistence

**Steps:**
1. Exit entropi (Ctrl+C or /exit)
2. Restart: `entropi`
3. Run `/sessions`

**Expected:** Previously created sessions still exist

**Result:** [ ] Pass  [ ] Fail
**Notes:**

---

## Tool Calling

### Test: List Directory

```bash
entropi ask "List all files in the src/entropi directory"
```

**Expected:** Returns directory listing using filesystem.list_directory tool

**Result:** [ ] Pass  [ ] Fail
**Notes:**

---

### Test: Read File

```bash
entropi ask "Read the pyproject.toml file and tell me the project name"
```

**Expected:** Reads file, extracts and reports project name

**Result:** [ ] Pass  [ ] Fail
**Notes:**

---

### Test: Search Files

```bash
entropi ask "Find all Python files in src/ that contain the word 'async'"
```

**Expected:** Uses search_files tool, returns matching files

**Result:** [ ] Pass  [ ] Fail
**Notes:**

---

## Agentic Todos

### Test: Multi-Step Task

```bash
entropi
```

**In interactive mode:**
```
List all Python files in src/entropi/mcp/, then read the first one and summarize what it does
```

**Expected:**
- Todo list appears showing planned steps
- Todos update as each step completes
- Final summary provided

**Result:** [ ] Pass  [ ] Fail
**Notes:**

---

### Test: Complex Task Planning

**In interactive mode:**
```
I want to understand how the config system works. Find the config files, read them, and explain the configuration options available.
```

**Expected:**
- Creates todo list with exploration steps
- Systematically reads config-related files
- Provides comprehensive explanation

**Result:** [ ] Pass  [ ] Fail
**Notes:**

---

## Read-before-Write

### Test: Write New File

**Setup:**
```bash
rm -f /tmp/entropi_new_file.txt
```

**Test:**
```bash
entropi ask "Create a new file at /tmp/entropi_new_file.txt with the content 'Hello from Entropi'"
```

**Expected:** File created successfully (new files don't require prior read)

**Verify:**
```bash
cat /tmp/entropi_new_file.txt
```

**Result:** [ ] Pass  [ ] Fail
**Notes:**

---

### Test: Edit Existing File

**Test:**
```bash
entropi ask "Change 'Hello' to 'Greetings' in /tmp/entropi_new_file.txt"
```

**Expected:**
- Model reads file first
- Then edits with string replacement
- File contains "Greetings from Entropi"

**Verify:**
```bash
cat /tmp/entropi_new_file.txt
```

**Result:** [ ] Pass  [ ] Fail
**Notes:**

---

### Test: Edit Multiple Matches

**Setup:**
```bash
echo "foo bar foo baz foo" > /tmp/multi_match.txt
```

**Test:**
```bash
entropi ask "Replace all occurrences of 'foo' with 'qux' in /tmp/multi_match.txt"
```

**Expected:** All 3 instances replaced

**Verify:**
```bash
cat /tmp/multi_match.txt
# Should show: qux bar qux baz qux
```

**Result:** [ ] Pass  [ ] Fail
**Notes:**

---

### Test: Write Protection

**Setup:**
```bash
echo "Original content" > /tmp/protected_file.txt
entropi
```

**In interactive mode:**
```
Overwrite /tmp/protected_file.txt with "New content" without reading it first. Do NOT read the file, just write to it directly.
```

**Expected:**
- Should fail because file wasn't read first
- Original content preserved

**Verify:**
```bash
cat /tmp/protected_file.txt
# Should still be: Original content
```

**Result:** [ ] Pass  [ ] Fail
**Notes:**

---

## Cleanup

```bash
rm -f /tmp/test_python_lsp.py /tmp/test_undefined.py
rm -rf /tmp/multi_py_test
rm -f /tmp/clean_python.py
rm -f /tmp/test_c_syntax.c /tmp/test_c_type.c /tmp/test_c_undef.c /tmp/clean_c.c
rm -f /tmp/entropi_new_file.txt /tmp/multi_match.txt /tmp/protected_file.txt
```

---

## Sign-off

**Tester:**
**Date:**
**Overall Result:** [ ] All Passed  [ ] Some Failed

**Issues Found:**

**Comments:**
