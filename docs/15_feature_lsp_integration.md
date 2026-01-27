# Feature Proposal: LSP Integration

> Language Server Protocol integration for real-time code intelligence

**Status:** Implemented (Core)
**Priority:** Medium (significant quality improvement)
**Complexity:** High
**Dependencies:** pylspclient
**Offline:** Yes - LSP servers run locally

## Implementation Status

| Component | Status | Location |
|-----------|--------|----------|
| LSPConfig | Done | `src/entropi/config/schema.py` |
| BaseLSPClient | Done | `src/entropi/lsp/base.py` |
| PyrightClient (Python) | Done | `src/entropi/lsp/pyright_client.py` |
| ClangdClient (C) | Done | `src/entropi/lsp/clangd_client.py` |
| LSPManager | Done | `src/entropi/lsp/manager.py` |
| Diagnostics MCP tool | Done | `src/entropi/mcp/servers/diagnostics.py` |
| Quality enforcer integration | Not started | - |

**Requirements:** `pip install pylspclient`

---

## What is LSP?

Language Server Protocol (LSP) is a standardized protocol for code intelligence. Language servers are **local processes** that analyze your code and provide:

- **Diagnostics**: Real-time errors, warnings, type mismatches
- **Completions**: Context-aware autocomplete suggestions
- **Hover info**: Type signatures and documentation
- **Go to definition**: Find where symbols are defined
- **Find references**: Find all usages of a symbol
- **Rename**: Rename symbols across project
- **Formatting**: Code formatting

### Why It Matters for AI Coding Assistants

Without LSP, the AI must **guess** what's wrong with code. With LSP:

| Without LSP | With LSP |
|-------------|----------|
| "I think there might be an issue..." | "Line 23 has error: 'foo' undefined" |
| Reads entire file looking for bugs | Gets exact error locations instantly |
| May miss subtle type errors | Type checker catches everything |
| Generates code, hopes it compiles | Can verify code before committing |

### Fully Offline

LSP servers run as local processes. No internet required:

```
┌──────────┐      stdin/stdout      ┌──────────┐
│ Entropi  │ ◄───── JSON-RPC ─────► │  gopls   │  ← Local process
└──────────┘                        └──────────┘
                                         │
                                         ▼
                                   Your local code
```

---

## Use Cases for Entropi

### 1. Pre-Generation Validation

Before the AI writes code, check if the file has existing errors:

```
User: "Add error handling to parser.py"

Entropi (internally):
  → Query LSP for parser.py diagnostics
  ← 2 existing errors found

Entropi: "I see parser.py has 2 existing issues:
  - Line 45: missing return statement
  - Line 89: undefined 'config'

Should I fix these as well, or just add error handling?"
```

### 2. Post-Generation Validation

After generating code, verify it's correct:

```
Entropi generates code → writes to temp buffer
  → Sends textDocument/didChange to LSP
  ← LSP returns diagnostics

If errors:
  → Feed errors back to model
  → Regenerate with error context

If clean:
  → Write to actual file
```

### 3. Targeted Bug Fixing

When user says "fix bugs", get real errors instead of guessing:

```
User: "Fix the bugs in this file"

Without LSP:
  AI reads file, guesses what might be wrong,
  might miss subtle issues or "fix" working code

With LSP:
  AI gets exact list:
    - Line 23: Type error
    - Line 45: Undefined variable
    - Line 67: Unreachable code
  Fixes only actual problems
```

### 4. Intelligent Tool Results

Include diagnostics in tool results:

```python
# When read_file is called, also get diagnostics
async def read_file_with_diagnostics(path: str) -> dict:
    content = await read_file(path)
    diagnostics = await lsp.get_diagnostics(path)

    return {
        "content": content,
        "diagnostics": [
            f"Line {d.line}: {d.message} ({d.severity})"
            for d in diagnostics
        ]
    }
```

---

## Architecture

### Component Overview

