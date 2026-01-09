# MCP Server: Python 3.12 Tools

> Dedicated Python environment management and execution tools

**Status:** Ready for Implementation
**Priority:** High (reduces error rate vs bash routing)
**Complexity:** Medium
**Dependencies:** MCP client infrastructure

---

## Problem Statement

Currently, Python operations route through the `bash.execute` tool:

```bash
bash.execute("python3 main.py")
bash.execute("pip install requests")
bash.execute("python3 -m venv .venv")
```

This causes issues:

1. **Error-prone**: Shell escaping, path issues, environment variables
2. **No isolation**: pip installs to system or wrong venv
3. **Verbose errors**: Shell errors mixed with Python errors
4. **No state management**: Model must track which venv is active
5. **Permission confusion**: Model may try to install globally

A dedicated Python MCP server provides:
- **Clean API**: Purpose-built tools for Python operations
- **Automatic venv isolation**: All operations scoped to project venv
- **Structured errors**: Python-specific error handling
- **State management**: Server tracks active environment
- **Reduced token usage**: Cleaner tool calls and results

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                     Python MCP Server                            │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  Tools:                                                          │
│  ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐  │
│  │ python.run      │  │ python.venv     │  │ python.pip      │  │
│  │                 │  │                 │  │                 │  │
│  │ Execute scripts │  │ Create/manage   │  │ Install/list    │  │
│  │ and code        │  │ virtual envs    │  │ packages        │  │
│  └────────┬────────┘  └────────┬────────┘  └────────┬────────┘  │
│           │                    │                    │            │
│           └────────────────────┼────────────────────┘            │
│                                │                                 │
│                    ┌───────────▼───────────┐                    │
│                    │   VenvManager         │                    │
│                    │                       │                    │
│                    │ - Auto-detect venv    │                    │
│                    │ - Create if missing   │                    │
│                    │ - Isolate all ops     │                    │
│                    └───────────────────────┘                    │
│                                │                                 │
└────────────────────────────────┼─────────────────────────────────┘
                                 │
                                 ▼
                    ┌───────────────────────┐
                    │   Python 3.12         │
                    │   (system or venv)    │
                    └───────────────────────┘
```

---

## Tools

### 1. python.run

Execute Python scripts or inline code.

```python
{
    "name": "python.run",
    "description": "Execute Python code or scripts. Automatically uses project venv if available.",
    "inputSchema": {
        "type": "object",
        "properties": {
            "file": {
                "type": "string",
                "description": "Path to Python file to execute"
            },
            "code": {
                "type": "string",
                "description": "Inline Python code to execute (alternative to file)"
            },
            "args": {
                "type": "array",
                "items": {"type": "string"},
                "description": "Command line arguments to pass to script"
            },
            "timeout": {
                "type": "integer",
                "description": "Execution timeout in seconds (default: 30)",
                "default": 30
            },
            "capture_output": {
                "type": "boolean",
                "description": "Capture stdout/stderr (default: true)",
                "default": true
            }
        },
        "oneOf": [
            {"required": ["file"]},
            {"required": ["code"]}
        ]
    }
}
```

**Examples:**

```json
// Run a file
{"name": "python.run", "arguments": {"file": "main.py"}}

// Run with arguments
{"name": "python.run", "arguments": {"file": "test.py", "args": ["-v", "--coverage"]}}

// Run inline code
{"name": "python.run", "arguments": {"code": "print('Hello')"}}

