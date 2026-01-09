# MCP Server: C Development Tools

> Dedicated C compilation, execution, and debugging tools

**Status:** Ready for Implementation
**Priority:** High (reduces error rate vs bash routing)
**Complexity:** Medium
**Dependencies:** MCP client infrastructure, LSP integration (recommended)

---

## Problem Statement

Currently, C development operations route through `bash.execute`:

```bash
bash.execute("gcc -o main main.c")
bash.execute("./main")
bash.execute("make clean && make")
bash.execute("valgrind ./main")
```

This causes issues:

1. **Complex flag management**: Compiler flags, warnings, optimization levels
2. **Error parsing**: GCC/Clang errors mixed with shell output
3. **Build system confusion**: Make vs CMake vs manual compilation
4. **No memory safety**: Easy to forget valgrind/sanitizers
5. **Path escaping**: Spaces and special characters in paths
6. **Missing context**: Model doesn't know available targets or compiler

A dedicated C MCP server provides:
- **Structured compilation**: Clean API for gcc/clang with sensible defaults
- **Parsed errors**: Line numbers, error types, suggestions extracted
- **Build system detection**: Auto-detect Make/CMake and use appropriately
- **Memory checking**: Built-in valgrind/sanitizer integration
- **LSP integration**: Feed diagnostics from clangd when available

---

## Prerequisites

**Note:** This MCP server is most effective when combined with LSP integration (doc 15). The LSP server (clangd) provides real-time diagnostics, while this MCP server handles compilation and execution. Together they provide a complete C development experience.

```
┌─────────────────────────────────────────────────────────────────┐
│                    Development Flow                              │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│   1. Edit Code                                                   │
│        │                                                         │
│        ▼                                                         │
│   2. LSP (clangd) ──► Real-time errors, warnings, suggestions   │
│        │                                                         │
│        ▼                                                         │
│   3. C MCP Server ──► Compile, run, test, debug                 │
│        │                                                         │
│        ▼                                                         │
│   4. Results ──► Parsed output fed back to model                │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                       C MCP Server                               │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  Tools:                                                          │
│  ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐  │
│  │ c.compile       │  │ c.run           │  │ c.build         │  │
│  │                 │  │                 │  │                 │  │
│  │ Compile source  │  │ Execute binary  │  │ Make/CMake      │  │
│  │ files           │  │ with args       │  │ build           │  │
│  └────────┬────────┘  └────────┬────────┘  └────────┬────────┘  │
│           │                    │                    │            │
│  ┌─────────────────┐  ┌─────────────────┐                       │
│  │ c.check         │  │ c.debug         │                       │
│  │                 │  │                 │                       │
│  │ Valgrind/ASAN   │  │ GDB commands    │                       │
│  │ memory check    │  │ (future)        │                       │
│  └────────┬────────┘  └────────┬────────┘                       │
│           │                    │                                 │
│           └────────────────────┼─────────────────────────────────┤
│                                │                                 │
│                    ┌───────────▼───────────┐                    │
│                    │   BuildManager        │                    │
│                    │                       │                    │
│                    │ - Detect build system │                    │
│                    │ - Parse compiler out  │                    │
│                    │ - Track artifacts     │                    │
│                    └───────────────────────┘                    │
│                                                                  │
└──────────────────────────────────────────────────────────────────┘
                                 │
                                 ▼
              ┌──────────────────────────────────┐
              │   gcc / clang / make / cmake     │
              └──────────────────────────────────┘
```

---

## Tools

### 1. c.compile

Compile C source files.