```
┌─────────────────────────────────────────────────────────────────────────┐
│                              ENTROPI                                    │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│   ┌─────────────┐         ┌─────────────┐         ┌─────────────┐      │
│   │   Agentic   │────────►│ LSP Manager │────────►│ Diagnostics │      │
│   │    Loop     │         │             │         │    Tool     │      │
│   └─────────────┘         └──────┬──────┘         └─────────────┘      │
│                                  │                                      │
│                    ┌─────────────┼─────────────┐                       │
│                    ▼             ▼             ▼                        │
│              ┌──────────┐  ┌──────────┐  ┌──────────┐                  │
│              │  Python  │  │    Go    │  │   Rust   │                  │
│              │  Client  │  │  Client  │  │  Client  │                  │
│              └────┬─────┘  └────┬─────┘  └────┬─────┘                  │
│                   │             │             │                         │
└───────────────────┼─────────────┼─────────────┼─────────────────────────┘
                    │             │             │
                    ▼             ▼             ▼
              ┌──────────┐  ┌──────────┐  ┌──────────┐
              │ pyright  │  │  gopls   │  │rust-     │
              │          │  │          │  │analyzer  │
              └──────────┘  └──────────┘  └──────────┘
                   │             │             │
                   └─────────────┼─────────────┘
                                 ▼
                          Local filesystem
```

### LSP Client

