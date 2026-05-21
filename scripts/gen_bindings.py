# SPDX-License-Identifier: Apache-2.0
"""Auto-generate python/src/entropic/_bindings.py from the C header.

@brief Full-fidelity C → Python ctypes binding generator.
@version 2.2.1

Replaces the partial regex generator that lived in tasks.py through
v2.2.0 (which skipped inline function-pointer params and hard-coded a
stale EntropicError prologue). Reads:

  include/entropic/types/error.h    → EntropicError
  include/entropic/types/enums.h    → AgentState + 3 more enums
  include/entropic/types/hooks.h    → EntropicHookPoint + HOOK_CB
  include/entropic/entropic.h       → ent_decision_t, structs, callbacks,
                                       all ENTROPIC_EXPORT functions

Emits:

  python/src/entropic/_bindings.py          (fully generated)
  python/src/entropic/_bindings_manifest.py (frozenset of exports)

Run via ``inv gen-bindings`` (writes files) or ``inv gen-bindings --check``
(regenerates to tempfiles and diffs; exit 1 on drift).

Why hand-rolled vs pycparser: this header is uniform C with no macro
trickery beyond ``ENTROPIC_EXPORT`` (stripped trivially) and
``size_t``/``stddef.h`` includes. pycparser requires a preprocessed
input and a ``fake_libc_include`` shim — operational complexity that
exceeds the marginal parsing-quality gain on this small surface.
"""

from __future__ import annotations

import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

# ── Configuration ──────────────────────────────────────────

REPO_ROOT = Path(__file__).resolve().parent.parent
HEADER_DIR = REPO_ROOT / "include" / "entropic"
TYPES_DIR = HEADER_DIR / "types"
ENTROPIC_H = HEADER_DIR / "entropic.h"

# Headers parsed for enums. Each tuple is (path, c_typedef_name).
# Order matters: emitted in this order so deterministic output.
ENUM_SOURCES = [
    (TYPES_DIR / "error.h", "entropic_error_t"),
    (TYPES_DIR / "enums.h", "entropic_agent_state_t"),
    (TYPES_DIR / "enums.h", "entropic_model_state_t"),
    (TYPES_DIR / "enums.h", "entropic_directive_type_t"),
    (TYPES_DIR / "enums.h", "entropic_compute_backend_t"),
    (TYPES_DIR / "hooks.h", "entropic_hook_point_t"),
    (ENTROPIC_H, "ent_decision_t"),
    (ENTROPIC_H, "entropic_mcp_access_level_t"),
]

# C-enum-typedef → (Python class name, prefix to strip, drop-sentinels set)
ENUM_RENAME = {
    "entropic_error_t": ("EntropicError", "ENTROPIC_ERROR_", set()),
    "entropic_agent_state_t": ("AgentState", "ENTROPIC_AGENT_STATE_", set()),
    "entropic_model_state_t": ("EntropicModelState", "ENTROPIC_MODEL_STATE_", set()),
    "entropic_directive_type_t": ("EntropicDirectiveType", "ENTROPIC_DIRECTIVE_", set()),
    "entropic_compute_backend_t": ("EntropicComputeBackend", "ENTROPIC_BACKEND_", set()),
    "entropic_hook_point_t": ("EntropicHookPoint", "ENTROPIC_HOOK_", {"COUNT_"}),
    "ent_decision_t": ("EntDecision", "ENT_DECISION_", set()),
    "entropic_mcp_access_level_t": ("EntropicMcpAccessLevel", "ENTROPIC_MCP_ACCESS_", set()),
}

# ENTROPIC_OK is the one error member without the ENTROPIC_ERROR_ prefix.
ENUM_SPECIAL_MEMBERS = {
    ("entropic_error_t", "ENTROPIC_OK"): "OK",
}

# C-struct-typedef → Python ctypes.Structure class name.
STRUCT_SOURCES = [
    (ENTROPIC_H, "ent_delegation_request_t", "EntDelegationRequest"),
    (ENTROPIC_H, "ent_delegation_result_t", "EntDelegationResult"),
    (ENTROPIC_H, "entropic_logprob_result_t", "EntropicLogprobResult"),
]

# C-callback-typedef → Python CFUNCTYPE name. Pulled from the header
# declarations directly; mapping renames to the legacy short forms the
# pre-2.2.1 wrapper exposed.
NAMED_CB_RENAME = {
    "entropic_hook_callback_t": "HOOK_CB",
    "ent_delegation_start_cb": "DELEGATION_START_CB",
    "ent_delegation_complete_cb": "DELEGATION_COMPLETE_CB",
    "ent_validation_attempt_boundary_cb": "ATTEMPT_BOUNDARY_CB",
    "entropic_compactor_fn": "COMPACTOR_CB",
    # v2.2.4, gh#57 — residency observer
    "entropic_residency_observer_t": "RESIDENCY_OBSERVER_CB",
}

