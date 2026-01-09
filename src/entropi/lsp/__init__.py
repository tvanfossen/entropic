"""
LSP integration for code intelligence.

Currently supports Python (pyright) and C (clangd).

Requirements:
    pip install pylspclient
"""

from entropi.lsp.base import BaseLSPClient, Diagnostic
from entropi.lsp.clangd_client import ClangdClient
from entropi.lsp.manager import LSPManager
from entropi.lsp.pyright_client import PyrightClient

__all__ = [
    "BaseLSPClient",
    "ClangdClient",
    "Diagnostic",
    "LSPManager",
    "PyrightClient",
]
