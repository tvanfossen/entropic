"""
C/C++ LSP client using clangd.

Provides C/C++ error detection, type checking, and code intelligence.

Install: sudo apt install clangd (or brew install llvm)
"""

import shutil

from entropic.lsp.base import BaseLSPClient


class ClangdClient(BaseLSPClient):
    """
    C/C++ LSP client using clangd.

    Clangd provides code intelligence for C and C++ projects.
    """

    language = "c"
    extensions = [".c", ".h", ".cpp", ".hpp", ".cc", ".cxx"]

    @property
    def command(self) -> list[str]:
        """Command to start clangd language server."""
        return ["clangd"]

    @property
    def is_available(self) -> bool:
        """Check if clangd is available."""
        from entropic.lsp.base import HAS_LSP

        if not HAS_LSP:
            return False

        # Check if clangd exists in PATH
        # Some systems may have clangd-XX (version suffixed)
        for cmd in ["clangd", "clangd-18", "clangd-17", "clangd-16", "clangd-15"]:
            if shutil.which(cmd):
                return True
        return False