// Run with timeout
{"name": "python.run", "arguments": {"file": "long_task.py", "timeout": 120}}
```

**Return Format:**

```json
{
    "success": true,
    "exit_code": 0,
    "stdout": "Hello, World!\n",
    "stderr": "",
    "duration_ms": 45,
    "python_version": "3.12.0",
    "venv": ".venv"
}
```

---

### 2. python.venv

Create and manage virtual environments.

```python
{
    "name": "python.venv",
    "description": "Create, activate, or manage Python virtual environments.",
    "inputSchema": {
        "type": "object",
        "properties": {
            "action": {
                "type": "string",
                "enum": ["create", "delete", "info", "list"],
                "description": "Action to perform"
            },
            "path": {
                "type": "string",
                "description": "Path for venv (default: .venv)",
                "default": ".venv"
            },
            "python": {
                "type": "string",
                "description": "Python interpreter to use (default: python3.12)",
                "default": "python3.12"
            },
            "system_site_packages": {
                "type": "boolean",
                "description": "Include system site-packages",
                "default": false
            }
        },
        "required": ["action"]
    }
}
```

**Examples:**

```json
// Create default venv
{"name": "python.venv", "arguments": {"action": "create"}}

// Create named venv
{"name": "python.venv", "arguments": {"action": "create", "path": ".venv-test"}}

// Get venv info
{"name": "python.venv", "arguments": {"action": "info"}}

// List available venvs
{"name": "python.venv", "arguments": {"action": "list"}}

// Delete venv
{"name": "python.venv", "arguments": {"action": "delete", "path": ".venv-old"}}
```

**Return Format (info):**

```json
{
    "exists": true,
    "path": "/workspace/.venv",
    "python_version": "3.12.0",
    "packages_count": 15,
    "size_mb": 45.2,
    "created": "2025-01-06T10:00:00Z"
}
```

---

### 3. python.pip

Install, uninstall, and list packages.

```python
{
    "name": "python.pip",
    "description": "Manage Python packages in the project virtual environment.",
    "inputSchema": {
        "type": "object",
        "properties": {
            "action": {
                "type": "string",
                "enum": ["install", "uninstall", "list", "freeze", "show"],
                "description": "Action to perform"
            },
            "packages": {
                "type": "array",
                "items": {"type": "string"},
                "description": "Package names (with optional version specifiers)"
            },
            "requirements": {
                "type": "string",
                "description": "Path to requirements file"
            },
            "upgrade": {
                "type": "boolean",
                "description": "Upgrade packages to latest version",
                "default": false
            }
        },
        "required": ["action"]
    }
}
```

**Examples:**

```json
// Install packages
{"name": "python.pip", "arguments": {"action": "install", "packages": ["requests", "flask>=2.0"]}}

// Install from requirements.txt
{"name": "python.pip", "arguments": {"action": "install", "requirements": "requirements.txt"}}

// Upgrade packages
{"name": "python.pip", "arguments": {"action": "install", "packages": ["requests"], "upgrade": true}}

// List installed packages
{"name": "python.pip", "arguments": {"action": "list"}}

// Freeze to requirements format
{"name": "python.pip", "arguments": {"action": "freeze"}}

// Show package info
{"name": "python.pip", "arguments": {"action": "show", "packages": ["requests"]}}

// Uninstall
{"name": "python.pip", "arguments": {"action": "uninstall", "packages": ["old-package"]}}
```

**Return Format (list):**

```json
{
    "packages": [
        {"name": "requests", "version": "2.31.0"},
        {"name": "flask", "version": "3.0.0"},
        {"name": "pip", "version": "24.0"}
    ],
    "count": 3,
    "venv": ".venv"
}
```

**Return Format (install):**

```json
{
    "success": true,
    "installed": [
        {"name": "requests", "version": "2.31.0"},
        {"name": "urllib3", "version": "2.1.0"}
    ],
    "already_satisfied": [],
    "duration_ms": 2340
}
```

---

## Implementation

### VenvManager

```python
"""
Virtual environment manager for Python MCP server.
"""
import asyncio
import json
import shutil
import sys
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

from entropi.core.logging import get_logger

logger = get_logger("mcp.python.venv")


@dataclass
class VenvInfo:
    """Information about a virtual environment."""

    path: Path
    exists: bool
    python_version: str | None = None
    packages_count: int = 0
    size_mb: float = 0.0
    created: datetime | None = None


