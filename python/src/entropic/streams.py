# SPDX-License-Identifier: LGPL-3.0-or-later
"""Pythonic facade for entropic engine stream observers (#8, v2.1.4).

The C ABI ``entropic_set_stream_observer(handle, cb, user_data)``
installs a persistent observer that fires for every streaming token
the engine emits, regardless of which entry point invoked the model.
This module wraps it so consumers don't have to manage CFUNCTYPE
lifetime themselves.

Usage
-----

::

    import entropic

    def on_token(text: str) -> None:
        print(text, end="", flush=True)

    handle = entropic.create()
    entropic.register_token_observer(handle, on_token)
    entropic.run_streaming(handle, "...")

@version 2.1.4
"""

from __future__ import annotations

import ctypes
from typing import Callable

from entropic._bindings import (
    STREAM_OBSERVER_CB,
    entropic_set_stream_observer,
)

TokenObserver = Callable[[str], None]
"""Observer callable: receives each token as a Python str (UTF-8)."""

# Per-handle keepalive: stores the CFUNCTYPE wrapper so it survives as
# long as the handle does. Without this Python's GC would reap the
# wrapper after register_token_observer returns and the engine's next
# stream emit would call into freed memory.
_observers: dict[int, object] = {}


## @brief Install a Python token observer on an engine handle.
## @utility
## @version 2.1.4
def register_token_observer(handle, observer: TokenObserver) -> None:
    """Wire ``observer`` as the engine's persistent stream observer.

    Replaces any previously-installed observer on the same handle.
    Calling with ``observer=None`` is not supported here — use the
    raw entropic_set_stream_observer with NULL to clear.

    Args:
        handle: An entropic_handle_t from entropic_create.
        observer: Callable invoked once per emitted token. Receives a
                  Python str (UTF-8 decoded). Return value ignored.
                  Exceptions are caught and printed to stderr — they
                  must NOT escape into the C engine.

    @version 2.1.4
    """

    def _trampoline(token_ptr, length, _user_data):  # noqa: ARG001
        try:
            if not token_ptr or length == 0:
                return
            data = ctypes.string_at(token_ptr, length)
            observer(data.decode("utf-8", errors="replace"))
        except Exception:
            import traceback

            traceback.print_exc()

    cb = STREAM_OBSERVER_CB(_trampoline)
    handle_key = ctypes.cast(handle, ctypes.c_void_p).value or 0
    _observers[handle_key] = cb  # keepalive
    entropic_set_stream_observer(handle, cb, None)
