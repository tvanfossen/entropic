# SPDX-License-Identifier: LGPL-3.0-or-later
"""entropic-engine — Python wrapper over librentropic.so.

The package itself is ~50 KB of pure Python: a ctypes binding shim and
a small CLI. The native engine binary is fetched on demand via::

    pip install entropic-engine
    entropic install-engine     # downloads the matching tarball

For programmatic usage, import the C ABI symbols directly::

    from entropic import (
        entropic_create, entropic_configure_dir,
        entropic_run_streaming, entropic_destroy,
        EntropicError, AgentState, TOKEN_CB,
    )

There is no ``EntropicEngine`` class. v1.7.x shipped one; v2.x is a flat
binding because the C ABI is the public surface and an OOP layer would
just lag behind it.

Imports of binding symbols are *lazy* (PEP 562) so that
``entropic install-engine`` — which runs *before* ``librentropic.so``
exists on disk — can succeed without triggering library load.
"""

from __future__ import annotations

from importlib.metadata import PackageNotFoundError, version
from typing import Any

# v2.1.2 (#4): single source of truth for version is the repo-root
# VERSION file, which pyproject.toml reads via dynamic version. After
# install (wheel or editable), importlib.metadata returns that same
# value. The except branch handles the rare case of running directly
# from the source tree without an install (e.g. ``PYTHONPATH=python/src
# python -c "import entropic"``); we fall back to reading VERSION
# directly so __version__ is never silently wrong.
try:
    __version__ = version("entropic-engine")
except PackageNotFoundError:  # pragma: no cover — uninstalled source-tree run
    from pathlib import Path

    _version_file = Path(__file__).resolve().parents[3] / "VERSION"
    __version__ = _version_file.read_text().strip() if _version_file.exists() else "0.0.0+local"

_LAZY_EXPORTS = frozenset(
    {
        "AgentState",
        "EntropicError",
        # Issue #8 (v2.1.4): EntropicHookPoint enum + 4 new ABI symbols.
        "EntropicHookPoint",
        "HOOK_CB",
        "STATE_OBSERVER_CB",
        "STREAM_OBSERVER_CB",
        "TOKEN_CB",
        "entropic_alloc",
        "entropic_api_version",
        "entropic_configure_dir",
        "entropic_context_clear",
        "entropic_context_count",
        "entropic_create",
        "entropic_destroy",
        "entropic_free",
        "entropic_handle_t",
        "entropic_interrupt",
        "entropic_register_hook",
        "entropic_register_mcp_server",
        "entropic_run",
        "entropic_run_streaming",
        "entropic_set_state_observer",
        "entropic_set_stream_observer",
        "entropic_version",
    }
)

# Issue #8 (v2.1.4): Pythonic facade — top-level re-exports of the
# decorator + helpers from the per-module surfaces. Keeps `entropic`
# the canonical import for application code while preserving the
# per-module imports for advanced consumers.
_FACADE_EXPORTS = frozenset(
    {
        # entropic.hooks
        "hook",
        "register_hooks",
        # entropic.streams
        "register_token_observer",
        # entropic.mcp
        "register_server",
    }
)

__all__ = ["__version__", *sorted(_LAZY_EXPORTS | _FACADE_EXPORTS)]


_FACADE_MODULE_FOR = {
    "hook": "entropic.hooks",
    "register_hooks": "entropic.hooks",
    "register_token_observer": "entropic.streams",
    "register_server": "entropic.mcp",
}


## @brief PEP 562 lazy attribute lookup importing _bindings on demand.
## @utility
## @version 2.1.4
def __getattr__(name: str) -> Any:
    """PEP 562 lazy attribute lookup that imports _bindings on demand."""
    if name in _LAZY_EXPORTS:
        from entropic import _bindings

        return getattr(_bindings, name)
    if name in _FACADE_MODULE_FOR:
        from importlib import import_module

        return getattr(import_module(_FACADE_MODULE_FOR[name]), name)
    raise AttributeError(f"module 'entropic' has no attribute {name!r}")