class VenvManager:
    """
    Manages Python virtual environments.

    Automatically detects and uses project venvs for all operations.
    """

    VENV_NAMES = [".venv", "venv", ".virtualenv", "virtualenv"]
    DEFAULT_VENV = ".venv"
    PYTHON_EXECUTABLE = "python3.12"

    def __init__(self, project_dir: Path) -> None:
        self.project_dir = project_dir
        self._active_venv: Path | None = None

    async def initialize(self) -> None:
        """Initialize and detect existing venv."""
        self._active_venv = self._detect_venv()
        if self._active_venv:
            logger.info(f"Detected venv: {self._active_venv}")
        else:
            logger.info("No venv detected, will use system Python or create on demand")

    def _detect_venv(self) -> Path | None:
        """Detect existing virtual environment in project."""
        for name in self.VENV_NAMES:
            venv_path = self.project_dir / name
            if self._is_valid_venv(venv_path):
                return venv_path
        return None

    def _is_valid_venv(self, path: Path) -> bool:
        """Check if path is a valid venv."""
        if not path.exists():
            return False
        # Check for python executable
        python_path = path / "bin" / "python"
        return python_path.exists()

    @property
    def python_executable(self) -> Path:
        """Get path to Python executable (venv or system)."""
        if self._active_venv:
            return self._active_venv / "bin" / "python"
        return Path(shutil.which(self.PYTHON_EXECUTABLE) or "python3")

    @property
    def pip_executable(self) -> Path:
        """Get path to pip executable."""
        if self._active_venv:
            return self._active_venv / "bin" / "pip"
        return Path(shutil.which("pip3") or "pip")

    async def create_venv(
        self,
        path: str = DEFAULT_VENV,
        python: str = PYTHON_EXECUTABLE,
        system_site_packages: bool = False,
    ) -> VenvInfo:
        """Create a new virtual environment."""
        venv_path = self.project_dir / path

        if venv_path.exists():
            raise ValueError(f"Venv already exists: {venv_path}")

        # Build command
        cmd = [python, "-m", "venv"]
        if system_site_packages:
            cmd.append("--system-site-packages")
        cmd.append(str(venv_path))

        logger.info(f"Creating venv: {' '.join(cmd)}")

        proc = await asyncio.create_subprocess_exec(
            *cmd,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
            cwd=self.project_dir,
        )
        stdout, stderr = await proc.communicate()

        if proc.returncode != 0:
            raise RuntimeError(f"Failed to create venv: {stderr.decode()}")

        # Upgrade pip in new venv
        pip_path = venv_path / "bin" / "pip"
        await self._run_pip(pip_path, ["install", "--upgrade", "pip"])

        # Set as active
        self._active_venv = venv_path

        return await self.get_venv_info(path)

    async def delete_venv(self, path: str) -> None:
        """Delete a virtual environment."""
        venv_path = self.project_dir / path

        if not venv_path.exists():
            raise ValueError(f"Venv not found: {venv_path}")

        if self._active_venv == venv_path:
            self._active_venv = None

        shutil.rmtree(venv_path)
        logger.info(f"Deleted venv: {venv_path}")

    async def get_venv_info(self, path: str = DEFAULT_VENV) -> VenvInfo:
        """Get information about a venv."""
        venv_path = self.project_dir / path

        if not venv_path.exists():
            return VenvInfo(path=venv_path, exists=False)

        # Get Python version
        python_path = venv_path / "bin" / "python"
        version = await self._get_python_version(python_path)

        # Count packages
        pip_path = venv_path / "bin" / "pip"
        packages = await self._list_packages(pip_path)

        # Calculate size
        size_mb = sum(f.stat().st_size for f in venv_path.rglob("*") if f.is_file()) / (1024 * 1024)

        # Get creation time
        created = datetime.fromtimestamp(venv_path.stat().st_ctime)

        return VenvInfo(
            path=venv_path,
            exists=True,
            python_version=version,
            packages_count=len(packages),
            size_mb=round(size_mb, 2),
            created=created,
        )

    async def list_venvs(self) -> list[VenvInfo]:
        """List all venvs in project directory."""
        venvs = []
        for name in self.VENV_NAMES:
            path = self.project_dir / name
            if self._is_valid_venv(path):
                info = await self.get_venv_info(name)
                venvs.append(info)

        # Also check for custom-named venvs (directories with bin/python)
        for path in self.project_dir.iterdir():
            if path.is_dir() and path.name not in self.VENV_NAMES:
                if self._is_valid_venv(path):
                    info = await self.get_venv_info(path.name)
                    venvs.append(info)

        return venvs

    async def _get_python_version(self, python_path: Path) -> str:
        """Get Python version from executable."""
        proc = await asyncio.create_subprocess_exec(
            str(python_path), "--version",
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )
        stdout, _ = await proc.communicate()
        return stdout.decode().strip().replace("Python ", "")

    async def _list_packages(self, pip_path: Path) -> list[dict]:
        """List installed packages."""
        proc = await asyncio.create_subprocess_exec(
            str(pip_path), "list", "--format=json",
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )
        stdout, _ = await proc.communicate()
        return json.loads(stdout.decode()) if stdout else []

    async def _run_pip(self, pip_path: Path, args: list[str]) -> tuple[str, str, int]:
        """Run pip command."""
        proc = await asyncio.create_subprocess_exec(
            str(pip_path), *args,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )
        stdout, stderr = await proc.communicate()
        return stdout.decode(), stderr.decode(), proc.returncode