```python
"""
LSP client for a single language server.
"""
import asyncio
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from entropi.core.logging import get_logger

logger = get_logger("lsp.client")


@dataclass
class Position:
    """Position in a document."""
    line: int      # 0-indexed
    character: int # 0-indexed


@dataclass
class Range:
    """Range in a document."""
    start: Position
    end: Position


@dataclass
class Diagnostic:
    """A diagnostic message (error/warning)."""
    range: Range
    severity: int  # 1=Error, 2=Warning, 3=Info, 4=Hint
    source: str
    message: str
    code: str | None = None

    @property
    def severity_name(self) -> str:
        return {1: "error", 2: "warning", 3: "info", 4: "hint"}.get(self.severity, "unknown")

    def format(self) -> str:
        """Format for display."""
        line = self.range.start.line + 1  # 1-indexed for display
        return f"Line {line}: [{self.severity_name}] {self.message}"


class LSPClient:
    """
    Client for a Language Server Protocol server.

    Manages connection to a single language server process.
    """

    def __init__(
        self,
        language: str,
        command: str,
        args: list[str] | None = None,
        root_path: Path | None = None,
    ) -> None:
        self.language = language
        self.command = command
        self.args = args or []
        self.root_path = root_path or Path.cwd()

        self._process: asyncio.subprocess.Process | None = None
        self._request_id = 0
        self._pending: dict[int, asyncio.Future] = {}
        self._diagnostics: dict[str, list[Diagnostic]] = {}  # uri -> diagnostics
        self._initialized = False

    async def start(self) -> None:
        """Start the language server process."""
        logger.info(f"Starting {self.language} LSP: {self.command} {self.args}")

        self._process = await asyncio.create_subprocess_exec(
            self.command,
            *self.args,
            stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )

        # Start reader task
        asyncio.create_task(self._read_loop())

        # Initialize the server
        await self._initialize()

    async def stop(self) -> None:
        """Stop the language server."""
        if self._process:
            # Send shutdown request
            await self._request("shutdown", {})
            # Send exit notification
            await self._notify("exit", {})
            self._process.terminate()
            await self._process.wait()

    async def _initialize(self) -> None:
        """Send initialize request to server."""
        result = await self._request("initialize", {
            "processId": None,
            "rootUri": self.root_path.as_uri(),
            "capabilities": {
                "textDocument": {
                    "publishDiagnostics": {
                        "relatedInformation": True,
                    },
                },
            },
        })

        # Send initialized notification
        await self._notify("initialized", {})
        self._initialized = True
        logger.info(f"{self.language} LSP initialized")

    async def open_file(self, path: Path) -> None:
        """Notify server that a file was opened."""
        content = path.read_text()
        await self._notify("textDocument/didOpen", {
            "textDocument": {
                "uri": path.as_uri(),
                "languageId": self.language,
                "version": 1,
                "text": content,
            }
        })

    async def update_file(self, path: Path, content: str, version: int) -> None:
        """Notify server of file content change."""
        await self._notify("textDocument/didChange", {
            "textDocument": {
                "uri": path.as_uri(),
                "version": version,
            },
            "contentChanges": [{"text": content}],
        })

    async def close_file(self, path: Path) -> None:
        """Notify server that a file was closed."""
        await self._notify("textDocument/didClose", {
            "textDocument": {"uri": path.as_uri()},
        })

    def get_diagnostics(self, path: Path) -> list[Diagnostic]:
        """Get current diagnostics for a file."""
        uri = path.as_uri()
        return self._diagnostics.get(uri, [])

    async def wait_for_diagnostics(
        self,
        path: Path,
        timeout: float = 5.0,
    ) -> list[Diagnostic]:
        """Wait for diagnostics to be published for a file."""
        uri = path.as_uri()

        # If we already have diagnostics, return them
        if uri in self._diagnostics:
            return self._diagnostics[uri]

        # Wait for diagnostics notification
        start = asyncio.get_event_loop().time()
        while asyncio.get_event_loop().time() - start < timeout:
            await asyncio.sleep(0.1)
            if uri in self._diagnostics:
                return self._diagnostics[uri]

        return []

    async def _request(self, method: str, params: dict) -> Any:
        """Send a request and wait for response."""
        self._request_id += 1
        request_id = self._request_id

        message = {
            "jsonrpc": "2.0",
            "id": request_id,
            "method": method,
            "params": params,
        }

        future = asyncio.get_event_loop().create_future()
        self._pending[request_id] = future

        await self._send(message)
        return await future

    async def _notify(self, method: str, params: dict) -> None:
        """Send a notification (no response expected)."""
        message = {
            "jsonrpc": "2.0",
            "method": method,
            "params": params,
        }
        await self._send(message)

    async def _send(self, message: dict) -> None:
        """Send a message to the server."""
        if not self._process or not self._process.stdin:
            raise RuntimeError("LSP not started")

        body = json.dumps(message)
        header = f"Content-Length: {len(body)}\r\n\r\n"
        self._process.stdin.write(header.encode() + body.encode())
        await self._process.stdin.drain()

    async def _read_loop(self) -> None:
        """Read messages from the server."""
        if not self._process or not self._process.stdout:
            return

        reader = self._process.stdout

        while True:
            try:
                # Read headers
                headers = {}
                while True:
                    line = await reader.readline()
                    if line == b"\r\n":
                        break
                    if b":" in line:
                        key, value = line.decode().strip().split(": ", 1)
                        headers[key] = value

                # Read body
                content_length = int(headers.get("Content-Length", 0))
                body = await reader.read(content_length)
                message = json.loads(body.decode())

                await self._handle_message(message)

            except Exception as e:
                logger.error(f"LSP read error: {e}")
                break

    async def _handle_message(self, message: dict) -> None:
        """Handle an incoming message."""
        if "id" in message and "result" in message:
            # Response to our request
            request_id = message["id"]
            if request_id in self._pending:
                self._pending[request_id].set_result(message.get("result"))
                del self._pending[request_id]

        elif "method" in message:
            # Notification from server
            method = message["method"]
            params = message.get("params", {})

            if method == "textDocument/publishDiagnostics":
                self._handle_diagnostics(params)

    def _handle_diagnostics(self, params: dict) -> None:
        """Handle diagnostics notification."""
        uri = params["uri"]
        diagnostics = []

        for d in params.get("diagnostics", []):
            diagnostics.append(Diagnostic(
                range=Range(
                    start=Position(d["range"]["start"]["line"], d["range"]["start"]["character"]),
                    end=Position(d["range"]["end"]["line"], d["range"]["end"]["character"]),
                ),
                severity=d.get("severity", 1),
                source=d.get("source", "unknown"),
                message=d["message"],
                code=str(d.get("code")) if d.get("code") else None,
            ))

        self._diagnostics[uri] = diagnostics
        logger.debug(f"Received {len(diagnostics)} diagnostics for {uri}")
```