```python
{
    "name": "c.compile",
    "description": "Compile C source files. Uses gcc by default with sensible warning flags.",
    "inputSchema": {
        "type": "object",
        "properties": {
            "sources": {
                "type": "array",
                "items": {"type": "string"},
                "description": "Source files to compile"
            },
            "output": {
                "type": "string",
                "description": "Output binary name (default: derived from first source)"
            },
            "compiler": {
                "type": "string",
                "enum": ["gcc", "clang"],
                "description": "Compiler to use (default: gcc)",
                "default": "gcc"
            },
            "standard": {
                "type": "string",
                "enum": ["c99", "c11", "c17", "c23", "gnu99", "gnu11", "gnu17"],
                "description": "C standard (default: c17)",
                "default": "c17"
            },
            "optimization": {
                "type": "string",
                "enum": ["0", "1", "2", "3", "s", "g"],
                "description": "Optimization level (default: g for debug)",
                "default": "g"
            },
            "warnings": {
                "type": "string",
                "enum": ["none", "default", "all", "extra", "pedantic"],
                "description": "Warning level (default: all)",
                "default": "all"
            },
            "debug": {
                "type": "boolean",
                "description": "Include debug symbols (default: true)",
                "default": true
            },
            "sanitizers": {
                "type": "array",
                "items": {"type": "string", "enum": ["address", "undefined", "thread", "memory"]},
                "description": "Enable sanitizers (ASAN, UBSAN, etc.)"
            },
            "include_dirs": {
                "type": "array",
                "items": {"type": "string"},
                "description": "Additional include directories"
            },
            "libraries": {
                "type": "array",
                "items": {"type": "string"},
                "description": "Libraries to link (e.g., ['m', 'pthread'])"
            },
            "defines": {
                "type": "array",
                "items": {"type": "string"},
                "description": "Preprocessor definitions (e.g., ['DEBUG', 'VERSION=1'])"
            },
            "extra_flags": {
                "type": "array",
                "items": {"type": "string"},
                "description": "Additional compiler flags"
            }
        },
        "required": ["sources"]
    }
}
```

**Examples:**

```json
// Simple compilation
{"name": "c.compile", "arguments": {"sources": ["main.c"]}}

// Multiple files with output name
{"name": "c.compile", "arguments": {"sources": ["main.c", "utils.c"], "output": "myapp"}}

// With sanitizers for debugging
{"name": "c.compile", "arguments": {
    "sources": ["main.c"],
    "sanitizers": ["address", "undefined"],
    "optimization": "g"
}}

// Release build
{"name": "c.compile", "arguments": {
    "sources": ["main.c"],
    "optimization": "2",
    "debug": false,
    "warnings": "extra"
}}

// With libraries
{"name": "c.compile", "arguments": {
    "sources": ["main.c"],
    "libraries": ["m", "pthread"],
    "defines": ["_GNU_SOURCE"]
}}
```

**Return Format:**

```json
{
    "success": true,
    "output": "main",
    "compiler": "gcc",
    "command": "gcc -std=c17 -Og -g -Wall main.c -o main",
    "warnings": [],
    "errors": [],
    "duration_ms": 234
}
```

**Error Return Format:**

```json
{
    "success": false,
    "output": null,
    "compiler": "gcc",
    "command": "gcc -std=c17 -Og -g -Wall main.c -o main",
    "warnings": [
        {"file": "main.c", "line": 10, "column": 5, "type": "warning", "message": "unused variable 'x'", "code": "-Wunused-variable"}
    ],
    "errors": [
        {"file": "main.c", "line": 15, "column": 12, "type": "error", "message": "expected ';' before 'return'"}
    ],
    "duration_ms": 156
}
```

---

### 2. c.run

Execute a compiled binary.

```python
{
    "name": "c.run",
    "description": "Execute a compiled C binary with optional arguments.",
    "inputSchema": {
        "type": "object",
        "properties": {
            "binary": {
                "type": "string",
                "description": "Path to binary (default: ./main or last compiled)"
            },
            "args": {
                "type": "array",
                "items": {"type": "string"},
                "description": "Command line arguments"
            },
            "stdin": {
                "type": "string",
                "description": "Input to provide via stdin"
            },
            "timeout": {
                "type": "integer",
                "description": "Execution timeout in seconds (default: 30)",
                "default": 30
            },
            "env": {
                "type": "object",
                "description": "Environment variables to set"
            }
        }
    }
}
```

**Examples:**

```json
// Run default binary
{"name": "c.run", "arguments": {}}

// Run with arguments
{"name": "c.run", "arguments": {"binary": "./myapp", "args": ["--verbose", "input.txt"]}}

// Run with stdin input
{"name": "c.run", "arguments": {"binary": "./calculator", "stdin": "5 + 3\n"}}

// Run with timeout
{"name": "c.run", "arguments": {"binary": "./server", "timeout": 5}}
```

**Return Format:**

```json
{
    "success": true,
    "exit_code": 0,
    "stdout": "Hello, World!\n",
    "stderr": "",
    "duration_ms": 12,
    "signal": null
}
```

**Crash Return Format:**

```json
{
    "success": false,
    "exit_code": -11,
    "stdout": "",
    "stderr": "",
    "duration_ms": 5,
    "signal": "SIGSEGV",
    "crash_info": "Segmentation fault - likely null pointer dereference or buffer overflow"
}
```

---

### 3. c.build

Run build system (Make or CMake).