# Inline function-pointer parameters that pre-2.2.1 had hand-named
# CFUNCTYPE aliases (TOKEN_CB, STATE_OBSERVER_CB, etc.). The generator
# emits a CFUNCTYPE under the chosen name and re-uses it everywhere an
# inline function-pointer with a matching signature appears.
#
# Keys: (function_name, parameter_index). Value: CFUNCTYPE name.
INLINE_CB_NAMES = {
    ("entropic_run_streaming", 2): "TOKEN_CB",
    ("entropic_run_messages_streaming", 2): "TOKEN_CB",
    ("entropic_set_stream_observer", 1): "STREAM_OBSERVER_CB",
    ("entropic_set_state_observer", 1): "STATE_OBSERVER_CB",
    ("entropic_set_queue_observer", 1): "QUEUE_OBSERVER_CB",
    ("entropic_set_critique_callbacks", 1): "CRITIQUE_START_CB",
    ("entropic_set_critique_callbacks", 2): "CRITIQUE_END_CB",
}

# Backward-compat aliases (gh#22). Emitted at the end of _bindings.py.
COMPAT_ALIASES = {
    "HOOK_CALLBACK_CB": "HOOK_CB",
    "TOKEN_STREAM_CB": "TOKEN_CB",
}

# C type → Python ctypes expression. Used for both struct fields and
# function signatures. Pointer suffix is folded (`* x` → `*`) before
# lookup so callers don't have to normalize.
CTYPE_MAP = {
    "void": "None",
    "int": "ctypes.c_int",
    "bool": "ctypes.c_bool",
    "int32_t": "ctypes.c_int32",
    "int64_t": "ctypes.c_int64",
    "uint32_t": "ctypes.c_uint32",
    "uint64_t": "ctypes.c_uint64",
    "size_t": "ctypes.c_size_t",
    "double": "ctypes.c_double",
    "float": "ctypes.c_float",
    "const char*": "ctypes.c_char_p",
    "char*": "ctypes.c_char_p",
    "char**": "ctypes.POINTER(ctypes.c_char_p)",
    "void*": "ctypes.c_void_p",
    "void**": "ctypes.POINTER(ctypes.c_void_p)",
    "int*": "ctypes.POINTER(ctypes.c_int)",
    "int32_t*": "ctypes.POINTER(ctypes.c_int32)",
    "int64_t*": "ctypes.POINTER(ctypes.c_int64)",
    "uint64_t*": "ctypes.POINTER(ctypes.c_uint64)",
    "float*": "ctypes.POINTER(ctypes.c_float)",
    "size_t*": "ctypes.POINTER(ctypes.c_size_t)",
    "const char* const*": "ctypes.POINTER(ctypes.c_char_p)",
    "entropic_handle_t": "entropic_handle_t",
    "entropic_handle_t*": "ctypes.POINTER(entropic_handle_t)",
    # Every C enum typedef maps to ctypes.c_int at the ABI boundary;
    # the Python IntEnum classes layer on top. Listed individually
    # rather than auto-derived so adding a new enum is an explicit
    # generator-side action, not a silent surface change.
    "entropic_error_t": "ctypes.c_int",
    "entropic_hook_point_t": "ctypes.c_int",
    "entropic_agent_state_t": "ctypes.c_int",
    "entropic_model_state_t": "ctypes.c_int",
    "entropic_directive_type_t": "ctypes.c_int",
    "entropic_compute_backend_t": "ctypes.c_int",
    "entropic_mcp_access_level_t": "ctypes.c_int",
    "ent_decision_t": "ctypes.c_int",
    # v2.2.4, gh#57
    "entropic_residency_event_t": "ctypes.c_int",
}

# ── Comment & token utilities ──────────────────────────────


_LINE_COMMENT_RE = re.compile(r"//[^\n]*")
_BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.DOTALL)


## @brief Strip both line and block C comments from header text.
## @utility
## @version 2.2.1
def strip_comments(text: str) -> str:
    """Remove ``//...`` and ``/* ... */`` so the parser sees code only.

    Doxygen ``/**`` blocks are stripped along with the rest — the
    generator does not currently lift docstrings into the bindings
    file. (A future pass can extract @brief / @param / @return into
    Python docstrings; the structure is there in the C headers.)
    """
    text = _LINE_COMMENT_RE.sub("", text)
    text = _BLOCK_COMMENT_RE.sub("", text)
    return text