```

### PythonRunner

```python
"""
Python script/code execution for MCP server.
"""
import asyncio
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path

from entropi.core.logging import get_logger
from entropi.mcp.servers.python.venv import VenvManager

logger = get_logger("mcp.python.runner")


@dataclass
class ExecutionResult:
    """Result of Python execution."""

    success: bool
    exit_code: int
    stdout: str
    stderr: str
    duration_ms: int
    python_version: str
    venv: str | None


class PythonRunner:
    """
    Executes Python scripts and code.

    Uses project venv automatically for isolation.
    """

    DEFAULT_TIMEOUT = 30

    def __init__(self, venv_manager: VenvManager) -> None:
        self.venv_manager = venv_manager

    async def run_file(
        self,
        file: str,
        args: list[str] | None = None,
        timeout: int = DEFAULT_TIMEOUT,
        capture_output: bool = True,
    ) -> ExecutionResult:
        """Run a Python file."""
        file_path = self.venv_manager.project_dir / file

        if not file_path.exists():
            return ExecutionResult(
                success=False,
                exit_code=1,
                stdout="",
                stderr=f"File not found: {file}",
                duration_ms=0,
                python_version="",
                venv=None,
            )

        return await self._execute(
            [str(file_path)] + (args or []),
            timeout=timeout,
            capture_output=capture_output,
        )

    async def run_code(
        self,
        code: str,
        timeout: int = DEFAULT_TIMEOUT,
        capture_output: bool = True,
    ) -> ExecutionResult:
        """Run inline Python code."""
        # Write code to temp file
        with tempfile.NamedTemporaryFile(
            mode="w",
            suffix=".py",
            delete=False,
            dir=self.venv_manager.project_dir,
        ) as f:
            f.write(code)
            temp_path = Path(f.name)

        try:
            return await self._execute(
                [str(temp_path)],
                timeout=timeout,
                capture_output=capture_output,
            )
        finally:
            temp_path.unlink()

    async def _execute(
        self,
        args: list[str],
        timeout: int,
        capture_output: bool,
    ) -> ExecutionResult:
        """Execute Python with given arguments."""
        python = self.venv_manager.python_executable

        start = time.time()

        try:
            if capture_output:
                proc = await asyncio.create_subprocess_exec(
                    str(python), *args,
                    stdout=asyncio.subprocess.PIPE,
                    stderr=asyncio.subprocess.PIPE,
                    cwd=self.venv_manager.project_dir,
                )
                stdout, stderr = await asyncio.wait_for(
                    proc.communicate(),
                    timeout=timeout,
                )
                stdout_str = stdout.decode()
                stderr_str = stderr.decode()
            else:
                proc = await asyncio.create_subprocess_exec(
                    str(python), *args,
                    cwd=self.venv_manager.project_dir,
                )
                await asyncio.wait_for(proc.wait(), timeout=timeout)
                stdout_str = ""
                stderr_str = ""

            duration_ms = int((time.time() - start) * 1000)

            # Get version info
            version = await self._get_version()

            return ExecutionResult(
                success=proc.returncode == 0,
                exit_code=proc.returncode or 0,
                stdout=stdout_str,
                stderr=stderr_str,
                duration_ms=duration_ms,
                python_version=version,
                venv=str(self.venv_manager._active_venv) if self.venv_manager._active_venv else None,
            )

        except asyncio.TimeoutError:
            duration_ms = int((time.time() - start) * 1000)
            return ExecutionResult(
                success=False,
                exit_code=-1,
                stdout="",
                stderr=f"Execution timed out after {timeout} seconds",
                duration_ms=duration_ms,
                python_version="",
                venv=None,
            )

    async def _get_version(self) -> str:
        """Get Python version."""
        python = self.venv_manager.python_executable
        proc = await asyncio.create_subprocess_exec(
            str(python), "--version",
            stdout=asyncio.subprocess.PIPE,
        )
        stdout, _ = await proc.communicate()
        return stdout.decode().strip().replace("Python ", "")