```python
{
    "name": "c.build",
    "description": "Build project using detected or specified build system (Make/CMake).",
    "inputSchema": {
        "type": "object",
        "properties": {
            "system": {
                "type": "string",
                "enum": ["auto", "make", "cmake"],
                "description": "Build system (default: auto-detect)",
                "default": "auto"
            },
            "target": {
                "type": "string",
                "description": "Build target (default: all)"
            },
            "clean": {
                "type": "boolean",
                "description": "Clean before building",
                "default": false
            },
            "jobs": {
                "type": "integer",
                "description": "Parallel jobs (default: auto)",
                "default": 0
            },
            "build_type": {
                "type": "string",
                "enum": ["Debug", "Release", "RelWithDebInfo", "MinSizeRel"],
                "description": "CMake build type (default: Debug)",
                "default": "Debug"
            },
            "cmake_args": {
                "type": "array",
                "items": {"type": "string"},
                "description": "Additional CMake arguments"
            },
            "make_args": {
                "type": "array",
                "items": {"type": "string"},
                "description": "Additional Make arguments"
            }
        }
    }
}
```

**Examples:**

```json
// Auto-detect and build
{"name": "c.build", "arguments": {}}

// Build specific target
{"name": "c.build", "arguments": {"target": "tests"}}

// Clean build
{"name": "c.build", "arguments": {"clean": true}}

// CMake release build
{"name": "c.build", "arguments": {"system": "cmake", "build_type": "Release"}}

// Parallel build
{"name": "c.build", "arguments": {"jobs": 8}}
```

**Return Format:**

```json
{
    "success": true,
    "system": "make",
    "target": "all",
    "artifacts": ["main", "libutils.a"],
    "warnings": [],
    "errors": [],
    "duration_ms": 1234
}
```

---

### 4. c.check

Run memory checkers and sanitizers.

```python
{
    "name": "c.check",
    "description": "Run memory checking tools (valgrind, sanitizers) on a binary.",
    "inputSchema": {
        "type": "object",
        "properties": {
            "binary": {
                "type": "string",
                "description": "Path to binary to check"
            },
            "tool": {
                "type": "string",
                "enum": ["valgrind", "valgrind-full", "helgrind", "drd"],
                "description": "Checking tool (default: valgrind)",
                "default": "valgrind"
            },
            "args": {
                "type": "array",
                "items": {"type": "string"},
                "description": "Arguments to pass to binary"
            },
            "stdin": {
                "type": "string",
                "description": "Input to provide via stdin"
            },
            "timeout": {
                "type": "integer",
                "description": "Timeout in seconds (default: 60)",
                "default": 60
            }
        },
        "required": ["binary"]
    }
}
```

**Examples:**

```json
// Basic memory check
{"name": "c.check", "arguments": {"binary": "./main"}}

// Full valgrind check
{"name": "c.check", "arguments": {"binary": "./main", "tool": "valgrind-full"}}

// Thread checking
{"name": "c.check", "arguments": {"binary": "./server", "tool": "helgrind"}}

// With input
{"name": "c.check", "arguments": {"binary": "./parser", "stdin": "test input"}}
```

**Return Format:**

```json
{
    "success": true,
    "tool": "valgrind",
    "exit_code": 0,
    "stdout": "Program output...",
    "memory_errors": 0,
    "memory_leaks": {
        "definitely_lost": 0,
        "indirectly_lost": 0,
        "possibly_lost": 0,
        "still_reachable": 1024
    },
    "issues": [],
    "duration_ms": 5432
}
```

**Error Return Format:**

```json
{
    "success": false,
    "tool": "valgrind",
    "exit_code": 1,
    "stdout": "",
    "memory_errors": 2,
    "memory_leaks": {
        "definitely_lost": 128,
        "indirectly_lost": 0,
        "possibly_lost": 0,
        "still_reachable": 0
    },
    "issues": [
        {
            "type": "Invalid read",
            "size": 4,
            "address": "0x1234",
            "location": {"file": "main.c", "line": 25, "function": "process_data"},
            "stack": ["process_data (main.c:25)", "main (main.c:10)"]
        },
        {
            "type": "Memory leak",
            "size": 128,
            "location": {"file": "main.c", "line": 15, "function": "allocate_buffer"},
            "stack": ["allocate_buffer (main.c:15)", "main (main.c:8)"]
        }
    ],
    "duration_ms": 4521
}
```

---

## Implementation

### CompilerManager

