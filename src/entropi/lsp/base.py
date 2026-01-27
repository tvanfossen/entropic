"""
LSP integration using pylspclient.

Provides code intelligence via Language Server Protocol.
Uses pylspclient for communication and lsprotocol for types.

Requirements:
    pip install pylspclient lsprotocol
"""

from __future__ import annotations

import subprocess
from abc import ABC, abstractmethod
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from entropi.core.logging import get_logger

# These are optional dependencies - gracefully handle if not installed
try:
    import pylspclient
    from pylspclient import LspClient, LspEndpoint

    HAS_LSP = True
except ImportError:
    HAS_LSP = False
    LspClient = None  # type: ignore
    LspEndpoint = None  # type: ignore

logger = get_logger("lsp.base")


@dataclass
class Diagnostic:
    """A diagnostic message (error/warning)."""

    line: int  # 1-indexed for display
    character: int
    severity: str  # "error", "warning", "info", "hint"
    message: str
    source: str
    code: str | None = None

    @property
    def is_error(self) -> bool:
        """Check if this is an error."""
        return self.severity == "error"

    def format(self) -> str:
        """Format for display."""
        return f"Line {self.line}: [{self.severity}] {self.message}"

    @classmethod
    def from_lsp(cls, diag: dict[str, Any]) -> Diagnostic:
        """Create from LSP diagnostic dict."""
        severity_map = {1: "error", 2: "warning", 3: "info", 4: "hint"}
        return cls(
            line=diag["range"]["start"]["line"] + 1,  # Convert to 1-indexed
            character=diag["range"]["start"]["character"],
            severity=severity_map.get(diag.get("severity", 1), "error"),
            message=diag["message"],
            source=diag.get("source", "unknown"),
            code=str(diag["code"]) if diag.get("code") else None,
        )


class BaseLSPClient(ABC):
    """
    Base class for LSP clients.

    Wraps pylspclient for specific language servers.
    """

    language: str = ""
    extensions: list[str] = []

    def __init__(self, root_path: Path | None = None) -> None:
        """
        Initialize LSP client.

        Args:
            root_path: Project root path
        """
        self.root_path = root_path or Path.cwd()
        self._process: subprocess.Popen[bytes] | None = None
        self._endpoint: Any = None  # LspEndpoint
        self._client: Any = None  # LspClient
        self._diagnostics: dict[str, list[Diagnostic]] = {}

    @property
    @abstractmethod
    def command(self) -> list[str]:
        """Command to start the language server."""
        pass

    @property
    def is_available(self) -> bool:
        """Check if LSP libraries and server are available."""
        if not HAS_LSP:
            return False
        # Check if command exists
        try:
            subprocess.run(
                [self.command[0], "--version"],
                capture_output=True,
                timeout=5,
            )
            return True
        except (FileNotFoundError, subprocess.TimeoutExpired):
            return False

    def start(self) -> bool:
        """
        Start the language server.

        Returns:
            True if started successfully
        """
        if not HAS_LSP:
            logger.warning("pylspclient not installed - LSP disabled")
            return False

        if not self.is_available:
            logger.warning(f"{self.language} LSP server not found: {self.command[0]}")
            return False

        try:
            logger.info(f"Starting {self.language} LSP: {' '.join(self.command)}")

            self._process = subprocess.Popen(
                self.command,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

            # Create LSP endpoint and client
            json_rpc_endpoint = pylspclient.JsonRpcEndpoint(
                self._process.stdin,
                self._process.stdout,
            )

            self._endpoint = LspEndpoint(
                json_rpc_endpoint,
                notify_callbacks={
                    "textDocument/publishDiagnostics": self._on_diagnostics,
                },
            )

            self._client = LspClient(self._endpoint)

            # Initialize
            self._client.initialize(
                processId=None,
                rootUri=self.root_path.as_uri(),
                rootPath=str(self.root_path),
                capabilities=self._get_capabilities(),
                initializationOptions=None,
                trace="off",
                workspaceFolders=None,
            )
            self._client.initialized()

            logger.info(f"{self.language} LSP started")
            return True

        except Exception as e:
            logger.error(f"Failed to start {self.language} LSP: {e}")
            self.stop()
            return False

    def stop(self) -> None:
        """Stop the language server."""
        if self._client:
            try:
                self._client.shutdown()
                self._client.exit()
            except Exception:
                pass

        if self._process:
            self._process.terminate()
            try:
                self._process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self._process.kill()

        self._process = None
        self._endpoint = None
        self._client = None
        logger.info(f"{self.language} LSP stopped")

    def _get_capabilities(self) -> dict[str, Any]:
        """Get client capabilities."""
        return {
            "textDocument": {
                "publishDiagnostics": {"relatedInformation": True},
                "synchronization": {"didSave": True},
            },
        }

    def _on_diagnostics(self, params: dict[str, Any]) -> None:
        """Handle diagnostics notification."""
        uri = params["uri"]
        self._diagnostics[uri] = [Diagnostic.from_lsp(d) for d in params.get("diagnostics", [])]
        logger.debug(f"Received {len(self._diagnostics[uri])} diagnostics for {uri}")

    def open_file(self, path: Path) -> None:
        """Notify server that a file was opened."""
        if not self._client:
            return

        try:
            content = path.read_text()
            uri = path.as_uri()

            self._client.didOpen(
                {
                    "uri": uri,
                    "languageId": self.language,
                    "version": 1,
                    "text": content,
                }
            )
        except Exception as e:
            logger.error(f"Failed to open file in LSP: {e}")

    def get_diagnostics(self, path: Path) -> list[Diagnostic]:
        """Get diagnostics for a file."""
        uri = path.as_uri()
        return self._diagnostics.get(uri, [])

    def get_all_diagnostics(self) -> dict[Path, list[Diagnostic]]:
        """Get all diagnostics."""
        result = {}
        for uri, diags in self._diagnostics.items():
            # Convert file:// URI to Path
            if uri.startswith("file://"):
                path = Path(uri[7:])
                result[path] = diags
        return result