```

### PipManager

```python
"""
Pip package management for MCP server.
"""
import asyncio
import json
import re
from dataclasses import dataclass
from pathlib import Path

from entropi.core.logging import get_logger
from entropi.mcp.servers.python.venv import VenvManager

logger = get_logger("mcp.python.pip")


@dataclass
class Package:
    """Package information."""

    name: str
    version: str


@dataclass
class InstallResult:
    """Result of package installation."""

    success: bool
    installed: list[Package]
    already_satisfied: list[str]
    errors: list[str]
    duration_ms: int


class PipManager:
    """
    Manages pip operations within project venv.
    """

    def __init__(self, venv_manager: VenvManager) -> None:
        self.venv_manager = venv_manager

    async def install(
        self,
        packages: list[str] | None = None,
        requirements: str | None = None,
        upgrade: bool = False,
    ) -> InstallResult:
        """Install packages."""
        import time
        start = time.time()

        cmd = ["install"]

        if upgrade:
            cmd.append("--upgrade")

        if requirements:
            req_path = self.venv_manager.project_dir / requirements
            if not req_path.exists():
                return InstallResult(
                    success=False,
                    installed=[],
                    already_satisfied=[],
                    errors=[f"Requirements file not found: {requirements}"],
                    duration_ms=0,
                )
            cmd.extend(["-r", str(req_path)])

        if packages:
            cmd.extend(packages)

        stdout, stderr, returncode = await self._run_pip(cmd)
        duration_ms = int((time.time() - start) * 1000)

        if returncode != 0:
            return InstallResult(
                success=False,
                installed=[],
                already_satisfied=[],
                errors=[stderr],
                duration_ms=duration_ms,
            )

        # Parse output to find what was installed
        installed = []
        already_satisfied = []

        for line in stdout.split("\n"):
            if "Successfully installed" in line:
                # Parse "Successfully installed package-1.0 package2-2.0"
                parts = line.replace("Successfully installed", "").strip().split()
                for part in parts:
                    match = re.match(r"(.+)-(\d+\..+)", part)
                    if match:
                        installed.append(Package(name=match.group(1), version=match.group(2)))
            elif "Requirement already satisfied" in line:
                match = re.search(r"Requirement already satisfied: (\S+)", line)
                if match:
                    already_satisfied.append(match.group(1))

        return InstallResult(
            success=True,
            installed=installed,
            already_satisfied=already_satisfied,
            errors=[],
            duration_ms=duration_ms,
        )

    async def uninstall(self, packages: list[str]) -> dict:
        """Uninstall packages."""
        cmd = ["uninstall", "-y"] + packages
        stdout, stderr, returncode = await self._run_pip(cmd)

        return {
            "success": returncode == 0,
            "uninstalled": packages if returncode == 0 else [],
            "errors": [stderr] if returncode != 0 else [],
        }

    async def list_packages(self) -> list[Package]:
        """List installed packages."""
        stdout, _, _ = await self._run_pip(["list", "--format=json"])
        packages_data = json.loads(stdout) if stdout else []
        return [Package(name=p["name"], version=p["version"]) for p in packages_data]

    async def freeze(self) -> str:
        """Get requirements.txt format output."""
        stdout, _, _ = await self._run_pip(["freeze"])
        return stdout.strip()

    async def show(self, packages: list[str]) -> list[dict]:
        """Show package details."""
        results = []
        for pkg in packages:
            stdout, stderr, returncode = await self._run_pip(["show", pkg])
            if returncode == 0:
                # Parse key: value format
                info = {}
                for line in stdout.split("\n"):
                    if ": " in line:
                        key, value = line.split(": ", 1)
                        info[key.lower().replace("-", "_")] = value
                results.append(info)
            else:
                results.append({"name": pkg, "error": f"Package not found: {pkg}"})
        return results

    async def _run_pip(self, args: list[str]) -> tuple[str, str, int]:
        """Run pip command."""
        pip = self.venv_manager.pip_executable

        proc = await asyncio.create_subprocess_exec(
            str(pip), *args,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
            cwd=self.venv_manager.project_dir,
        )
        stdout, stderr = await proc.communicate()
        return stdout.decode(), stderr.decode(), proc.returncode
