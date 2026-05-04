# SPDX-License-Identifier: LGPL-3.0-or-later
"""Pythonic facade for entropic engine hooks (#8, v2.1.4).

The C ABI for hooks is callable from Python via ``_bindings.HOOK_CB``
and ``_bindings.entropic_register_hook``, but using it directly carries
two foot-guns:

1. **Lifetime**: a CFUNCTYPE-wrapped Python callable must be referenced
   by Python for as long as the C engine might invoke it. A bare
   ``entropic_register_hook(handle, point, HOOK_CB(my_func), None, 0)``
   drops the CFUNCTYPE wrapper as soon as the call returns —
   subsequent invocations from C dereference freed memory. This module
   owns a per-handle registry that keeps wrappers alive.

2. **Marshalling**: the C ABI hands you ``context_json`` as a JSON
   string and expects ``modified_json`` as a heap-allocated C string
   you produce via ``entropic_alloc``. This module marshals dict ↔
   JSON for you and uses ``entropic_alloc`` so the engine's free()
   sees a heap pointer of the right provenance.

Usage
-----

::

    import entropic
    from entropic import EntropicHookPoint as Hook

    @entropic.hook(Hook.PRE_TOOL_CALL)
    def gate_dangerous_tools(ctx: dict) -> dict | None:
        # Return a dict to transform the call (post-hooks) or block it
        # (pre-hooks). Return None to pass through untouched.
        if ctx.get("tool_name", "").startswith("filesystem.write"):
            return {"reason": "no writes during readonly session"}
        return None

    handle = entropic.create()
    entropic.configure(handle, ...)
    entropic.register_hooks(handle)   # binds all decorated callables
    entropic.run(handle, "...")

@version 2.1.4
"""

from __future__ import annotations

import ctypes
import json
from dataclasses import dataclass
from typing import Any, Callable

from entropic._bindings import (
    HOOK_CB,
    EntropicHookPoint,
    entropic_alloc,
    entropic_register_hook,
)

# Public hook-callable signature: receives the parsed context dict, returns
# either None (no-op / OK) or a dict that becomes the modified_json
# transform. For pre-hooks, returning a dict signals CANCEL with the dict
# as feedback; for post-hooks, the dict replaces the result content.
HookCallable = Callable[[dict[str, Any]], dict[str, Any] | None]


@dataclass
class _HookEntry:
    """One registered hook. Stored in the per-handle registry."""

    point: EntropicHookPoint
    func: HookCallable
    priority: int = 0
    # The CFUNCTYPE-wrapped trampoline. Held here so it survives as long
    # as the engine handle does — without this Python would GC the
    # trampoline after register_hooks returns and the next C-side
    # invocation would dereference freed memory.
    trampoline: Any = None


# Module-level decorator registry (populated at import time as @hook
# decorators run; consumed by register_hooks(handle)).
_pending: list[_HookEntry] = []
# Per-handle registry of bound hooks; keeps trampolines alive.
_bound: dict[int, list[_HookEntry]] = {}


## @brief Register a Python function as an entropic engine hook.
## @utility
## @version 2.1.4
def hook(
    point: EntropicHookPoint,
    *,
    priority: int = 0,
) -> Callable[[HookCallable], HookCallable]:
    """Decorator: mark a function as an entropic hook for ``point``.

    The function is added to a module-level pending list; call
    :func:`register_hooks` after engine handle creation to bind every
    pending hook to that handle. Decorating after register_hooks does
    NOT auto-bind to past handles — call register_hooks again for new
    handles.

    Args:
        point: Hook point from :class:`EntropicHookPoint`.
        priority: Resolution order (higher fires first within a point).

    Returns:
        The undecorated function (the registry holds the binding).

    @version 2.1.4
    """

    def decorator(func: HookCallable) -> HookCallable:
        _pending.append(_HookEntry(point=point, func=func, priority=priority))
        return func

    return decorator


## @brief C-allocate and copy a string for the engine to free().
## @utility
## @version 2.1.4
def _alloc_c_string(s: str) -> ctypes.c_void_p:
    """Allocate via entropic_alloc and copy a UTF-8 string into it.

    Returns a void* the engine will free() after consuming. Caller is
    responsible for ensuring no Python reference outlives that free.
    """
    encoded = s.encode("utf-8") + b"\x00"
    ptr = entropic_alloc(len(encoded))
    if not ptr:
        return None
    ctypes.memmove(ptr, encoded, len(encoded))
    return ptr


## @brief Build the CFUNCTYPE trampoline for one Python hook callable.
## @utility
## @version 2.1.4
def _build_trampoline(func: HookCallable):
    """Wrap a Pythonic hook callable as a HOOK_CB CFUNCTYPE pointer.

    The trampoline is responsible for:
    - JSON-decoding context_json into a dict
    - Calling the user function
    - JSON-encoding any returned dict and writing to *modified_json via
      entropic_alloc (engine free()s it)
    - Mapping return: None / non-None to 0 / 1 (pass / cancel-or-transform)
    """

    def _trampoline(point, ctx_ptr, mod_out_ptr, _user_data):  # noqa: ARG001
        rc = 0
        try:
            ctx_str = ctypes.string_at(ctx_ptr).decode("utf-8") if ctx_ptr else ""
            ctx = json.loads(ctx_str) if ctx_str else {}
            result = func(ctx)
            if result is not None:
                payload = json.dumps(result).encode("utf-8") + b"\x00"
                ptr = entropic_alloc(len(payload))
                if ptr:
                    ctypes.memmove(ptr, payload, len(payload))
                    mod_out_ptr[0] = ctypes.cast(ptr, ctypes.c_char_p)
                rc = 1
        except Exception:
            # Never let a Python exception escape into the C engine —
            # log to stderr and pass-through. Future: pluggable logger.
            import traceback

            traceback.print_exc()
            rc = 0
        return rc

    return HOOK_CB(_trampoline)


## @brief Bind every @hook-decorated function to an engine handle.
## @utility
## @version 2.1.4
def register_hooks(handle) -> int:
    """Bind every pending @hook-decorated callable to ``handle``.

    The trampoline references are stored in a per-handle list so they
    survive as long as the handle does. Subsequent calls register
    only NEW pending entries (idempotent for already-bound handles).

    Args:
        handle: An entropic_handle_t from entropic_create.

    Returns:
        Number of hooks bound on this call.

    @version 2.1.4
    """
    handle_key = ctypes.cast(handle, ctypes.c_void_p).value or 0
    bound_for_handle = _bound.setdefault(handle_key, [])
    bound = 0
    for entry in _pending:
        if any(b.func is entry.func and b.point == entry.point for b in bound_for_handle):
            continue
        entry.trampoline = _build_trampoline(entry.func)
        rc = entropic_register_hook(
            handle,
            int(entry.point),
            entry.trampoline,
            None,
            entry.priority,
        )
        if rc == 0:
            bound_for_handle.append(entry)
            bound += 1
    return bound


## @brief Forget all decorated hooks (test surface).
## @utility
## @version 2.1.4
def _clear_pending() -> None:
    """Test-only helper: clear the module-level pending registry."""
    _pending.clear()
