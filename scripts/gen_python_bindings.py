#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Generate ``python/src/entropic/_bindings.py`` from ``entropic.h``.

Approach:
    * Multi-line regex of ``ENTROPIC_EXPORT <ret> <name>(<args>);`` to
      pick up function declarations.
    * Map a small fixed set of C types to ``ctypes`` types; any
      function whose signature mentions an unmapped type is *skipped*
      with a warning to stderr (caller can hand-wire it in
      ``_bindings_extra.py`` or extend this map).
    * Emit a top-of-file curated handful of enums + callback CFUNCTYPE
      objects; the parser does not attempt to walk every enum block in
      the header.

The output is intended to be checked into the repo at release time.
The hand-written ``_bindings.py`` shipping in v2.1.0 is the reference
shape; this generator reproduces that shape from the header so future
versions don't drift.

Usage::

    scripts/gen_python_bindings.py \\
        --header include/entropic/entropic.h \\
        --out python/src/entropic/_bindings.py
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# ── C → ctypes type map ────────────────────────────────────────────────
_TYPE_MAP = {
    "void": "None",
    "int": "ctypes.c_int",
    "size_t": "ctypes.c_size_t",
    "const char*": "ctypes.c_char_p",
    "char*": "ctypes.c_char_p",
    "char**": "ctypes.POINTER(ctypes.c_char_p)",
    "void*": "ctypes.c_void_p",
    "int*": "ctypes.POINTER(ctypes.c_int)",
    "size_t*": "ctypes.POINTER(ctypes.c_size_t)",
    "entropic_handle_t": "entropic_handle_t",
    "entropic_handle_t*": "ctypes.POINTER(entropic_handle_t)",
    "entropic_error_t": "ctypes.c_int",
}

# Functions whose signatures contain inline function-pointer types or
# struct-by-value parameters cannot be auto-bound. These are listed by
# name and emitted as a TODO comment so a human can hand-wire them.
_INLINE_CB_FUNCTIONS = frozenset(
    {
        "entropic_run_streaming",
        "entropic_set_state_observer",
        "entropic_set_stream_observer",
        "entropic_register_hook",
        "entropic_deregister_hook",
    }
)

_DECL_RE = re.compile(
    r"ENTROPIC_EXPORT\s+([\w\s\*]+?)\s+(entropic_\w+)\s*\(([^)]*)\)\s*;",
    re.MULTILINE | re.DOTALL,
)


## @brief Collapse whitespace, fold `* x` → `*`, strip qualifiers we ignore.
## @utility
## @version 2.1.0
def _normalize_type(raw: str) -> str:
    """Collapse whitespace, fold `* x` → `*`, strip qualifiers we ignore."""
    t = raw.strip()
    t = re.sub(r"\s+", " ", t)
    t = re.sub(r"\s*\*", "*", t)
    return t


## @brief Split a top-level argument list (no inline structs in this surface).
## @utility
## @version 2.1.0
def _split_args(arglist: str) -> list[str]:
    """Split a top-level argument list, respecting nothing fancy.

    The header has no inline structs in the args of bindable
    functions — those are filtered into _INLINE_CB_FUNCTIONS — so a
    plain comma split is sufficient.
    """
    arglist = arglist.strip()
    if not arglist or arglist == "void":
        return []
    return [a.strip() for a in arglist.split(",")]


## @brief Drop the parameter name and return the ctypes string, or None.
## @utility
## @version 2.1.0
def _arg_type(arg: str) -> str | None:
    """Drop the parameter name and return the ctypes string, or None."""
    arg = re.sub(r"/\*.*?\*/", "", arg).strip()
    parts = arg.rsplit(maxsplit=1)
    raw = _normalize_type(parts[0] if len(parts) == 2 else arg)
    return _TYPE_MAP.get(raw)