```python
"""
Compiler management for C MCP server.
"""
import asyncio
import re
import shutil
from dataclasses import dataclass, field
from pathlib import Path

from entropi.core.logging import get_logger

logger = get_logger("mcp.c.compiler")


@dataclass
class CompilerDiagnostic:
    """A compiler warning or error."""

    file: str
    line: int
    column: int
    type: str  # "error", "warning", "note"
    message: str
    code: str | None = None  # e.g., "-Wunused-variable"


@dataclass
class CompileResult:
    """Result of compilation."""

    success: bool
    output: str | None
    compiler: str
    command: str
    warnings: list[CompilerDiagnostic] = field(default_factory=list)
    errors: list[CompilerDiagnostic] = field(default_factory=list)
    duration_ms: int = 0


class CompilerManager:
    """
    Manages C compilation.

    Provides sensible defaults and parses compiler output.
    """

    WARNING_LEVELS = {
        "none": [],
        "default": [],
        "all": ["-Wall"],
        "extra": ["-Wall", "-Wextra"],
        "pedantic": ["-Wall", "-Wextra", "-Wpedantic"],
    }

    SANITIZER_FLAGS = {
        "address": "-fsanitize=address",
        "undefined": "-fsanitize=undefined",
        "thread": "-fsanitize=thread",
        "memory": "-fsanitize=memory",
    }

    # Regex for parsing GCC/Clang output
    DIAGNOSTIC_PATTERN = re.compile(
        r"^(?P<file>[^:]+):(?P<line>\d+):(?P<column>\d+): "
        r"(?P<type>error|warning|note): (?P<message>.+?)(?:\s+\[(?P<code>[^\]]+)\])?$"
    )

    def __init__(self, project_dir: Path) -> None:
        self.project_dir = project_dir
        self._last_output: str | None = None

    def _find_compiler(self, preferred: str) -> str:
        """Find compiler executable."""
        if shutil.which(preferred):
            return preferred
        # Fallback
        for compiler in ["gcc", "clang", "cc"]:
            if shutil.which(compiler):
                return compiler
        raise RuntimeError("No C compiler found")

    async def compile(
        self,
        sources: list[str],
        output: str | None = None,
        compiler: str = "gcc",
        standard: str = "c17",
        optimization: str = "g",
        warnings: str = "all",
        debug: bool = True,
        sanitizers: list[str] | None = None,
        include_dirs: list[str] | None = None,
        libraries: list[str] | None = None,
        defines: list[str] | None = None,
        extra_flags: list[str] | None = None,
    ) -> CompileResult:
        """Compile C source files."""
        import time
        start = time.time()

        # Find compiler
        compiler_path = self._find_compiler(compiler)

        # Build output name
        if output is None:
            first_source = Path(sources[0])
            output = first_source.stem

        # Build command
        cmd = [compiler_path]

        # Standard
        cmd.append(f"-std={standard}")

        # Optimization
        cmd.append(f"-O{optimization}")

        # Debug symbols
        if debug:
            cmd.append("-g")

        # Warnings
        cmd.extend(self.WARNING_LEVELS.get(warnings, []))

        # Sanitizers
        if sanitizers:
            for san in sanitizers:
                if san in self.SANITIZER_FLAGS:
                    cmd.append(self.SANITIZER_FLAGS[san])

        # Include directories
        if include_dirs:
            for inc in include_dirs:
                cmd.extend(["-I", inc])

        # Defines
        if defines:
            for d in defines:
                cmd.append(f"-D{d}")

        # Extra flags
        if extra_flags:
            cmd.extend(extra_flags)

        # Source files
        cmd.extend(sources)

        # Output
        cmd.extend(["-o", output])

        # Libraries (must come after sources)
        if libraries:
            for lib in libraries:
                cmd.append(f"-l{lib}")

        # Run compilation
        logger.info(f"Compiling: {' '.join(cmd)}")

        proc = await asyncio.create_subprocess_exec(
            *cmd,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
            cwd=self.project_dir,
        )
        stdout, stderr = await proc.communicate()

        duration_ms = int((time.time() - start) * 1000)

        # Parse output
        warnings_list, errors_list = self._parse_diagnostics(stderr.decode())

        success = proc.returncode == 0
        if success:
            self._last_output = output

        return CompileResult(
            success=success,
            output=output if success else None,
            compiler=compiler_path,
            command=" ".join(cmd),
            warnings=warnings_list,
            errors=errors_list,
            duration_ms=duration_ms,
        )

    def _parse_diagnostics(self, output: str) -> tuple[list[CompilerDiagnostic], list[CompilerDiagnostic]]:
        """Parse compiler output into structured diagnostics."""
        warnings = []
        errors = []

        for line in output.split("\n"):
            match = self.DIAGNOSTIC_PATTERN.match(line)
            if match:
                diag = CompilerDiagnostic(
                    file=match.group("file"),
                    line=int(match.group("line")),
                    column=int(match.group("column")),
                    type=match.group("type"),
                    message=match.group("message"),
                    code=match.group("code"),
                )
                if diag.type == "error":
                    errors.append(diag)
                elif diag.type == "warning":
                    warnings.append(diag)

        return warnings, errors

    @property
    def last_output(self) -> str | None:
        """Get the last compiled binary name."""
        return self._last_output
```

