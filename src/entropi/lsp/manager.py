"""
LSP Manager for coordinating language servers.

Manages lifecycle and routing to appropriate language servers.
"""

from pathlib import Path
from typing import TYPE_CHECKING

from entropi.core.logging import get_logger
from entropi.lsp.base import BaseLSPClient, Diagnostic
from entropi.lsp.clangd_client import ClangdClient
from entropi.lsp.pyright_client import PyrightClient

if TYPE_CHECKING:
    from entropi.config.schema import LSPConfig

logger = get_logger("lsp.manager")


class LSPManager:
    """
    Manages LSP clients for multiple languages.

    Auto-detects project languages and starts appropriate servers.
    """

    def __init__(self, config: "LSPConfig", root_path: Path | None = None) -> None:
        """
        Initialize LSP manager.

        Args:
            config: LSP configuration
            root_path: Project root path
        """
        self.config = config
        self.root_path = root_path or Path.cwd()
        self._clients: dict[str, BaseLSPClient] = {}

    @property
    def is_enabled(self) -> bool:
        """Check if LSP is enabled."""
        return self.config.enabled

    def start(self) -> None:
        """Start configured language servers."""
        if not self.config.enabled:
            logger.info("LSP disabled in config")
            return

        # Start Python LSP if enabled
        if self.config.python_enabled:
            client = PyrightClient(self.root_path)
            if client.start():
                self._clients["python"] = client

        # Start C LSP if enabled
        if self.config.c_enabled:
            client = ClangdClient(self.root_path)
            if client.start():
                self._clients["c"] = client

        logger.info(f"LSP started: {list(self._clients.keys())}")

    def stop(self) -> None:
        """Stop all language servers."""
        for name, client in self._clients.items():
            logger.debug(f"Stopping {name} LSP")
            client.stop()
        self._clients.clear()
        logger.info("LSP stopped")

    def get_client_for_file(self, path: Path) -> BaseLSPClient | None:
        """
        Get the LSP client for a file based on extension.

        Args:
            path: File path

        Returns:
            LSP client or None
        """
        suffix = path.suffix.lower()

        # Check Python
        if "python" in self._clients:
            if suffix in PyrightClient.extensions:
                return self._clients["python"]

        # Check C
        if "c" in self._clients:
            if suffix in ClangdClient.extensions:
                return self._clients["c"]

        return None

    def open_file(self, path: Path) -> None:
        """Open a file in the appropriate LSP server."""
        client = self.get_client_for_file(path)
        if client:
            client.open_file(path)

    def get_diagnostics(self, path: Path) -> list[Diagnostic]:
        """
        Get diagnostics for a file.

        Args:
            path: File path

        Returns:
            List of diagnostics
        """
        client = self.get_client_for_file(path)
        if client:
            return client.get_diagnostics(path)
        return []

    def get_all_diagnostics(self) -> dict[Path, list[Diagnostic]]:
        """
        Get diagnostics from all language servers.

        Returns:
            Dict mapping paths to diagnostics
        """
        result: dict[Path, list[Diagnostic]] = {}
        for client in self._clients.values():
            result.update(client.get_all_diagnostics())
        return result

    def get_errors(self, path: Path) -> list[Diagnostic]:
        """
        Get only error-level diagnostics for a file.

        Args:
            path: File path

        Returns:
            List of error diagnostics
        """
        return [d for d in self.get_diagnostics(path) if d.is_error]

    def has_errors(self, path: Path) -> bool:
        """Check if a file has any errors."""
        return len(self.get_errors(path)) > 0

    def get_supported_extensions(self) -> set[str]:
        """Get all file extensions supported by active LSP clients."""
        extensions: set[str] = set()
        for client in self._clients.values():
            extensions.update(client.extensions)
        return extensions

    def supports_file(self, path: Path) -> bool:
        """Check if a file type is supported by any active LSP client."""
        return path.suffix.lower() in self.get_supported_extensions()

    async def wait_for_diagnostics(self, path: Path, timeout: float = 2.0) -> list[Diagnostic]:
        """
        Wait for diagnostics to be published for a file.

        Delegates to the appropriate client based on file extension.

        Args:
            path: File path
            timeout: Maximum wait time in seconds

        Returns:
            List of diagnostics (may be empty if timeout reached)
        """
        client = self.get_client_for_file(path)
        if client:
            return await client.wait_for_diagnostics(path, timeout)
        return []