## @brief Strip C qualifiers and collapse pointer/whitespace.
## @utility
## @version 2.2.1
def normalize_ctype(raw: str) -> str:
    """Return a canonical type spelling for lookup in CTYPE_MAP.

    Drops `static`/`inline`/`restrict`; collapses whitespace; folds
    ``* x`` → ``*``; preserves leading ``const`` because it changes the
    spelling we look up (``const char*`` is distinct from ``char*`` for
    documentation but maps to the same ctypes spelling — both are
    present in CTYPE_MAP).
    """
    t = re.sub(r"\b(static|inline|restrict)\b", "", raw).strip()
    t = re.sub(r"\s+", " ", t)
    t = re.sub(r"\s*\*", "*", t)
    return t


## @brief Split a top-level C argument list at commas, bracket-aware.
## @utility
## @version 2.2.1
def split_args(arglist: str) -> list[str]:
    """Comma-split respecting paren depth so function-pointer params survive.

    A signature like ``void (*on_token)(const char*, size_t, void*),
    void* user_data, int* cancel_flag`` splits into three top-level
    arguments, not five.
    """
    arglist = arglist.strip()
    if not arglist or arglist == "void":
        return []
    out: list[str] = []
    depth = 0
    start = 0
    for i, ch in enumerate(arglist):
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        elif ch == "," and depth == 0:
            out.append(arglist[start:i].strip())
            start = i + 1
    out.append(arglist[start:].strip())
    return [a for a in out if a]


# ── Declaration containers ─────────────────────────────────


@dataclass
class EnumDecl:
    """Parsed ``typedef enum`` declaration.

    @brief Resolved enum members for IntEnum emission.
    @version 2.2.1
    """

    c_typedef: str
    members: list[tuple[str, int]] = field(default_factory=list)


@dataclass
class StructField:
    """One ``ctypes.Structure._fields_`` entry.

    @brief Parsed struct field with resolved ctypes spelling.
    @version 2.2.1
    """

    name: str
    ctype: str


@dataclass
class StructDecl:
    """Parsed ``typedef struct`` declaration.

    @brief Resolved struct fields for ctypes.Structure emission.
    @version 2.2.1
    """

    c_typedef: str
    py_name: str
    fields: list[StructField] = field(default_factory=list)


@dataclass
class CallbackDecl:
    """Parsed ``typedef R (*name)(args)`` function-pointer typedef.

    @brief Named callback signature for CFUNCTYPE emission.
    @version 2.2.1
    """

    c_typedef: str
    py_name: str
    restype: str
    argtypes: list[str] = field(default_factory=list)


@dataclass
class FunctionDecl:
    """Parsed ``ENTROPIC_EXPORT R name(args)`` function declaration.

    @brief Function binding spec; ``argtypes`` may contain CFUNCTYPE
           names for inline function-pointer parameters.
    @version 2.2.1
    """

    name: str
    restype: str
    argtypes: list[str] = field(default_factory=list)
    inline_cbs: list[CallbackDecl] = field(default_factory=list)


## @brief Parse a single typedef enum block out of header text.
## @utility
## @version 2.2.1
def parse_enum(text: str, c_typedef: str) -> EnumDecl:
    """Return an EnumDecl with sequential auto-numbering applied."""
    pattern = re.compile(
        r"typedef\s+enum\s*\{([^}]*)\}\s*" + re.escape(c_typedef) + r"\s*;",
        re.DOTALL,
    )
    m = pattern.search(text)
    if m is None:
        raise SystemExit(f"gen_bindings: typedef enum {c_typedef} not found")
    body = m.group(1)
    decl = EnumDecl(c_typedef=c_typedef)
    next_value = 0
    for raw in body.split(","):
        entry = raw.strip()
        if not entry:
            continue
        if "=" in entry:
            name, value_expr = (s.strip() for s in entry.split("=", 1))
            value = int(value_expr, 0)
        else:
            name, value = entry, next_value
        decl.members.append((name, value))
        next_value = value + 1
    return decl