### ExecutionManager

```python
"""
Binary execution for C MCP server.
"""
import asyncio
import signal
from dataclasses import dataclass
from pathlib import Path

from entropi.core.logging import get_logger

logger = get_logger("mcp.c.execution")


SIGNAL_NAMES = {
    signal.SIGSEGV: ("SIGSEGV", "Segmentation fault - likely null pointer dereference or buffer overflow"),
    signal.SIGABRT: ("SIGABRT", "Aborted - likely assertion failure or abort() call"),
    signal.SIGFPE: ("SIGFPE", "Floating point exception - division by zero or overflow"),
    signal.SIGBUS: ("SIGBUS", "Bus error - misaligned memory access"),
    signal.SIGILL: ("SIGILL", "Illegal instruction"),
    signal.SIGKILL: ("SIGKILL", "Killed"),
    signal.SIGTERM: ("SIGTERM", "Terminated"),
}


@dataclass
class ExecutionResult:
    """Result of binary execution."""

    success: bool
    exit_code: int
    stdout: str
    stderr: str
    duration_ms: int
    signal_name: str | None = None
    crash_info: str | None = None


class ExecutionManager:
    """
    Executes compiled C binaries.
    """

    DEFAULT_TIMEOUT = 30

    def __init__(self, project_dir: Path, compiler_manager: "CompilerManager") -> None:
        self.project_dir = project_dir
        self.compiler_manager = compiler_manager

    async def run(
        self,
        binary: str | None = None,
        args: list[str] | None = None,
        stdin_data: str | None = None,
        timeout: int = DEFAULT_TIMEOUT,
        env: dict[str, str] | None = None,
    ) -> ExecutionResult:
        """Run a compiled binary."""
        import os
        import time

        # Determine binary path
        if binary is None:
            binary = self.compiler_manager.last_output
            if binary is None:
                return ExecutionResult(
                    success=False,
                    exit_code=1,
                    stdout="",
                    stderr="No binary specified and none compiled yet",
                    duration_ms=0,
                )

        # Make path absolute if relative
        binary_path = self.project_dir / binary if not binary.startswith("/") else Path(binary)

        if not binary_path.exists():
            return ExecutionResult(
                success=False,
                exit_code=1,
                stdout="",
                stderr=f"Binary not found: {binary_path}",
                duration_ms=0,
            )

        # Build command
        cmd = [str(binary_path)] + (args or [])

        # Environment
        run_env = os.environ.copy()
        if env:
            run_env.update(env)

        start = time.time()

        try:
            proc = await asyncio.create_subprocess_exec(
                *cmd,
                stdin=asyncio.subprocess.PIPE if stdin_data else None,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE,
                cwd=self.project_dir,
                env=run_env,
            )

            stdin_bytes = stdin_data.encode() if stdin_data else None
            stdout, stderr = await asyncio.wait_for(
                proc.communicate(stdin_bytes),
                timeout=timeout,
            )

            duration_ms = int((time.time() - start) * 1000)

            # Check for signal
            exit_code = proc.returncode or 0
            signal_name = None
            crash_info = None

            if exit_code < 0:
                sig = -exit_code
                if sig in SIGNAL_NAMES:
                    signal_name, crash_info = SIGNAL_NAMES[sig]
                else:
                    signal_name = f"Signal {sig}"
                    crash_info = f"Process terminated by signal {sig}"

            return ExecutionResult(
                success=exit_code == 0,
                exit_code=exit_code,
                stdout=stdout.decode(),
                stderr=stderr.decode(),
                duration_ms=duration_ms,
                signal_name=signal_name,
                crash_info=crash_info,
            )

        except asyncio.TimeoutError:
            duration_ms = int((time.time() - start) * 1000)
            return ExecutionResult(
                success=False,
                exit_code=-1,
                stdout="",
                stderr=f"Execution timed out after {timeout} seconds",
                duration_ms=duration_ms,
            )
```

### BuildManager

