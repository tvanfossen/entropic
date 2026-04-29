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
    """Mirrors ``entropic_error_t`` from include/entropic/types/error.h.

    Same drift hazard as :class:`AgentState`: an IntEnum that doesn't match
    the C ABI silently mislabels error codes (issue #1: pre-2.1.1 wrapper
    had 11 entries against 47 in the C header, and the entries it did have
    were misaligned by integer value). ``tests/unit/test_bindings_abi.py``
    pins these against the canonical header — do not edit one without
    the other.
    """

    OK = 0
    INVALID_ARGUMENT = 1
    INVALID_CONFIG = 2
    INVALID_STATE = 3
    MODEL_NOT_FOUND = 4
    LOAD_FAILED = 5
    GENERATE_FAILED = 6
    TOOL_NOT_FOUND = 7
    PERMISSION_DENIED = 8
    PLUGIN_VERSION_MISMATCH = 9
    PLUGIN_LOAD_FAILED = 10
    TIMEOUT = 11
    CANCELLED = 12
    OUT_OF_MEMORY = 13
    IO = 14
    INTERNAL = 15
    SERVER_ALREADY_EXISTS = 16
    SERVER_NOT_FOUND = 17
    CONNECTION_FAILED = 18
    INVALID_HANDLE = 19
    TOOL_EXECUTION_FAILED = 20
    STORAGE_FAILED = 21
    IDENTITY_NOT_FOUND = 22
    ALREADY_RUNNING = 23
    NOT_RUNNING = 24
    NOT_IMPLEMENTED = 25
    INTERRUPTED = 26
    ADAPTER_NOT_FOUND = 27
    ADAPTER_LOAD_FAILED = 28
    ADAPTER_SWAP_FAILED = 29
    ADAPTER_CANCELLED = 30
    GRAMMAR_NOT_FOUND = 31
    GRAMMAR_INVALID = 32
    MCP_KEY_DENIED = 33
    LIMIT_REACHED = 34
    ALREADY_EXISTS = 35
    IN_USE = 36
    PROFILE_NOT_FOUND = 37
    TIME_LIMIT_EXCEEDED = 38
    VALIDATION_FAILED = 39
    COMPACTION_FAILED = 40
    MODEL_NOT_ACTIVE = 41
    EVAL_CONTEXT_FULL = 42
    EVAL_FAILED = 43
    IMAGE_LOAD_FAILED = 44
    IMAGE_TOO_LARGE = 45
    MMPROJ_LOAD_FAILED = 46
    UNSUPPORTED_URL = 47
    NOT_SUPPORTED = 48
    STATE_INCOMPATIBLE = 49


class AgentState(enum.IntEnum):
    """Mirrors ``entropic_agent_state_t`` from include/entropic/types/enums.h.

    Drift from the C header silently mislabels every state from 1 onward,
    since IntEnum maps by integer value. ``tests/unit/test_bindings_abi.py``
    parses the canonical header and asserts every (name, value) pair here
    matches; do not edit one without the other.
    """

    IDLE = 0
    PLANNING = 1
    EXECUTING = 2
    WAITING_TOOL = 3
    VERIFYING = 4
    DELEGATING = 5
    COMPLETE = 6
    ERROR = 7
    INTERRUPTED = 8
    PAUSED = 9


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