## @brief Parse a typedef struct block out of header text.
## @utility
## @version 2.2.1
def parse_struct(text: str, c_typedef: str, py_name: str) -> StructDecl:
    """Return a StructDecl with resolved ctypes for each field.

    Handles both anonymous (``typedef struct { ... } name;``) and named
    (``typedef struct tag { ... } name;``) forms — the latter appears
    for ``entropic_logprob_result_t`` and a couple of other types whose
    inner struct keeps a tag for forward-declaration purposes.
    """
    pattern = re.compile(
        r"typedef\s+struct(?:\s+\w+)?\s*\{([^}]*)\}\s*" + re.escape(c_typedef) + r"\s*;",
        re.DOTALL,
    )
    m = pattern.search(text)
    if m is None:
        raise SystemExit(f"gen_bindings: typedef struct {c_typedef} not found")
    decl = StructDecl(c_typedef=c_typedef, py_name=py_name)
    for line in m.group(1).split(";"):
        entry = line.strip()
        if not entry:
            continue
        # Split on the last whitespace before the field name.
        parts = entry.rsplit(maxsplit=1)
        if len(parts) != 2:
            raise SystemExit(f"gen_bindings: cannot parse struct field {entry!r}")
        type_raw, name = parts
        # Pointer-on-name vs pointer-on-type: fold ``const char* foo``
        # vs ``const char *foo`` into the same spelling.
        if name.startswith("*"):
            type_raw = type_raw + name[: name.rfind("*") + 1]
            name = name.lstrip("*")
        ctype = normalize_ctype(type_raw)
        py_ctype = resolve_one(ctype, type_raw)
        decl.fields.append(StructField(name=name, ctype=py_ctype))
    return decl


## @brief Parse all `typedef R (*name)(args)` callback declarations.
## @utility
## @version 2.2.1
def parse_callbacks(text: str) -> list[CallbackDecl]:
    """Return CallbackDecls for every named callback typedef in text."""
    # Pattern: `typedef <ret> (*<name>)(<args>);`. Restype may contain
    # spaces and pointer suffix. Bracket-balance the args list with a
    # tail re-parse to allow nested parens (rare but possible).
    pattern = re.compile(
        r"typedef\s+([\w\s\*]+?)\s*\(\s*\*\s*(\w+)\s*\)\s*\(([^)]*)\)\s*;",
        re.DOTALL,
    )
    out: list[CallbackDecl] = []
    for m in pattern.finditer(text):
        ret_raw, name, args_raw = m.groups()
        if name not in NAMED_CB_RENAME:
            continue
        restype = normalize_ctype(ret_raw)
        argtypes = resolve_argtypes(split_args(args_raw))
        py_restype = CTYPE_MAP.get(restype)
        if py_restype is None:
            raise SystemExit(f"gen_bindings: unmapped callback restype {restype!r} for {name}")
        out.append(
            CallbackDecl(
                c_typedef=name,
                py_name=NAMED_CB_RENAME[name],
                restype=py_restype,
                argtypes=argtypes,
            )
        )
    return out


## @brief Drop a parameter name from "type name"; return canonical type.
## @utility
## @version 2.2.1
def extract_argtype(arg: str) -> str:
    """Return the type portion of a ``<type> <name>`` C parameter."""
    arg = re.sub(r"/\*.*?\*/", "", arg).strip()
    parts = arg.rsplit(maxsplit=1)
    if len(parts) == 2 and parts[1].lstrip("*").isidentifier():
        type_raw = parts[0]
        # Pull `*` off the front of the name if present (e.g. `char *foo`).
        stars = len(parts[1]) - len(parts[1].lstrip("*"))
        if stars:
            type_raw = type_raw + ("*" * stars)
        return normalize_ctype(type_raw)
    return normalize_ctype(arg)


## @brief Translate C arg list to Python ctypes spellings.
## @utility
## @version 2.2.1
def resolve_argtypes(args: list[str]) -> list[str]:
    """Map each C arg string to its Python ctypes expression."""
    return [resolve_one(extract_argtype(a), a) for a in args]


## @brief Resolve a single canonical C type to a Python ctypes spelling.
## @utility
## @version 2.2.1
def resolve_one(canon: str, raw: str) -> str:
    """Single-type resolver shared between args, restypes, and struct fields.

    Recognized forms (in order):
      1. Pointer-to-known-struct → ``ctypes.POINTER(<PyStruct>)``
      2. Named callback typedef → its CFUNCTYPE name
      3. Pointer-to-named-callback typedef → ``ctypes.POINTER(<CB>)``
         (used for out-params delivering function pointers, e.g.
         ``entropic_get_default_compactor``)
      4. CTYPE_MAP lookup (with leading ``const`` stripped if needed —
         ctypes has no const qualifier)
    """
    result = _try_resolve_one(canon)
    if result is not None:
        return result
    raise SystemExit(f"gen_bindings: unmapped arg type {canon!r} (from {raw!r})")