```python
"""
Build system management for C MCP server.
"""
import asyncio
import os
from dataclasses import dataclass, field
from pathlib import Path

from entropi.core.logging import get_logger

logger = get_logger("mcp.c.build")


@dataclass
class BuildResult:
    """Result of build operation."""

    success: bool
    system: str
    target: str
    artifacts: list[str] = field(default_factory=list)
    warnings: list[dict] = field(default_factory=list)
    errors: list[dict] = field(default_factory=list)
    duration_ms: int = 0
    output: str = ""


class BuildManager:
    """
    Manages build systems (Make, CMake).
    """

    def __init__(self, project_dir: Path) -> None:
        self.project_dir = project_dir
        self._build_dir = project_dir / "build"

    def detect_build_system(self) -> str | None:
        """Detect which build system is used."""
        if (self.project_dir / "CMakeLists.txt").exists():
            return "cmake"
        if (self.project_dir / "Makefile").exists():
            return "make"
        if (self.project_dir / "makefile").exists():
            return "make"
        return None

    async def build(
        self,
        system: str = "auto",
        target: str | None = None,
        clean: bool = False,
        jobs: int = 0,
        build_type: str = "Debug",
        cmake_args: list[str] | None = None,
        make_args: list[str] | None = None,
    ) -> BuildResult:
        """Run build system."""
        import time

        # Auto-detect if needed
        if system == "auto":
            detected = self.detect_build_system()
            if detected is None:
                return BuildResult(
                    success=False,
                    system="none",
                    target=target or "all",
                    output="No build system detected (no Makefile or CMakeLists.txt found)",
                )
            system = detected

        start = time.time()

        if system == "cmake":
            result = await self._build_cmake(target, clean, jobs, build_type, cmake_args)
        else:
            result = await self._build_make(target, clean, jobs, make_args)

        result.duration_ms = int((time.time() - start) * 1000)
        return result

    async def _build_cmake(
        self,
        target: str | None,
        clean: bool,
        jobs: int,
        build_type: str,
        cmake_args: list[str] | None,
    ) -> BuildResult:
        """Build with CMake."""
        # Ensure build directory exists
        self._build_dir.mkdir(exist_ok=True)

        # Clean if requested
        if clean and self._build_dir.exists():
            import shutil
            shutil.rmtree(self._build_dir)
            self._build_dir.mkdir()

        # Configure
        config_cmd = [
            "cmake",
            f"-DCMAKE_BUILD_TYPE={build_type}",
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        ]
        if cmake_args:
            config_cmd.extend(cmake_args)
        config_cmd.append(str(self.project_dir))

        proc = await asyncio.create_subprocess_exec(
            *config_cmd,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
            cwd=self._build_dir,
        )
        stdout, stderr = await proc.communicate()

        if proc.returncode != 0:
            return BuildResult(
                success=False,
                system="cmake",
                target=target or "all",
                output=stderr.decode(),
            )

        # Build
        build_cmd = ["cmake", "--build", "."]
        if target:
            build_cmd.extend(["--target", target])
        if jobs > 0:
            build_cmd.extend(["-j", str(jobs)])
        else:
            build_cmd.extend(["-j", str(os.cpu_count() or 4)])

        proc = await asyncio.create_subprocess_exec(
            *build_cmd,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
            cwd=self._build_dir,
        )
        stdout, stderr = await proc.communicate()

        # Find artifacts
        artifacts = [str(f.relative_to(self._build_dir)) for f in self._build_dir.glob("*") if f.is_file() and os.access(f, os.X_OK)]

        return BuildResult(
            success=proc.returncode == 0,
            system="cmake",
            target=target or "all",
            artifacts=artifacts,
            output=stdout.decode() + stderr.decode(),
        )

    async def _build_make(
        self,
        target: str | None,
        clean: bool,
        jobs: int,
        make_args: list[str] | None,
    ) -> BuildResult:
        """Build with Make."""
        # Clean if requested
        if clean:
            proc = await asyncio.create_subprocess_exec(
                "make", "clean",
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE,
                cwd=self.project_dir,
            )
            await proc.communicate()

        # Build
        cmd = ["make"]
        if target:
            cmd.append(target)
        if jobs > 0:
            cmd.extend(["-j", str(jobs)])
        else:
            cmd.extend(["-j", str(os.cpu_count() or 4)])
        if make_args:
            cmd.extend(make_args)

        proc = await asyncio.create_subprocess_exec(
            *cmd,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
            cwd=self.project_dir,
        )
        stdout, stderr = await proc.communicate()

        # Find artifacts (executables in current directory)
        artifacts = [f.name for f in self.project_dir.glob("*") if f.is_file() and os.access(f, os.X_OK) and not f.name.startswith(".")]

        return BuildResult(
            success=proc.returncode == 0,
            system="make",
            target=target or "all",
            artifacts=artifacts,
            output=stdout.decode() + stderr.decode(),
        )
```

