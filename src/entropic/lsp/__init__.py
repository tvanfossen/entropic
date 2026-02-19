"""
LSP integration for code intelligence.

Currently supports Python (pyright) and C (clangd).

Requirements:
    pip install pylspclient
"""

from entropic.lsp.base import BaseLSPClient, Diagnostic
from entropic.lsp.clangd_client import ClangdClient
from entropic.lsp.manager import LSPManager
from entropic.lsp.pyright_client import PyrightClient

__all__ = [
    "BaseLSPClient",
    "ClangdClient",
    "Diagnostic",
    "LSPManager",
    "PyrightClient",
]