## @brief Inner resolver that returns None on failure.
## @utility
## @version 2.2.1
def _try_resolve_one(canon: str) -> str | None:
    """Apply each resolution rule in order; return the first hit, else None.

    Returns are kept ≤3 by building a chain of fall-through resolvers
    and walking them in order. Each resolver maps canon → str | None.
    """
    for resolver in _RESOLVERS:
        result = resolver(canon)
        if result is not None:
            return result
    return None


## @brief Resolve pointer-to-known-struct → ctypes.POINTER(PyStruct).
## @utility
## @version 2.2.1
def _resolve_struct_ptr(canon: str) -> str | None:
    """Return POINTER(PyStruct) when canon names a struct pointer."""
    sp = struct_pointer_py_name(canon)
    return f"ctypes.POINTER({sp})" if sp is not None else None


## @brief Resolve a bare named-callback typedef.
## @utility
## @version 2.2.1
def _resolve_named_cb(canon: str) -> str | None:
    """Return the CFUNCTYPE name for a registered callback typedef."""
    return NAMED_CB_RENAME.get(canon)


## @brief Resolve pointer-to-named-callback typedef.
## @utility
## @version 2.2.1
def _resolve_named_cb_ptr(canon: str) -> str | None:
    """Return POINTER(<CB>) when canon is a pointer to a callback typedef."""
    if not canon.endswith("*"):
        return None
    base = canon[:-1].rstrip()
    return f"ctypes.POINTER({NAMED_CB_RENAME[base]})" if base in NAMED_CB_RENAME else None


## @brief Direct CTYPE_MAP lookup with no fallback.
## @utility
## @version 2.2.1
def _resolve_ctype(canon: str) -> str | None:
    """Return the CTYPE_MAP entry for ``canon`` verbatim, or None."""
    return CTYPE_MAP.get(canon)


## @brief Resolve a leading-``const`` canonical type by stripping the qualifier.
## @utility
## @version 2.2.1
def _resolve_const_stripped(canon: str) -> str | None:
    """Retry CTYPE_MAP lookup after dropping a leading ``const`` qualifier."""
    if not canon.startswith("const "):
        return None
    return CTYPE_MAP.get(canon[len("const ") :])


_RESOLVERS = (
    _resolve_struct_ptr,
    _resolve_named_cb,
    _resolve_named_cb_ptr,
    _resolve_ctype,
    _resolve_const_stripped,
)


## @brief Detect an inline function-pointer parameter.
## @utility
## @version 2.2.1
def is_inline_fnptr(arg: str) -> bool:
    """True when ``arg`` is ``void (*name)(...)`` or similar."""
    return bool(re.match(r"^[\w\s\*]+\(\s*\*\s*\w+\s*\)\s*\(", arg, re.DOTALL))


## @brief Build a synthetic CallbackDecl from an inline function-pointer.
## @utility
## @version 2.2.1
def synth_callback(func_name: str, arg_idx: int, arg: str) -> CallbackDecl:
    """Return a CFUNCTYPE-emittable CallbackDecl with a minted name."""
    m = re.match(r"^([\w\s\*]+?)\s*\(\s*\*\s*\w+\s*\)\s*\((.*)\)\s*$", arg, re.DOTALL)
    if m is None:
        raise SystemExit(f"gen_bindings: cannot parse inline fnptr {arg!r}")
    ret_raw, args_raw = m.groups()
    restype = normalize_ctype(ret_raw)
    py_restype = CTYPE_MAP.get(restype)
    if py_restype is None:
        raise SystemExit(f"gen_bindings: unmapped inline-cb restype {restype!r} in {func_name}")
    argtypes = resolve_argtypes(split_args(args_raw))
    py_name = INLINE_CB_NAMES.get((func_name, arg_idx))
    if py_name is None:
        # Fall back to a deterministic mint that won't collide.
        py_name = f"{func_name.upper()}_ARG{arg_idx}_CB"
    return CallbackDecl(
        c_typedef=f"<inline {func_name} arg {arg_idx}>",
        py_name=py_name,
        restype=py_restype,
        argtypes=argtypes,
    )