### LSP Manager

```python
"""
Manages multiple language server connections.
"""
from pathlib import Path

from entropi.core.logging import get_logger
from entropi.lsp.client import LSPClient, Diagnostic

logger = get_logger("lsp.manager")


# Default language server configurations
DEFAULT_SERVERS = {
    "python": {
        "command": "pyright-langserver",
        "args": ["--stdio"],
        "extensions": [".py"],
    },
    "go": {
        "command": "gopls",
        "args": [],
        "extensions": [".go"],
    },
    "rust": {
        "command": "rust-analyzer",
        "args": [],
        "extensions": [".rs"],
    },
    "typescript": {
        "command": "typescript-language-server",
        "args": ["--stdio"],
        "extensions": [".ts", ".tsx", ".js", ".jsx"],
    },
}


class LSPManager:
    """
    Manages LSP clients for multiple languages.

    Auto-detects project languages and starts appropriate servers.
    """

    def __init__(self, config: LSPConfig, root_path: Path) -> None:
        self.config = config
        self.root_path = root_path
        self._clients: dict[str, LSPClient] = {}
        self._extension_map: dict[str, str] = {}  # .py -> python

    async def start(self) -> None:
        """Start LSP servers for detected languages."""
        if not self.config.enabled:
            logger.info("LSP disabled")
            return

        # Build extension map
        for lang, server_config in {**DEFAULT_SERVERS, **self.config.servers}.items():
            for ext in server_config.get("extensions", []):
                self._extension_map[ext] = lang

        # Detect languages in project
        languages = self._detect_languages()
        logger.info(f"Detected languages: {languages}")

        # Start servers for detected languages
        for lang in languages:
            await self._start_server(lang)

    async def stop(self) -> None:
        """Stop all LSP servers."""
        for client in self._clients.values():
            await client.stop()
        self._clients.clear()

    async def _start_server(self, language: str) -> None:
        """Start a language server."""
        server_config = {**DEFAULT_SERVERS, **self.config.servers}.get(language)
        if not server_config:
            logger.warning(f"No LSP config for {language}")
            return

        if self.config.disabled_languages and language in self.config.disabled_languages:
            logger.info(f"LSP for {language} is disabled")
            return

        try:
            client = LSPClient(
                language=language,
                command=server_config["command"],
                args=server_config.get("args", []),
                root_path=self.root_path,
            )
            await client.start()
            self._clients[language] = client
        except Exception as e:
            logger.error(f"Failed to start {language} LSP: {e}")

    def _detect_languages(self) -> set[str]:
        """Detect languages used in project."""
        languages = set()

        for ext, lang in self._extension_map.items():
            # Check if any files with this extension exist
            if list(self.root_path.rglob(f"*{ext}"))[:1]:  # Just check if any exist
                languages.add(lang)

        return languages

    def get_client(self, path: Path) -> LSPClient | None:
        """Get the LSP client for a file."""
        ext = path.suffix
        lang = self._extension_map.get(ext)
        if lang:
            return self._clients.get(lang)
        return None

    async def get_diagnostics(self, path: Path) -> list[Diagnostic]:
        """Get diagnostics for a file."""
        client = self.get_client(path)
        if not client:
            return []

        # Open the file (idempotent)
        await client.open_file(path)

        # Wait for diagnostics
        return await client.wait_for_diagnostics(path)

    async def get_all_diagnostics(self) -> dict[Path, list[Diagnostic]]:
        """Get diagnostics for all open files."""
        result = {}
        for lang, client in self._clients.items():
            for uri, diags in client._diagnostics.items():
                path = Path(uri.replace("file://", ""))
                result[path] = diags
        return result
```