```

### MCP Server

```python
"""
Python MCP Server - provides Python environment and execution tools.
"""
from pathlib import Path
from typing import Any

from entropi.core.logging import get_logger
from entropi.mcp.server import MCPServer, Tool

from .pip import PipManager
from .runner import PythonRunner
from .venv import VenvManager

logger = get_logger("mcp.servers.python")


class PythonMCPServer(MCPServer):
    """
    MCP Server providing Python 3.12 tools.

    Tools:
    - python.run: Execute Python scripts or code
    - python.venv: Create/manage virtual environments
    - python.pip: Install/manage packages
    """

    def __init__(self, project_dir: Path) -> None:
        super().__init__(name="python")
        self.project_dir = project_dir

        # Components
        self.venv_manager = VenvManager(project_dir)
        self.runner = PythonRunner(self.venv_manager)
        self.pip_manager = PipManager(self.venv_manager)

    async def initialize(self) -> None:
        """Initialize the server."""
        await self.venv_manager.initialize()
        logger.info("Python MCP server initialized")

    def get_tools(self) -> list[Tool]:
        """Get available tools."""
        return [
            Tool(
                name="python.run",
                description="Execute Python code or scripts. Automatically uses project venv if available.",
                input_schema={
                    "type": "object",
                    "properties": {
                        "file": {"type": "string", "description": "Path to Python file to execute"},
                        "code": {"type": "string", "description": "Inline Python code to execute"},
                        "args": {"type": "array", "items": {"type": "string"}, "description": "Command line arguments"},
                        "timeout": {"type": "integer", "description": "Timeout in seconds", "default": 30},
                    },
                },
                handler=self._handle_run,
            ),
            Tool(
                name="python.venv",
                description="Create, manage, or get info about Python virtual environments.",
                input_schema={
                    "type": "object",
                    "properties": {
                        "action": {"type": "string", "enum": ["create", "delete", "info", "list"]},
                        "path": {"type": "string", "description": "Venv path", "default": ".venv"},
                        "python": {"type": "string", "description": "Python interpreter", "default": "python3.12"},
                        "system_site_packages": {"type": "boolean", "default": False},
                    },
                    "required": ["action"],
                },
                handler=self._handle_venv,
            ),
            Tool(
                name="python.pip",
                description="Install, uninstall, or list Python packages in the project venv.",
                input_schema={
                    "type": "object",
                    "properties": {
                        "action": {"type": "string", "enum": ["install", "uninstall", "list", "freeze", "show"]},
                        "packages": {"type": "array", "items": {"type": "string"}, "description": "Package names"},
                        "requirements": {"type": "string", "description": "Path to requirements file"},
                        "upgrade": {"type": "boolean", "default": False},
                    },
                    "required": ["action"],
                },
                handler=self._handle_pip,
            ),
        ]

    async def _handle_run(self, arguments: dict[str, Any]) -> str:
        """Handle python.run tool."""
        file = arguments.get("file")
        code = arguments.get("code")
        args = arguments.get("args", [])
        timeout = arguments.get("timeout", 30)

        if file:
            result = await self.runner.run_file(file, args, timeout)
        elif code:
            result = await self.runner.run_code(code, timeout)
        else:
            return "Error: Must provide either 'file' or 'code'"

        # Format result
        output = []
        if result.stdout:
            output.append(result.stdout)
        if result.stderr:
            output.append(f"[stderr]\n{result.stderr}")

        if result.success:
            output.append(f"\n[Completed in {result.duration_ms}ms, Python {result.python_version}]")
        else:
            output.append(f"\n[Failed with exit code {result.exit_code}]")

        return "\n".join(output)

    async def _handle_venv(self, arguments: dict[str, Any]) -> str:
        """Handle python.venv tool."""
        action = arguments["action"]
        path = arguments.get("path", ".venv")

        if action == "create":
            python = arguments.get("python", "python3.12")
            system_packages = arguments.get("system_site_packages", False)
            info = await self.venv_manager.create_venv(path, python, system_packages)
            return f"Created venv at {info.path}\nPython: {info.python_version}"

        elif action == "delete":
            await self.venv_manager.delete_venv(path)
            return f"Deleted venv: {path}"

        elif action == "info":
            info = await self.venv_manager.get_venv_info(path)
            if not info.exists:
                return f"No venv found at: {path}"
            return (
                f"Venv: {info.path}\n"
                f"Python: {info.python_version}\n"
                f"Packages: {info.packages_count}\n"
                f"Size: {info.size_mb} MB\n"
                f"Created: {info.created}"
            )

        elif action == "list":
            venvs = await self.venv_manager.list_venvs()
            if not venvs:
                return "No virtual environments found"
            return "\n".join(f"- {v.path.name} (Python {v.python_version}, {v.packages_count} packages)" for v in venvs)

        return f"Unknown action: {action}"

    async def _handle_pip(self, arguments: dict[str, Any]) -> str:
        """Handle python.pip tool."""
        action = arguments["action"]

        if action == "install":
            packages = arguments.get("packages", [])
            requirements = arguments.get("requirements")
            upgrade = arguments.get("upgrade", False)

            result = await self.pip_manager.install(packages, requirements, upgrade)

            if not result.success:
                return f"Installation failed:\n{result.errors[0]}"

            lines = []
            if result.installed:
                lines.append("Installed:")
                for pkg in result.installed:
                    lines.append(f"  - {pkg.name}=={pkg.version}")
            if result.already_satisfied:
                lines.append(f"Already satisfied: {', '.join(result.already_satisfied)}")
            lines.append(f"[{result.duration_ms}ms]")
            return "\n".join(lines)

        elif action == "uninstall":
            packages = arguments.get("packages", [])
            result = await self.pip_manager.uninstall(packages)
            if result["success"]:
                return f"Uninstalled: {', '.join(result['uninstalled'])}"
            return f"Failed: {result['errors'][0]}"

        elif action == "list":
            packages = await self.pip_manager.list_packages()
            if not packages:
                return "No packages installed"
            return "\n".join(f"{p.name}=={p.version}" for p in packages)

        elif action == "freeze":
            return await self.pip_manager.freeze()

        elif action == "show":
            packages = arguments.get("packages", [])
            results = await self.pip_manager.show(packages)
            output = []
            for info in results:
                if "error" in info:
                    output.append(f"[{info['name']}] {info['error']}")
                else:
                    output.append(f"[{info.get('name', 'unknown')}]")
                    output.append(f"  Version: {info.get('version', 'unknown')}")
                    output.append(f"  Location: {info.get('location', 'unknown')}")
                    if info.get("requires"):
                        output.append(f"  Requires: {info['requires']}")
            return "\n".join(output)

        return f"Unknown action: {action}"