## @brief Parse every ENTROPIC_EXPORT function declaration in text.
## @utility
## @version 2.2.1
def parse_functions(text: str) -> list[FunctionDecl]:
    """Return one FunctionDecl per ENTROPIC_EXPORT declaration.

    Bracket-balances the argument list so inline function-pointer
    params survive. Restype regex stops at the function name; arg
    list is captured by paren-depth from the opening ``(``.
    """
    # Locate `ENTROPIC_EXPORT <ret> <name>(`. Then walk forward to find
    # the matching closing paren of the argument list, then the
    # trailing semicolon.
    starter = re.compile(r"ENTROPIC_EXPORT\s+([\w\s\*]+?)\s+(\w+)\s*\(", re.DOTALL)
    out: list[FunctionDecl] = []
    for m in starter.finditer(text):
        ret_raw, name = m.group(1), m.group(2)
        args_start = m.end()
        depth = 1
        i = args_start
        while i < len(text) and depth > 0:
            if text[i] == "(":
                depth += 1
            elif text[i] == ")":
                depth -= 1
            i += 1
        if depth != 0:
            raise SystemExit(f"gen_bindings: unclosed args for {name}")
        args_raw = text[args_start : i - 1]
        # Require trailing `;` for a declaration (not a definition).
        tail = text[i:].lstrip()
        if not tail.startswith(";"):
            continue
        decl = build_function_decl(name, ret_raw, args_raw)
        out.append(decl)
    return out


## @brief Construct a FunctionDecl, lifting inline fnptr params.
## @utility
## @version 2.2.1
def build_function_decl(name: str, ret_raw: str, args_raw: str) -> FunctionDecl:
    """Resolve restype, argtypes, and any inline callbacks for one function."""
    restype = normalize_ctype(ret_raw)
    py_restype = CTYPE_MAP.get(restype)
    if py_restype is None:
        raise SystemExit(f"gen_bindings: unmapped restype {restype!r} for {name}")
    args = split_args(args_raw)
    argtypes: list[str] = []
    inline_cbs: list[CallbackDecl] = []
    for idx, a in enumerate(args):
        if is_inline_fnptr(a):
            cb = synth_callback(name, idx, a)
            argtypes.append(cb.py_name)
            inline_cbs.append(cb)
            continue
        argtypes.append(resolve_one(extract_argtype(a), a))
    return FunctionDecl(name=name, restype=py_restype, argtypes=argtypes, inline_cbs=inline_cbs)


## @brief If `canon` is a pointer-to-known-struct, return PyStruct name.
## @utility
## @version 2.2.1
def struct_pointer_py_name(canon: str) -> str | None:
    """Map ``const ent_delegation_request_t*`` → ``EntDelegationRequest``."""
    stripped = canon.replace("const", "").replace(" ", "")
    if not stripped.endswith("*"):
        return None
    base = stripped[:-1]
    for _src, c_typedef, py_name in STRUCT_SOURCES:
        if c_typedef == base:
            return py_name
    return None


# ── Emitters ──────────────────────────────────────────────


PROLOGUE = '''# SPDX-License-Identifier: Apache-2.0
# AUTO-GENERATED by scripts/gen_bindings.py — do not edit.
# Run ``inv gen-bindings`` to regenerate; ``inv gen-bindings --check``
# runs in pre-commit and fails if the committed file drifts from the
# header. Source: include/entropic/entropic.h + types/*.h.
"""ctypes bindings for librentropic.so (auto-generated).

This module exposes the entropic C ABI as a flat collection of ctypes
function objects, IntEnum classes, ctypes.Structure types, and CFUNCTYPE
callback typedefs. Symbol names match the C header verbatim.

For symbols not exposed here (none, by construction — the generator
covers every ENTROPIC_EXPORT in entropic.h), load librentropic.so
directly via :func:`entropic._loader.load`.
"""

from __future__ import annotations

import ctypes
import enum

from entropic._loader import load

_lib = load()

# Opaque handle. C side: ``struct entropic_engine*``. From Python it is
# a void* the C functions interpret; pass ``ctypes.byref(handle)`` for
# out-params and ``handle`` for in-params.
entropic_handle_t = ctypes.c_void_p


## @brief Pin restype/argtypes on a ctypes symbol from the loaded .so.
## @utility
## @version 2.2.1
def _bind(name, restype, *argtypes):
    """Pin restype/argtypes on a symbol from the loaded library.

    Helper used by every binding line below.
    """
    fn = getattr(_lib, name)
    fn.restype = restype
    fn.argtypes = list(argtypes)
    return fn


'''