---

## Configuration

```yaml
# ~/.entropi/config.yaml
lsp:
  enabled: true

  # Languages to disable
  disabled_languages:
    - java  # Skip Java, server is slow

  # Custom server configurations (override defaults)
  servers:
    python:
      command: "pylsp"  # Use pylsp instead of pyright
      args: []
      extensions: [".py"]

    # Add a language not in defaults
    elixir:
      command: "elixir-ls"
      args: []
      extensions: [".ex", ".exs"]
```

---

## MCP Tool: diagnostics

```python
class DiagnosticsTool(MCPTool):
    """Get code diagnostics from LSP."""

    name = "diagnostics"
    description = "Get errors and warnings for a file from the language server"

    input_schema = {
        "type": "object",
        "properties": {
            "file_path": {
                "type": "string",
                "description": "Path to the file (or 'all' for all files)",
            },
        },
    }

    async def execute(self, file_path: str) -> str:
        if file_path == "all":
            all_diags = await self.lsp_manager.get_all_diagnostics()
            if not all_diags:
                return "No diagnostics found in any files."

            lines = []
            for path, diags in all_diags.items():
                if diags:
                    lines.append(f"\n{path}:")
                    for d in diags:
                        lines.append(f"  {d.format()}")
            return "\n".join(lines)

        else:
            path = Path(file_path)
            if not path.exists():
                return f"File not found: {file_path}"

            diags = await self.lsp_manager.get_diagnostics(path)
            if not diags:
                return f"No diagnostics for {file_path}"

            lines = [f"Diagnostics for {file_path}:"]
            for d in diags:
                lines.append(f"  {d.format()}")
            return "\n".join(lines)
```

---

## Integration with Quality Enforcer

The quality enforcer can use LSP as a validation step:

```python
class QualityEnforcer:
    """Enforces code quality with LSP validation."""

    async def validate_generated_code(
        self,
        code: str,
        file_path: Path,
    ) -> ValidationResult:
        # Write code to temp file
        temp_path = self._write_temp(code, file_path.suffix)

        try:
            # Get LSP diagnostics
            diagnostics = await self.lsp_manager.get_diagnostics(temp_path)

            # Filter to errors only
            errors = [d for d in diagnostics if d.severity == 1]

            if errors:
                return ValidationResult(
                    passed=False,
                    errors=[d.format() for d in errors],
                    feedback=self._format_feedback(errors),
                )

            return ValidationResult(passed=True, errors=[])

        finally:
            temp_path.unlink()
```

---

## System Requirements

### Installing Language Servers

```bash
# Python (pick one)
pip install pyright                    # Fast, TypeScript-based
pip install python-lsp-server[all]     # Python-based, more plugins

# Go
go install golang.org/x/tools/gopls@latest

# Rust
rustup component add rust-analyzer

# TypeScript/JavaScript
npm install -g typescript-language-server typescript

# C/C++
sudo apt install clangd
```

### Entropi Pre-work Addition

```bash
# Optional: Install language servers for LSP support
# Python
pip install pyright

# Go (if working with Go projects)
go install golang.org/x/tools/gopls@latest

# Verify
pyright --version
gopls version
```

---

## Benefits Summary

| Feature | Without LSP | With LSP |
|---------|-------------|----------|
| Error detection | AI guesses | Machine-verified |
| Type checking | Best effort | Full type analysis |
| Bug fixing | May fix wrong things | Targets real issues |
| Code validation | Run and see | Pre-flight check |
| Refactoring | Hope for best | Confidence in correctness |

---

## Implementation Priority

LSP adds significant value but isn't required for v1. Recommend:

1. **v1.0**: Ship without LSP (manual validation)
2. **v1.1**: Add LSP diagnostics tool
3. **v1.2**: Add pre-generation validation
4. **v1.3**: Add post-generation validation loop
