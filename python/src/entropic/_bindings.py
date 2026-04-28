# SPDX-License-Identifier: LGPL-3.0-or-later
"""ctypes bindings for librentropic.so.

This module exposes the entropic C ABI as a flat collection of ctypes
function objects. It is *not* an OOP wrapper — symbols match the C
header verbatim (``entropic_create``, ``entropic_run_streaming``, etc.).

Bindings are intentionally a hand-curated subset of the full surface
declared in ``include/entropic/entropic.h``. The full surface can be
regenerated via ``inv gen-bindings``; this hand-written file pins the
symbols most consumers need at v2.1.0 release time and keeps the
wheel small (~50 KB).

For symbols not exposed here, load librentropic.so directly:

    from entropic._loader import load
    lib = load()
    lib.entropic_some_other_symbol.argtypes = [...]
"""

from __future__ import annotations

import ctypes
import enum

from entropic._loader import load

_lib = load()

# ── Opaque handle ────────────────────────────────────────────────────────
# The C header types entropic_handle_t as `struct entropic_engine*`, an
# opaque forward declaration. From Python it's just a void* — callers
# pass `ctypes.byref(handle)` for out-params and `handle` for ins.
entropic_handle_t = ctypes.c_void_p


# ── Enums ────────────────────────────────────────────────────────────────
class EntropicError(enum.IntEnum):
    """Mirrors ``entropic_error_t`` from include/entropic/types/error.h."""

    OK = 0
    INVALID_HANDLE = 1
    INVALID_STATE = 2
    INVALID_ARGUMENT = 3
    GENERATE_FAILED = 4
    OUT_OF_MEMORY = 5
    NOT_FOUND = 6
    ALREADY_EXISTS = 7
    PERMISSION_DENIED = 8
    IO_ERROR = 9
    UNKNOWN = 99


class AgentState(enum.IntEnum):
    """Mirrors ``entropic_agent_state_t`` from entropic.h."""

    IDLE = 0
    GENERATING = 1
    EXECUTING = 2
    VERIFYING = 3
    COMPLETE = 4
    INTERRUPTED = 5
    ERROR = 6


# ── Callback signatures (CFUNCTYPE) ──────────────────────────────────────
# Each named here has a stable address; reusing the same CFUNCTYPE across
# bindings ensures function pointers we hand to C remain alive as long
# as the holding Python reference does.
TOKEN_CB = ctypes.CFUNCTYPE(None, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p)
"""Stream callback: (token: bytes, len: size_t, user_data: void*) -> None."""

STATE_OBSERVER_CB = ctypes.CFUNCTYPE(None, ctypes.c_int, ctypes.c_void_p)
"""State observer: (state: AgentState, user_data: void*) -> None."""


## @brief Pin restype/argtypes on a symbol from the loaded library.
## @utility
## @version 2.1.0
def _bind(name: str, restype, *argtypes):
    """Pin restype/argtypes on a symbol from the loaded library."""
    fn = getattr(_lib, name)
    fn.restype = restype
    fn.argtypes = list(argtypes)
    return fn


# ── Lifecycle ────────────────────────────────────────────────────────────
entropic_create = _bind("entropic_create", ctypes.c_int, ctypes.POINTER(entropic_handle_t))
entropic_destroy = _bind("entropic_destroy", None, entropic_handle_t)

entropic_configure_dir = _bind(
    "entropic_configure_dir",
    ctypes.c_int,
    entropic_handle_t,
    ctypes.c_char_p,
)

# ── Version / API ────────────────────────────────────────────────────────
entropic_api_version = _bind("entropic_api_version", ctypes.c_int)
entropic_version = _bind("entropic_version", ctypes.c_char_p)

# ── Inference ────────────────────────────────────────────────────────────
entropic_run = _bind(
    "entropic_run",
    ctypes.c_int,
    entropic_handle_t,
    ctypes.c_char_p,
    ctypes.POINTER(ctypes.c_char_p),
)
"""(handle, input, out_result_json) → entropic_error_t.

The C function allocates ``*out_result_json`` via malloc; callers must
free it via :data:`entropic_free`.
"""

entropic_run_streaming = _bind(
    "entropic_run_streaming",
    ctypes.c_int,
    entropic_handle_t,
    ctypes.c_char_p,
    TOKEN_CB,
    ctypes.c_void_p,
    ctypes.POINTER(ctypes.c_int),
)

entropic_interrupt = _bind("entropic_interrupt", ctypes.c_int, entropic_handle_t)

entropic_free = _bind("entropic_free", None, ctypes.c_void_p)

# ── Conversation context ─────────────────────────────────────────────────
entropic_context_clear = _bind("entropic_context_clear", ctypes.c_int, entropic_handle_t)
entropic_context_count = _bind(
    "entropic_context_count",
    ctypes.c_int,
    entropic_handle_t,
    ctypes.POINTER(ctypes.c_size_t),
)

# ── Observers ────────────────────────────────────────────────────────────
entropic_set_state_observer = _bind(
    "entropic_set_state_observer",
    ctypes.c_int,
    entropic_handle_t,
    STATE_OBSERVER_CB,
    ctypes.c_void_p,
)