## @brief Render an EnumDecl as a Python IntEnum class.
## @utility
## @version 2.2.1
def emit_enum(decl: EnumDecl) -> str:
    """Return the source text for one IntEnum class."""
    py_name, prefix, drop = ENUM_RENAME[decl.c_typedef]
    lines = [f"class {py_name}(enum.IntEnum):"]
    lines.append(f'    """Mirrors ``{decl.c_typedef}`` from the C header."""')
    lines.append("")
    for c_name, value in decl.members:
        special_key = (decl.c_typedef, c_name)
        if special_key in ENUM_SPECIAL_MEMBERS:
            py_member = ENUM_SPECIAL_MEMBERS[special_key]
        elif c_name.startswith(prefix):
            py_member = c_name[len(prefix) :]
        else:
            raise SystemExit(
                f"gen_bindings: enum member {c_name!r} in {decl.c_typedef}"
                f" lacks prefix {prefix!r} and no special mapping"
            )
        if py_member in drop:
            continue
        lines.append(f"    {py_member} = {value}")
    return "\n".join(lines) + "\n"


## @brief Render a StructDecl as a ctypes.Structure subclass.
## @utility
## @version 2.2.1
def emit_struct(decl: StructDecl) -> str:
    """Return the source text for one ctypes.Structure class."""
    lines = [f"class {decl.py_name}(ctypes.Structure):"]
    lines.append(f'    """Mirrors ``{decl.c_typedef}`` from the C header."""')
    lines.append("")
    lines.append("    _fields_ = [")
    for f in decl.fields:
        lines.append(f'        ("{f.name}", {f.ctype}),')
    lines.append("    ]")
    return "\n".join(lines) + "\n"


## @brief Render a CallbackDecl as a module-level CFUNCTYPE assignment.
## @utility
## @version 2.2.1
def emit_callback(decl: CallbackDecl) -> str:
    """Return ``NAME = ctypes.CFUNCTYPE(restype, *argtypes)``."""
    parts = [decl.restype, *decl.argtypes]
    return f"{decl.py_name} = ctypes.CFUNCTYPE({', '.join(parts)})\n"


## @brief Render a FunctionDecl as a _bind() call.
## @utility
## @version 2.2.1
def emit_function(decl: FunctionDecl) -> str:
    """Return ``name = _bind("name", restype, *argtypes)``."""
    parts = [f'"{decl.name}"', decl.restype, *decl.argtypes]
    return f"{decl.name} = _bind({', '.join(parts)})\n"


## @brief Render the gh#22 compat-alias section.
## @utility
## @version 2.2.1
def emit_aliases() -> str:
    """Return alias assignments preserving pre-2.2.1 wrapper names."""
    lines = ["# gh#22 backward-compat aliases for the CFUNCTYPE typedefs."]
    for alias, target in COMPAT_ALIASES.items():
        lines.append(f"{alias} = {target}")
    return "\n".join(lines) + "\n"


# ── Top-level driver ──────────────────────────────────────


@dataclass
class GeneratedSources:
    """Resolved generator outputs for both files.

    @brief Container holding bindings + manifest source text.
    @version 2.2.1
    """

    bindings_py: str
    manifest_py: str


## @brief Run the full parse + emit pipeline; return generated sources.
## @utility
## @version 2.2.1
def generate() -> GeneratedSources:
    """Parse all input headers and return generated source text.

    Does not write any files. Callers (``inv gen-bindings``,
    ``inv gen-bindings --check``) decide whether to commit or compare.
    """
    enums = [parse_enum(read(p), name) for (p, name) in ENUM_SOURCES]
    structs = [
        parse_struct(read(p), c_typedef, py_name) for (p, c_typedef, py_name) in STRUCT_SOURCES
    ]
    # Named callbacks live in different headers; parse each. Order is
    # explicit (not set-iteration) so generator output is deterministic.
    cb_sources = [TYPES_DIR / "hooks.h", ENTROPIC_H]
    callbacks: list[CallbackDecl] = []
    seen_cb_names: set[str] = set()
    for path in cb_sources:
        for cb in parse_callbacks(read(path)):
            if cb.py_name in seen_cb_names:
                continue
            callbacks.append(cb)
            seen_cb_names.add(cb.py_name)
    functions = parse_functions(read(ENTROPIC_H))
    # Collect minted CFUNCTYPEs from inline-fnptr params, dedup by name.
    inline_cbs: list[CallbackDecl] = []
    for fn in functions:
        for cb in fn.inline_cbs:
            if cb.py_name in seen_cb_names:
                continue
            inline_cbs.append(cb)
            seen_cb_names.add(cb.py_name)
    bindings = render_bindings(enums, structs, callbacks, inline_cbs, functions)
    manifest = render_manifest(enums, structs, callbacks, inline_cbs, functions)
    return GeneratedSources(bindings_py=bindings, manifest_py=manifest)