### MemoryChecker

```python
"""
Memory checking tools for C MCP server.
"""
import asyncio
import re
from dataclasses import dataclass, field
from pathlib import Path

from entropi.core.logging import get_logger

logger = get_logger("mcp.c.memcheck")


@dataclass
class MemoryIssue:
    """A memory-related issue found by valgrind."""

    type: str
    size: int | None
    address: str | None
    location: dict | None
    stack: list[str] = field(default_factory=list)


@dataclass
class MemoryLeaks:
    """Memory leak summary."""

    definitely_lost: int = 0
    indirectly_lost: int = 0
    possibly_lost: int = 0
    still_reachable: int = 0


@dataclass
class CheckResult:
    """Result of memory check."""

    success: bool
    tool: str
    exit_code: int
    stdout: str
    memory_errors: int = 0
    memory_leaks: MemoryLeaks = field(default_factory=MemoryLeaks)
    issues: list[MemoryIssue] = field(default_factory=list)
    duration_ms: int = 0


class MemoryChecker:
    """
    Runs memory checking tools (valgrind).
    """

    VALGRIND_TOOLS = {
        "valgrind": ["--leak-check=summary", "--error-exitcode=1"],
        "valgrind-full": ["--leak-check=full", "--show-leak-kinds=all", "--track-origins=yes", "--error-exitcode=1"],
        "helgrind": ["--tool=helgrind", "--error-exitcode=1"],
        "drd": ["--tool=drd", "--error-exitcode=1"],
    }

    def __init__(self, project_dir: Path) -> None:
        self.project_dir = project_dir

    async def check(
        self,
        binary: str,
        tool: str = "valgrind",
        args: list[str] | None = None,
        stdin_data: str | None = None,
        timeout: int = 60,
    ) -> CheckResult:
        """Run memory checker on binary."""
        import shutil
        import time

        if not shutil.which("valgrind"):
            return CheckResult(
                success=False,
                tool=tool,
                exit_code=1,
                stdout="",
                issues=[MemoryIssue(type="error", size=None, address=None, location=None, stack=["valgrind not found"])],
            )

        binary_path = self.project_dir / binary if not binary.startswith("/") else Path(binary)

        if not binary_path.exists():
            return CheckResult(
                success=False,
                tool=tool,
                exit_code=1,
                stdout="",
                issues=[MemoryIssue(type="error", size=None, address=None, location=None, stack=[f"Binary not found: {binary}"])],
            )

        # Build valgrind command
        valgrind_args = self.VALGRIND_TOOLS.get(tool, self.VALGRIND_TOOLS["valgrind"])
        cmd = ["valgrind"] + valgrind_args + [str(binary_path)] + (args or [])

        start = time.time()

        try:
            proc = await asyncio.create_subprocess_exec(
                *cmd,
                stdin=asyncio.subprocess.PIPE if stdin_data else None,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE,
                cwd=self.project_dir,
            )

            stdin_bytes = stdin_data.encode() if stdin_data else None
            stdout, stderr = await asyncio.wait_for(
                proc.communicate(stdin_bytes),
                timeout=timeout,
            )

            duration_ms = int((time.time() - start) * 1000)

            # Parse valgrind output
            issues, leaks, error_count = self._parse_valgrind(stderr.decode())

            return CheckResult(
                success=proc.returncode == 0 and error_count == 0,
                tool=tool,
                exit_code=proc.returncode or 0,
                stdout=stdout.decode(),
                memory_errors=error_count,
                memory_leaks=leaks,
                issues=issues,
                duration_ms=duration_ms,
            )

        except asyncio.TimeoutError:
            duration_ms = int((time.time() - start) * 1000)
            return CheckResult(
                success=False,
                tool=tool,
                exit_code=-1,
                stdout="",
                issues=[MemoryIssue(type="timeout", size=None, address=None, location=None, stack=[f"Timeout after {timeout}s"])],
                duration_ms=duration_ms,
            )

    def _parse_valgrind(self, output: str) -> tuple[list[MemoryIssue], MemoryLeaks, int]:
        """Parse valgrind output."""
        issues = []
        leaks = MemoryLeaks()
        error_count = 0

        # Parse leak summary
        leak_patterns = [
            (r"definitely lost: ([\d,]+) bytes", "definitely_lost"),
            (r"indirectly lost: ([\d,]+) bytes", "indirectly_lost"),
            (r"possibly lost: ([\d,]+) bytes", "possibly_lost"),
            (r"still reachable: ([\d,]+) bytes", "still_reachable"),
        ]

        for pattern, attr in leak_patterns:
            match = re.search(pattern, output)
            if match:
                value = int(match.group(1).replace(",", ""))
                setattr(leaks, attr, value)

        # Parse error count
        error_match = re.search(r"ERROR SUMMARY: (\d+) errors", output)
        if error_match:
            error_count = int(error_match.group(1))

        # Parse individual errors (simplified)
        error_patterns = [
            r"Invalid (read|write) of size (\d+)",
            r"Use of uninitialised value",
            r"Conditional jump or move depends on uninitialised value",
            r"Source and destination overlap",
        ]

        for pattern in error_patterns:
            for match in re.finditer(pattern, output):
                issues.append(MemoryIssue(
                    type=match.group(0),
                    size=int(match.group(2)) if len(match.groups()) > 1 else None,
                    address=None,
                    location=None,
                ))

        return issues, leaks, error_count
```

