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

from typing import Any

__version__ = "2.1.0"

_LAZY_EXPORTS = frozenset(
    {
        "AgentState",
        "EntropicError",
        "STATE_OBSERVER_CB",
        "TOKEN_CB",
        "entropic_api_version",
        "entropic_configure_dir",
        "entropic_context_clear",
        "entropic_context_count",
        "entropic_create",
        "entropic_destroy",
        "entropic_free",
        "entropic_handle_t",
        "entropic_interrupt",
        "entropic_run",
        "entropic_run_streaming",
        "entropic_set_state_observer",
        "entropic_version",
    }
)

__all__ = ["__version__", *sorted(_LAZY_EXPORTS)]


## @brief PEP 562 lazy attribute lookup importing _bindings on demand.
## @utility
## @version 2.1.0
def __getattr__(name: str) -> Any:
    """PEP 562 lazy attribute lookup that imports _bindings on demand."""
    if name in _LAZY_EXPORTS:
        from entropic import _bindings

        return getattr(_bindings, name)
    raise AttributeError(f"module 'entropic' has no attribute {name!r}")