## @brief Concatenate prologue + emitted sections into the bindings source.
## @utility
## @version 2.2.1
def render_bindings(
    enums: list[EnumDecl],
    structs: list[StructDecl],
    callbacks: list[CallbackDecl],
    inline_cbs: list[CallbackDecl],
    functions: list[FunctionDecl],
) -> str:
    """Return the full text of python/src/entropic/_bindings.py."""
    sections: list[str] = [PROLOGUE]
    sections.append("# ── Enums ─────────────────────────────────────\n\n")
    sections.extend(emit_enum(e) + "\n" for e in enums)
    sections.append("# ── Structs ───────────────────────────────────\n\n")
    sections.extend(emit_struct(s) + "\n" for s in structs)
    sections.append("# ── Callback typedefs (named) ─────────────────\n\n")
    sections.extend(emit_callback(c) for c in callbacks)
    sections.append("\n# ── Callback typedefs (from inline params) ────\n\n")
    sections.extend(emit_callback(c) for c in inline_cbs)
    sections.append("\n" + emit_aliases() + "\n")
    sections.append("# ── Bindings ──────────────────────────────────\n\n")
    sections.extend(emit_function(f) for f in functions)
    return "".join(sections)


## @brief Build the manifest module text.
## @utility
## @version 2.2.2
def render_manifest(
    enums: list[EnumDecl],
    structs: list[StructDecl],
    callbacks: list[CallbackDecl],
    inline_cbs: list[CallbackDecl],
    functions: list[FunctionDecl],
) -> str:
    """Return the text of python/src/entropic/_bindings_manifest.py."""
    names: list[str] = []
    names.append("entropic_handle_t")
    names.extend(ENUM_RENAME[e.c_typedef][0] for e in enums)
    names.extend(s.py_name for s in structs)
    names.extend(c.py_name for c in callbacks)
    names.extend(c.py_name for c in inline_cbs)
    names.extend(COMPAT_ALIASES.keys())
    names.extend(f.name for f in functions)
    quoted = ",\n        ".join(f'"{n}"' for n in sorted(names))
    return (
        "# SPDX-License-Identifier: Apache-2.0\n"
        "# AUTO-GENERATED by scripts/gen_bindings.py — do not edit.\n"
        '"""Exported names for the entropic ctypes bindings.\n\n'
        "This file is imported eagerly by ``entropic.__init__`` to populate\n"
        "the public ``_LAZY_EXPORTS`` set without loading ``librentropic.so``.\n"
        '"""\n\n'
        "from __future__ import annotations\n\n"
        "EXPORTS = frozenset(\n"
        "    {\n"
        f"        {quoted},\n"
        "    }\n"
        ")\n"
    )


## @brief Read a UTF-8 text file; strip comments; return the cleaned text.
## @utility
## @version 2.2.1
def read(path: Path) -> str:
    """Return path.read_text() with C comments stripped."""
    return strip_comments(path.read_text())


# ── CLI ───────────────────────────────────────────────────


BINDINGS_OUT = REPO_ROOT / "python" / "src" / "entropic" / "_bindings.py"
MANIFEST_OUT = REPO_ROOT / "python" / "src" / "entropic" / "_bindings_manifest.py"


## @brief Write generated files. Used by ``inv gen-bindings``.
## @utility
## @version 2.2.1
def write_outputs(src: GeneratedSources) -> None:
    """Persist generated text to the canonical output paths."""
    BINDINGS_OUT.write_text(src.bindings_py)
    MANIFEST_OUT.write_text(src.manifest_py)


## @brief Compare generated text against committed files; exit 1 on drift.
## @utility
## @version 2.2.1
def check_outputs(src: GeneratedSources) -> int:
    """Return 0 if checked-in files match generated text, else 1 + diagnostic."""
    ok = True
    for label, generated, path in (
        ("_bindings.py", src.bindings_py, BINDINGS_OUT),
        ("_bindings_manifest.py", src.manifest_py, MANIFEST_OUT),
    ):
        committed = path.read_text() if path.exists() else ""
        if committed != generated:
            ok = False
            print(
                f"gen_bindings: drift in {label} — committed file differs"
                f" from header-derived output.\n"
                f"  Path: {path.relative_to(REPO_ROOT)}\n"
                f"  Fix:  run `inv gen-bindings` and commit the result.",
                file=sys.stderr,
            )
    return 0 if ok else 1


## @brief Module entrypoint.
## @utility
## @version 2.2.1
def main(argv: list[str]) -> int:
    """Top-level CLI: --check vs default write."""
    src = generate()
    if "--check" in argv:
        return check_outputs(src)
    write_outputs(src)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