---

## Configuration

```yaml
# ~/.entropi/config.yaml
mcp:
  enable_c: true

  c:
    # Default compiler
    compiler: "gcc"

    # Default C standard
    standard: "c17"

    # Default warning level
    warnings: "all"

    # Always enable debug symbols
    debug: true

    # Default sanitizers for development
    default_sanitizers: ["address", "undefined"]

    # Valgrind options
    valgrind_timeout: 60
```

---

## Benefits vs Bash Routing

| Aspect | bash.execute | c.* tools |
|--------|--------------|-----------|
| **Error parsing** | Raw gcc output | Structured file/line/message |
| **Build detection** | Manual | Auto-detect Make/CMake |
| **Memory checking** | Forget to run | Built-in valgrind |
| **Sanitizers** | Complex flags | Simple option |
| **Signal handling** | Exit code only | Named signal + explanation |
| **Token efficiency** | Long commands | Clean tool calls |

**Example comparison:**

```json
// Bash approach (error-prone)
{"name": "bash.execute", "arguments": {"command": "gcc -std=c17 -Wall -Wextra -g -fsanitize=address main.c -o main && ./main"}}

// C tools approach (clean)
{"name": "c.compile", "arguments": {"sources": ["main.c"], "sanitizers": ["address"]}}
{"name": "c.run", "arguments": {}}
```

---

## System Prompt Addition

```markdown
## C Development

For C development tasks, prefer dedicated tools over bash:

| Task | Use | Not |
|------|-----|-----|
| Compile | `c.compile` | `bash.execute("gcc ...")` |
| Run binary | `c.run` | `bash.execute("./main")` |
| Build project | `c.build` | `bash.execute("make")` |
| Memory check | `c.check` | `bash.execute("valgrind ...")` |

C tools automatically:
- Parse compiler errors into structured format
- Detect build system (Make/CMake)
- Include debug symbols and warnings
- Provide crash signal information
```

---

## Rollout Plan

1. **Phase 0**: Implement LSP integration (doc 15) with clangd for real-time diagnostics
2. **Phase 1**: Implement CompilerManager with compile and error parsing
3. **Phase 2**: Implement ExecutionManager with signal handling
4. **Phase 3**: Implement BuildManager for Make/CMake
5. **Phase 4**: Implement MemoryChecker with valgrind
6. **Phase 5**: Create MCP server wrapper and register tools
7. **Phase 6**: Update system prompt to prefer c.* tools

---

## Integration with LSP

When LSP is available, the C MCP server can leverage it:

```python
class CMCPServer:
    def __init__(self, project_dir: Path, lsp_manager: LSPManager | None = None):
        self.lsp = lsp_manager

    async def _handle_compile(self, arguments: dict) -> str:
        # Compile first
        result = await self.compiler.compile(...)

        # If LSP available, also get diagnostics
        if self.lsp and result.success:
            for source in arguments["sources"]:
                diags = await self.lsp.get_diagnostics(Path(source))
                if diags:
                    # Include LSP warnings that compiler might have missed
                    result.warnings.extend(...)

        return self._format_result(result)
```

This provides a complete development experience:
- **LSP (clangd)**: Real-time errors as you type, IDE-like features
- **c.compile**: Actual compilation with all flags
- **c.check**: Runtime memory verification