```

---

## Configuration

```yaml
# ~/.entropi/config.yaml
mcp:
  enable_python: true

  python:
    # Default Python interpreter for venv creation
    interpreter: "python3.12"

    # Auto-create venv if not found
    auto_create_venv: true

    # Default execution timeout
    default_timeout: 30

    # Max execution timeout
    max_timeout: 300
```

---

## Benefits vs Bash Routing

| Aspect | bash.execute | python.* tools |
|--------|--------------|----------------|
| **Venv handling** | Manual activation | Automatic |
| **Error messages** | Shell + Python mixed | Python-only |
| **Path handling** | Shell escaping needed | Native Path objects |
| **Package install** | May hit wrong pip | Always venv pip |
| **Timeout** | No built-in | Configurable |
| **Output parsing** | Raw text | Structured JSON |
| **Token efficiency** | Verbose commands | Clean tool calls |

**Example comparison:**

```json
// Bash approach (error-prone)
{"name": "bash.execute", "arguments": {"command": "source .venv/bin/activate && pip install requests && python main.py"}}

// Python tools approach (clean)
{"name": "python.pip", "arguments": {"action": "install", "packages": ["requests"]}}
{"name": "python.run", "arguments": {"file": "main.py"}}
```

---

## Testing

```python
class TestPythonMCPServer:
    async def test_venv_creation(self, temp_dir):
        """Should create venv successfully."""
        server = PythonMCPServer(temp_dir)
        await server.initialize()

        result = await server._handle_venv({"action": "create"})
        assert "Created venv" in result
        assert (temp_dir / ".venv" / "bin" / "python").exists()

    async def test_pip_install(self, temp_dir):
        """Should install packages in venv."""
        server = PythonMCPServer(temp_dir)
        await server.initialize()
        await server._handle_venv({"action": "create"})

        result = await server._handle_pip({
            "action": "install",
            "packages": ["requests"]
        })
        assert "Installed" in result
        assert "requests" in result

    async def test_python_run(self, temp_dir):
        """Should run Python code."""
        server = PythonMCPServer(temp_dir)
        await server.initialize()

        result = await server._handle_run({
            "code": "print('hello')"
        })
        assert "hello" in result

    async def test_run_with_venv(self, temp_dir):
        """Should use venv Python."""
        server = PythonMCPServer(temp_dir)
        await server.initialize()
        await server._handle_venv({"action": "create"})
        await server._handle_pip({"action": "install", "packages": ["requests"]})

        # This should work because requests is in venv
        result = await server._handle_run({
            "code": "import requests; print(requests.__version__)"
        })
        assert "2." in result  # Version number
```

---

## Rollout Plan

1. **Phase 1**: Implement VenvManager with create/detect/info
2. **Phase 2**: Implement PipManager with install/list
3. **Phase 3**: Implement PythonRunner with file/code execution
4. **Phase 4**: Create MCP server wrapper and register tools
5. **Phase 5**: Add to default MCP server list in config
6. **Phase 6**: Update system prompt to prefer python.* tools over bash for Python operations

---

## System Prompt Addition

```markdown
## Python Operations

For Python tasks, prefer dedicated tools over bash:

| Task | Use | Not |
|------|-----|-----|
| Run script | `python.run` | `bash.execute("python ...")` |
| Install package | `python.pip` | `bash.execute("pip ...")` |
| Create venv | `python.venv` | `bash.execute("python -m venv ...")` |

Python tools automatically use the project's virtual environment.
```