## @brief Translate args; return (py_args, None) on success or (None, reason).
## @utility
## @version 2.1.0
def _try_translate_args(args: list[str]) -> tuple[list[str] | None, str | None]:
    """Translate args; return (py_args, None) on success or (None, reason)."""
    py_args: list[str] = []
    for a in args:
        t = _arg_type(a)
        if t is None:
            return None, a
        py_args.append(t)
    return py_args, None


## @brief Return a comment string if this function should be skipped, else None.
## @utility
## @version 2.1.0
def _skip_reason(name: str, ret_norm: str, py_ret, bad_arg) -> str | None:
    """Return a comment string if this function should be skipped, else None."""
    reasons = []
    if name in _INLINE_CB_FUNCTIONS:
        reasons.append("hand-wired (inline function-pointer in signature)")
    elif py_ret is None:
        reasons.append(f"skipped (unknown return type {ret_norm!r})")
    elif bad_arg is not None:
        reasons.append(f"skipped (unknown arg type in {bad_arg!r})")
    return f"# {name}: {reasons[0]}" if reasons else None


## @brief Return a Python `_bind(...)` line, or a comment if untranslatable.
## @utility
## @version 2.1.0
def _emit_function(name: str, ret: str, args: list[str]) -> str:
    """Return a Python ``_bind(...)`` line, or a comment if untranslatable."""
    ret_norm = _normalize_type(ret)
    py_ret = _TYPE_MAP.get(ret_norm)
    py_args, bad_arg = _try_translate_args(args)
    skip = _skip_reason(name, ret_norm, py_ret, bad_arg)
    if skip is not None:
        return skip
    # _skip_reason guarantees py_ret/py_args are non-None when skip is None.
    parts: list[str] = [f'"{name}"', str(py_ret), *(py_args or [])]
    return f"{name} = _bind({', '.join(parts)})"


## @brief Return a list of (name, return_type, args) from the header.
## @utility
## @version 2.1.0
def _parse_header(header_path: Path) -> list[tuple[str, str, list[str]]]:
    """Return a list of ``(name, return_type, args)`` from the header."""
    text = header_path.read_text()
    out: list[tuple[str, str, list[str]]] = []
    for match in _DECL_RE.finditer(text):
        ret_raw, name, args_raw = match.groups()
        # Strip leading qualifiers (static/const) the regex caught.
        ret = re.sub(r"^\s*(static|inline)\s+", "", ret_raw).strip()
        out.append((name, ret, _split_args(args_raw)))
    return out


_PROLOGUE = '''# SPDX-License-Identifier: LGPL-3.0-or-later
# AUTO-GENERATED by scripts/gen_python_bindings.py — do not edit.
"""ctypes bindings for librentropic.so (auto-generated)."""
from __future__ import annotations

import ctypes
import enum

from entropic._loader import load

_lib = load()
entropic_handle_t = ctypes.c_void_p


class EntropicError(enum.IntEnum):
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
    IDLE = 0
    GENERATING = 1
    EXECUTING = 2
    VERIFYING = 3
    COMPLETE = 4
    INTERRUPTED = 5
    ERROR = 6


TOKEN_CB = ctypes.CFUNCTYPE(
    None, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p
)
STATE_OBSERVER_CB = ctypes.CFUNCTYPE(None, ctypes.c_int, ctypes.c_void_p)


def _bind(name, restype, *argtypes):
    fn = getattr(_lib, name)
    fn.restype = restype
    fn.argtypes = list(argtypes)
    return fn


'''


## @brief Parse args, generate bindings, exit code.
## @utility
## @version 2.1.0
def main() -> int:
    """Parse args, generate bindings, exit code."""
    ap = argparse.ArgumentParser(description="Generate _bindings.py from entropic.h")
    ap.add_argument("--header", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()
    decls = _parse_header(args.header)
    if not decls:
        print(f"error: no ENTROPIC_EXPORT declarations found in {args.header}", file=sys.stderr)
        return 1
    body_lines = [_emit_function(name, ret, a) for (name, ret, a) in decls]
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(_PROLOGUE + "\n".join(b for b in body_lines if b) + "\n")
    print(f"  wrote {len(decls)} declarations → {args.out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
