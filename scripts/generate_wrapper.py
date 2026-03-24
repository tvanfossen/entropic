#!/usr/bin/env python3
"""Generate Python ctypes wrapper from entropic C API headers.

Parses entropic.h, types/error.h, and types/hooks.h to produce a complete
Python module with ctypes bindings, enum classes, wrapper classes, and
error handling.

@brief Header parser + ctypes wrapper code generator for entropic C API.
@version 1

Usage:
    python scripts/generate_wrapper.py \
        --header include/entropic/entropic.h \
        --types-dir include/entropic/types/ \
        --output python/entropic/__init__.py
"""

from __future__ import annotations

import argparse
import re
import sys
import textwrap
from dataclasses import dataclass, field
from pathlib import Path

# ── Parsed Data Structures ───────────────────────────────


@dataclass
class EnumValue:
    """Single enum member.

    @brief Stores name, optional explicit integer value, and doc comment.
    @version 1
    """

    name: str
    value: int | None = None
    doc: str = ""


@dataclass
class EnumDef:
    """Parsed C enum typedef.

    @brief Stores enum typedef name and all member values.
    @version 1
    """

    name: str
    values: list[EnumValue] = field(default_factory=list)


@dataclass
class FuncParam:
    """Single function parameter.

    @brief Stores C type string and parameter name.
    @version 1
    """

    c_type: str
    name: str


@dataclass
class FuncDecl:
    """Parsed exported function declaration.

    @brief Stores return type, name, parameters, and docstring.
    @version 1
    """

    return_type: str
    name: str
    params: list[FuncParam] = field(default_factory=list)
    brief: str = ""


@dataclass
class TypedefDecl:
    """Parsed typedef (handle or callback).

    @brief Stores the original C typedef text, the alias name, and kind.
    @version 1
    """

    name: str
    kind: str  # "handle" or "callback"
    c_text: str = ""
    callback_return: str = ""
    callback_params: list[FuncParam] = field(default_factory=list)


@dataclass
class ParsedAPI:
    """Complete parsed C API surface.

    @brief Aggregates all enums, functions, typedefs from header parsing.
    @version 1
    """

    enums: list[EnumDef] = field(default_factory=list)
    functions: list[FuncDecl] = field(default_factory=list)
    typedefs: list[TypedefDecl] = field(default_factory=list)


# ── Parser ───────────────────────────────────────────────


def _extract_briefs(text: str) -> dict[str, str]:
    """Extract @brief comments associated with function declarations.

    Maps function name → brief text by finding Doxygen blocks immediately
    preceding ENTROPIC_EXPORT lines.

    @brief Build function-name-to-brief mapping from Doxygen comments.
    @version 1
    """
    briefs: dict[str, str] = {}
    # Match Doxygen comment blocks followed by ENTROPIC_EXPORT
    pattern = re.compile(
        r"/\*\*.*?@brief\s+(.+?)[\n*].*?\*/\s*" r"ENTROPIC_EXPORT\s+\S+\s+(\w+)\s*\(",
        re.DOTALL,
    )
    for match in pattern.finditer(text):
        brief_text = match.group(1).strip().rstrip(".")
        func_name = match.group(2)
        briefs[func_name] = brief_text
    return briefs


def parse_enums(text: str) -> list[EnumDef]:
    """Parse typedef enum { ... } name; declarations.

    @brief Extract all entropic enum typedefs with values and docs.
    @version 2
    """
    enums: list[EnumDef] = []
    pattern = re.compile(
        r"typedef\s+enum\s*\{([^}]+)\}\s*(\w+)\s*;",
        re.DOTALL,
    )
    for match in pattern.finditer(text):
        values = _parse_enum_body(match.group(1))
        if values:
            enums.append(EnumDef(name=match.group(2), values=values))

    return enums


def _parse_enum_body(body: str) -> list[EnumValue]:
    """Parse the body of a C enum into EnumValue list.

    @brief Extract member names, values, and docs from enum body text.
    @version 1
    """
    values: list[EnumValue] = []
    current_val = 0

    for line in body.split("\n"):
        line = line.strip()
        if not line or line.startswith("/*") or line.startswith("*"):
            continue

        doc = ""
        doc_match = re.search(r"///<?(.+)", line)
        if doc_match:
            doc = doc_match.group(1).strip()

        member_match = re.match(r"(\w+)\s*(?:=\s*(\d+))?\s*,?", line)
        if not member_match or not member_match.group(1).startswith("ENTROPIC"):
            continue

        if member_match.group(2) is not None:
            current_val = int(member_match.group(2))
        values.append(EnumValue(name=member_match.group(1), value=current_val, doc=doc))
        current_val += 1

    return values


def parse_handle_typedefs(text: str) -> list[TypedefDecl]:
    """Parse opaque handle typedefs: typedef struct X* Y;

    @brief Extract opaque handle pointer typedefs.
    @version 1
    """
    typedefs: list[TypedefDecl] = []
    pattern = re.compile(
        r"typedef\s+struct\s+\w+\s*\*\s*(\w+)\s*;",
    )
    for match in pattern.finditer(text):
        name = match.group(1)
        typedefs.append(
            TypedefDecl(
                name=name,
                kind="handle",
                c_text=match.group(0),
            )
        )
    return typedefs


def parse_callback_typedefs(text: str) -> list[TypedefDecl]:
    """Parse callback function pointer typedefs.

    @brief Extract callback typedefs like typedef void (*name_t)(...).
    @version 1
    """
    typedefs: list[TypedefDecl] = []
    pattern = re.compile(
        r"typedef\s+(\w+)\s+\(\*(\w+)\)\s*\(([^)]*)\)\s*;",
        re.DOTALL,
    )
    for match in pattern.finditer(text):
        ret_type = match.group(1)
        name = match.group(2)
        params_str = match.group(3).strip()
        params = _parse_param_list(params_str)
        typedefs.append(
            TypedefDecl(
                name=name,
                kind="callback",
                c_text=match.group(0),
                callback_return=ret_type,
                callback_params=params,
            )
        )
    return typedefs


def _extract_balanced_parens(text: str, start: int) -> str | None:
    """Extract text inside balanced parentheses starting at position start.

    start must point to the opening '('. Returns the content between
    the outermost parens (exclusive), or None if unbalanced.

    @brief Scan balanced parentheses accounting for nesting depth.
    @version 1
    """
    if start >= len(text) or text[start] != "(":
        return None
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return text[start + 1 : i]
    return None


def parse_exported_functions(text: str) -> list[FuncDecl]:
    """Parse ENTROPIC_EXPORT function declarations.

    Uses balanced-paren scanning to handle function pointer parameters
    like void (*on_token)(const char* token, size_t len, void* user_data).

    @brief Extract all exported C API function signatures.
    @version 2
    """
    briefs = _extract_briefs(text)
    functions: list[FuncDecl] = []

    # Match up to the opening paren of the parameter list
    pattern = re.compile(
        r"ENTROPIC_EXPORT\s+"
        r"([\w\s*]+?)\s+"  # return type
        r"(\w+)\s*"  # function name
        r"\(",  # opening paren of params
        re.DOTALL,
    )
    for match in pattern.finditer(text):
        ret_type = _normalize_type(match.group(1).strip())
        func_name = match.group(2)

        # Use balanced-paren scanner from the opening paren
        paren_start = match.end() - 1  # position of '('
        params_str = _extract_balanced_parens(text, paren_start)
        if params_str is None:
            continue

        params = _parse_param_list(params_str.strip())
        functions.append(
            FuncDecl(
                return_type=ret_type,
                name=func_name,
                params=params,
                brief=briefs.get(func_name, ""),
            )
        )

    return functions


def _parse_param_list(params_str: str) -> list[FuncParam]:
    """Parse a C function parameter list string.

    @brief Split parameter string into typed name pairs.
    @version 1
    """
    if not params_str or params_str == "void":
        return []

    params: list[FuncParam] = []
    # Handle function pointer params specially
    # Split on commas, but respect parentheses for func ptr params
    parts = _split_params(params_str)

    for part in parts:
        part = part.strip()
        if not part:
            continue

        # Function pointer parameter: void (*name)(...)
        fptr_match = re.match(
            r"([\w\s]+)\s*\(\*(\w+)\)\s*\(([^)]*)\)",
            part,
        )
        if fptr_match:
            params.append(
                FuncParam(
                    c_type=part,
                    name=fptr_match.group(2),
                )
            )
            continue

        # Regular parameter: type name or type* name
        # Split off the last word as the name
        tokens = part.split()
        if len(tokens) >= 2:
            param_name = tokens[-1].lstrip("*")
            param_type = " ".join(tokens[:-1])
            # Move pointer stars to the type
            stars = ""
            while tokens[-1].startswith("*"):
                stars += "*"
                tokens[-1] = tokens[-1][1:]
            if stars:
                param_type += stars
            params.append(FuncParam(c_type=_normalize_type(param_type), name=param_name))
        elif len(tokens) == 1:
            # Single token — likely just a type with no name (e.g., "void")
            params.append(FuncParam(c_type=_normalize_type(tokens[0]), name=""))

    return params


def _split_params(params_str: str) -> list[str]:
    """Split parameter string on commas, respecting nested parentheses.

    @brief Comma-split with parenthesis depth tracking.
    @version 1
    """
    parts: list[str] = []
    depth = 0
    current = ""
    for ch in params_str:
        if ch == "(":
            depth += 1
            current += ch
        elif ch == ")":
            depth -= 1
            current += ch
        elif ch == "," and depth == 0:
            parts.append(current)
            current = ""
        else:
            current += ch
    if current.strip():
        parts.append(current)
    return parts


def _normalize_type(c_type: str) -> str:
    """Normalize whitespace in a C type string.

    @brief Collapse whitespace and normalize pointer/const spacing.
    @version 1
    """
    return re.sub(r"\s+", " ", c_type).strip()


def parse_headers(
    header_path: Path,
    types_dir: Path,
) -> ParsedAPI:
    """Parse all headers and return the complete API surface.

    @brief Main parse entry point — reads header files and extracts API.
    @version 1
    """
    api = ParsedAPI()

    # Read all header text
    header_text = header_path.read_text()
    error_text = (types_dir / "error.h").read_text()
    hooks_text = (types_dir / "hooks.h").read_text()

    # Parse enums from type headers
    api.enums.extend(parse_enums(error_text))
    api.enums.extend(parse_enums(hooks_text))

    # Parse typedefs from type headers (handles, callbacks)
    api.typedefs.extend(parse_handle_typedefs(error_text))
    api.typedefs.extend(parse_callback_typedefs(error_text))
    api.typedefs.extend(parse_callback_typedefs(hooks_text))

    # Parse exported functions from main header
    api.functions.extend(parse_exported_functions(header_text))

    # Also parse functions exported from error.h
    api.functions.extend(parse_exported_functions(error_text))

    return api


# ── C Type → ctypes Mapping ─────────────────────────────


# Map of C type strings to ctypes type expressions
_CTYPE_MAP: dict[str, str] = {
    "void": "None",
    "int": "ctypes.c_int",
    "size_t": "ctypes.c_size_t",
    "char*": "ctypes.c_char_p",
    "char **": "ctypes.POINTER(ctypes.c_char_p)",
    "const char*": "ctypes.c_char_p",
    "const char *": "ctypes.c_char_p",
    "void*": "ctypes.c_void_p",
    "void *": "ctypes.c_void_p",
    "int*": "ctypes.POINTER(ctypes.c_int)",
    "int *": "ctypes.POINTER(ctypes.c_int)",
    "entropic_error_t": "ctypes.c_int",
    "entropic_handle_t": "ctypes.c_void_p",
    "entropic_handle_t*": "ctypes.POINTER(ctypes.c_void_p)",
    "entropic_hook_point_t": "ctypes.c_int",
    "entropic_error_callback_t": "entropic_error_callback_t",
    "entropic_hook_callback_t": "entropic_hook_callback_t",
}


def _c_type_to_ctypes(c_type: str) -> str:
    """Convert a C type string to its ctypes equivalent.

    @brief Map C type to Python ctypes expression string.
    @version 2
    """
    normalized = _normalize_type(c_type)

    # Direct lookup first
    if normalized in _CTYPE_MAP:
        return _CTYPE_MAP[normalized]

    # Double-pointer check before single-pointer (** ends with * too)
    result = "ctypes.c_void_p"  # default for pointers and unknown types
    if normalized.endswith("**"):
        result = "ctypes.POINTER(ctypes.c_void_p)"
    return result


def _is_func_ptr_param(param: FuncParam) -> bool:
    """Check if a parameter is a function pointer.

    @brief Detect function pointer syntax in parameter type.
    @version 1
    """
    return "(*" in param.c_type


# ── Code Generator ───────────────────────────────────────


def generate_wrapper(api: ParsedAPI, lib_name: str = "librentropic.so") -> str:
    """Generate complete Python wrapper module source code.

    @brief Main code generation entry point — produces full Python module.
    @version 1
    """
    sections = [
        _gen_header(lib_name),
        _gen_enums(api.enums),
        _gen_callback_types(api.typedefs),
        _gen_library_loader(lib_name),
        _gen_function_bindings(api.functions),
        _gen_error_handling(),
        _gen_owned_string(),
        _gen_engine_class(),
        _gen_generation_result(),
    ]
    return "\n".join(sections)


def _gen_header(lib_name: str) -> str:
    """Generate module header with imports.

    @brief Emit module docstring, imports, and version info.
    @version 1
    """
    return textwrap.dedent(f'''\
        """
        Auto-generated ctypes wrapper for the Entropic C API.

        Generated by scripts/generate_wrapper.py from entropic.h.
        DO NOT EDIT — changes will be overwritten on next build.

        @brief Ctypes bindings for librentropic C API.
        @version 1
        """

        from __future__ import annotations

        import ctypes
        import ctypes.util
        import enum
        import json
        import os
        import threading
        from dataclasses import dataclass, field
        from typing import Any, Callable

        _LIB_NAME = "{lib_name}"

    ''')


def _gen_enums(enums: list[EnumDef]) -> str:
    """Generate Python enum classes from C enums.

    @brief Emit IntEnum classes mirroring C enum typedefs.
    @version 1
    """
    lines = ["# ── Enums ────────────────────────────────────────────", ""]
    for enum_def in enums:
        # Convert C name to Python class name
        py_name = _enum_class_name(enum_def.name)
        lines.append(f"class {py_name}(enum.IntEnum):")
        lines.append(f'    """Mirror of C {enum_def.name}."""')
        lines.append("")

        for val in enum_def.values:
            # Strip common prefix for Python member names
            member_name = _enum_member_name(val.name, enum_def.name)
            doc_comment = f"  # {val.doc}" if val.doc else ""
            lines.append(f"    {member_name} = {val.value}{doc_comment}")

        lines.append("")
        lines.append("")

    return "\n".join(lines)


def _enum_class_name(c_name: str) -> str:
    """Convert C enum typedef name to Python class name.

    entropic_error_t → EntropicError
    entropic_hook_point_t → HookPoint

    @brief C enum name to PascalCase Python class name.
    @version 1
    """
    # Remove entropic_ prefix and _t suffix
    stem = c_name
    if stem.startswith("entropic_"):
        stem = stem[len("entropic_") :]
    if stem.endswith("_t"):
        stem = stem[:-2]

    # Convert to PascalCase
    return "".join(word.capitalize() for word in stem.split("_"))


def _enum_member_name(c_name: str, enum_c_name: str) -> str:
    """Convert C enum member name to Python member name.

    ENTROPIC_OK → OK
    ENTROPIC_ERROR_INVALID_CONFIG → INVALID_CONFIG
    ENTROPIC_HOOK_PRE_GENERATE → PRE_GENERATE

    @brief Strip common C prefix from enum member name.
    @version 2
    """
    prefix_map = {
        "entropic_error_t": "ENTROPIC_ERROR_",
        "entropic_hook_point_t": "ENTROPIC_HOOK_",
    }
    prefix = prefix_map.get(enum_c_name, "ENTROPIC_")

    # Special case: ENTROPIC_OK doesn't follow ENTROPIC_ERROR_ pattern
    if c_name.startswith(prefix):
        return c_name[len(prefix) :]
    return c_name.removeprefix("ENTROPIC_")


def _gen_callback_types(typedefs: list[TypedefDecl]) -> str:
    """Generate ctypes CFUNCTYPE declarations for callback typedefs.

    @brief Emit CFUNCTYPE definitions for C callback function pointers.
    @version 1
    """
    lines = ["# ── Callback Types ───────────────────────────────────", ""]

    for td in typedefs:
        if td.kind != "callback":
            continue

        ret_ctype = _c_type_to_ctypes(td.callback_return)
        param_ctypes = [_c_type_to_ctypes(p.c_type) for p in td.callback_params]
        params_str = ", ".join([ret_ctype, *param_ctypes])
        lines.append(f"{td.name} = ctypes.CFUNCTYPE({params_str})")

    lines.append("")
    lines.append("")
    return "\n".join(lines)


def _gen_library_loader(lib_name: str) -> str:
    """Generate library discovery and loading code.

    @brief Emit _find_library() and module-level _lib loading.
    @version 1
    """
    return textwrap.dedent(f'''\
        # ── Library Discovery ────────────────────────────────────


        def _find_library() -> ctypes.CDLL:
            """Locate and load {lib_name}.

            Search order:
            1. ENTROPIC_LIB_PATH environment variable
            2. Adjacent to this module (python/entropic/lib/)
            3. System library paths (ldconfig)
            4. Build directory

            @brief Find and load the entropic shared library.
            @version 1
            """
            search_paths = [
                os.environ.get("ENTROPIC_LIB_PATH"),
                os.path.join(os.path.dirname(__file__), "lib", "{lib_name}"),
            ]

            for path in search_paths:
                if path and os.path.isfile(path):
                    return ctypes.CDLL(path)

            # Try system library paths
            system_lib = ctypes.util.find_library("rentropic")
            if system_lib:
                return ctypes.CDLL(system_lib)

            raise ImportError(
                "{lib_name} not found. Set ENTROPIC_LIB_PATH or install entropic-engine. "
                "Searched: ENTROPIC_LIB_PATH env var, module lib/ dir, system paths."
            )


        # Load library at import time — fail fast if not found
        _lib: ctypes.CDLL | None = None


        def _get_lib() -> ctypes.CDLL:
            """Get the loaded library, loading lazily on first access.

            @brief Lazy library loader — defers ImportError until first use.
            @version 1
            """
            global _lib  # noqa: PLW0603
            if _lib is None:
                _lib = _find_library()
            return _lib


    ''')


def _gen_function_bindings(functions: list[FuncDecl]) -> str:
    """Generate ctypes function binding declarations.

    @brief Emit argtypes/restype setup for each exported C function.
    @version 1
    """
    lines = [
        "# ── Function Bindings ────────────────────────────────",
        "",
        "",
        "def _setup_bindings(lib: ctypes.CDLL) -> None:",
        '    """Configure argtypes and restype for all C API functions.',
        "",
        "    @brief Set ctypes type signatures for all exported functions.",
        "    @version 1",
        '    """',
    ]

    for func in functions:
        # Build argtypes list
        argtypes = []
        for param in func.params:
            if _is_func_ptr_param(param):
                # Use ctypes.c_void_p for function pointers
                # (actual CFUNCTYPE cast happens at call site)
                argtypes.append("ctypes.c_void_p")
            else:
                argtypes.append(_c_type_to_ctypes(param.c_type))

        restype = _c_type_to_ctypes(func.return_type)

        lines.append(f"    lib.{func.name}.restype = {restype}")
        if argtypes:
            args_str = ", ".join(argtypes)
            lines.append(f"    lib.{func.name}.argtypes = [{args_str}]")
        else:
            lines.append(f"    lib.{func.name}.argtypes = []")
        lines.append("")

    lines.append("")
    return "\n".join(lines)


def _gen_error_handling() -> str:
    """Generate error exception class and check function.

    @brief Emit EntropicError exception and _check_error helper.
    @version 1
    """
    return textwrap.dedent('''\
        # ── Error Handling ───────────────────────────────────────


        class EntropicError(Exception):
            """Exception raised for entropic C engine errors.

            @brief Python exception wrapping entropic_error_t codes.
            @version 1
            """

            def __init__(self, code: int, message: str):
                self.code = code
                self.message = message
                try:
                    self.error_type = Error(code)
                except ValueError:
                    self.error_type = None
                super().__init__(f"entropic error {code}: {message}")


        def _check_error(handle: ctypes.c_void_p, error_code: int) -> None:
            """Check an entropic_error_t return and raise on failure.

            @brief Convert non-OK error codes to EntropicError exceptions.
            @version 1
            """
            if error_code == 0:
                return
            lib = _get_lib()
            msg_ptr = lib.entropic_last_error(handle)
            msg = msg_ptr.decode() if msg_ptr else "unknown error"
            raise EntropicError(error_code, msg)


    ''')


def _gen_owned_string() -> str:
    """Generate RAII wrapper for engine-allocated strings.

    @brief Emit _OwnedString class for automatic entropic_free() calls.
    @version 1
    """
    return textwrap.dedent('''\
        # ── Memory Management ────────────────────────────────────


        class _OwnedString:
            """RAII wrapper for strings allocated by the C engine.

            Ensures entropic_free() is called when the string is no longer needed.

            @brief Auto-freeing wrapper for engine-allocated char* strings.
            @version 1
            """

            def __init__(self, ptr: ctypes.c_void_p):
                self._ptr = ptr

            def __str__(self) -> str:
                if self._ptr:
                    return ctypes.cast(self._ptr, ctypes.c_char_p).value.decode()
                return ""

            def __del__(self) -> None:
                if self._ptr:
                    lib = _get_lib()
                    lib.entropic_free(self._ptr)
                    self._ptr = None


    ''')


def _gen_generation_result() -> str:
    """Generate GenerationResult dataclass.

    @brief Emit dataclass for parsed JSON generation results.
    @version 1
    """
    return textwrap.dedent('''\
        # ── Result Types ─────────────────────────────────────────


        @dataclass
        class Message:
            """A conversation message.

            @brief Message with role, content, and optional metadata.
            @version 1
            """

            role: str = ""
            content: str = ""
            metadata: dict[str, Any] = field(default_factory=dict)
            tool_calls: list[dict[str, Any]] = field(default_factory=list)

            @classmethod
            def from_dict(cls, data: dict[str, Any]) -> "Message":
                """Create from JSON dict.

                @brief Construct Message from deserialized JSON.
                @version 1
                """
                return cls(
                    role=data.get("role", ""),
                    content=data.get("content", ""),
                    metadata=data.get("metadata", {}),
                    tool_calls=data.get("tool_calls", []),
                )


        @dataclass
        class ToolCall:
            """A tool call extracted from a generation result.

            @brief Tool invocation with name and arguments.
            @version 1
            """

            name: str = ""
            arguments: dict[str, Any] = field(default_factory=dict)

            @classmethod
            def from_dict(cls, data: dict[str, Any]) -> "ToolCall":
                """Create from JSON dict.

                @brief Construct ToolCall from deserialized JSON.
                @version 1
                """
                return cls(
                    name=data.get("name", ""),
                    arguments=data.get("arguments", {}),
                )


        @dataclass
        class GenerationResult:
            """Result from an entropic_run() call.

            @brief Parsed generation result with response, messages, metrics.
            @version 1
            """

            response: str = ""
            messages: list[Message] = field(default_factory=list)
            tool_calls: list[ToolCall] = field(default_factory=list)
            token_count: int = 0
            generation_time_ms: float = 0.0
            model_name: str = ""
            tier: str = ""
            raw_json: dict[str, Any] = field(default_factory=dict)

            @classmethod
            def from_json(cls, json_str: str) -> "GenerationResult":
                """Parse a JSON result string from the C engine.

                @brief Deserialize JSON from entropic_run into GenerationResult.
                @version 1
                """
                data = json.loads(json_str)
                return cls(
                    response=data.get("response", ""),
                    messages=[
                        Message.from_dict(m) for m in data.get("messages", [])
                    ],
                    tool_calls=[
                        ToolCall.from_dict(tc) for tc in data.get("tool_calls", [])
                    ],
                    token_count=data.get("token_count", 0),
                    generation_time_ms=data.get("generation_time_ms", 0.0),
                    model_name=data.get("model_name", ""),
                    tier=data.get("tier", ""),
                    raw_json=data,
                )


    ''')


def _gen_engine_class() -> str:
    """Generate the EntropicEngine wrapper class.

    @brief Emit high-level Engine class with handle lifecycle management.
    @version 1
    """
    return textwrap.dedent('''\
        # ── Engine Class ─────────────────────────────────────────


        # ctypes callback type for streaming tokens
        _StreamCallbackType = ctypes.CFUNCTYPE(
            None,
            ctypes.c_char_p,  # token
            ctypes.c_size_t,  # len
            ctypes.c_void_p,  # user_data
        )


        class EntropicEngine:
            """High-level wrapper around the entropic C engine.

            Manages the lifecycle of an entropic_handle_t. Use as a context
            manager or call destroy() explicitly.

            @brief Python wrapper for entropic engine handle lifecycle.
            @version 1
            """

            def __init__(
                self,
                config_path: str | None = None,
                config_json: str | None = None,
            ):
                """Create and optionally configure an engine instance.

                @brief Create engine handle and apply configuration.
                @version 1
                """
                lib = _get_lib()
                _setup_bindings(lib)

                self._handle = ctypes.c_void_p()
                _check_error(None, lib.entropic_create(ctypes.byref(self._handle)))

                if not self._handle:
                    raise RuntimeError("entropic_create returned NULL handle")

                try:
                    if config_path:
                        _check_error(
                            self._handle,
                            lib.entropic_configure_from_file(
                                self._handle, config_path.encode(),
                            ),
                        )
                    elif config_json:
                        _check_error(
                            self._handle,
                            lib.entropic_configure(
                                self._handle, config_json.encode(),
                            ),
                        )
                except Exception:
                    lib.entropic_destroy(self._handle)
                    self._handle = None
                    raise

                self._lib = lib

            def run(self, prompt: str) -> GenerationResult:
                """Run the agentic loop synchronously.

                @brief Execute entropic_run and return parsed result.
                @version 1
                """
                result_ptr = ctypes.c_char_p()
                _check_error(
                    self._handle,
                    self._lib.entropic_run(
                        self._handle,
                        prompt.encode(),
                        ctypes.byref(result_ptr),
                    ),
                )
                try:
                    result_json = result_ptr.value.decode() if result_ptr.value else "{}"
                    return GenerationResult.from_json(result_json)
                finally:
                    if result_ptr.value:
                        self._lib.entropic_free(result_ptr)

            def run_streaming(
                self,
                prompt: str,
                on_token: Callable[[str], None],
                cancel: threading.Event | None = None,
            ) -> None:
                """Run the agentic loop with streaming token callback.

                @brief Execute entropic_run_streaming with Python callback.
                @version 1
                """
                cancel_flag = ctypes.c_int(0)

                def _cancel_monitor() -> None:
                    """Monitor cancel event and set flag.

                    @brief Background thread to bridge Event to C cancel_flag.
                    @version 1
                    """
                    if cancel:
                        cancel.wait()
                        cancel_flag.value = 1

                if cancel:
                    monitor = threading.Thread(target=_cancel_monitor, daemon=True)
                    monitor.start()

                @_StreamCallbackType
                def _on_token_cb(
                    token: bytes,
                    length: int,
                    user_data: ctypes.c_void_p,  # noqa: ARG001
                ) -> None:
                    """Forward C token callback to Python callable.

                    @brief Bridge ctypes callback to user-provided on_token.
                    @version 1
                    """
                    if token:
                        on_token(token[:length].decode())

                _check_error(
                    self._handle,
                    self._lib.entropic_run_streaming(
                        self._handle,
                        prompt.encode(),
                        _on_token_cb,
                        None,
                        ctypes.byref(cancel_flag),
                    ),
                )

            def interrupt(self) -> None:
                """Interrupt a running generation.

                @brief Signal the engine to abort current run.
                @version 1
                """
                _check_error(
                    self._handle,
                    self._lib.entropic_interrupt(self._handle),
                )

            def load_identity(self, name: str) -> None:
                """Load an identity by name.

                @brief Set active identity for subsequent runs.
                @version 1
                """
                _check_error(
                    self._handle,
                    self._lib.entropic_load_identity(
                        self._handle, name.encode(),
                    ),
                )

            def get_identity(self) -> dict[str, Any]:
                """Get the current active identity as a dict.

                @brief Retrieve active identity JSON from C engine.
                @version 1
                """
                result_ptr = ctypes.c_char_p()
                _check_error(
                    self._handle,
                    self._lib.entropic_get_identity(
                        self._handle, ctypes.byref(result_ptr),
                    ),
                )
                try:
                    return json.loads(result_ptr.value.decode() if result_ptr.value else "{}")
                finally:
                    if result_ptr.value:
                        self._lib.entropic_free(result_ptr)

            def configure(self, config_json: str) -> None:
                """Apply configuration from a JSON string.

                @brief Configure engine from JSON after creation.
                @version 1
                """
                _check_error(
                    self._handle,
                    self._lib.entropic_configure(
                        self._handle, config_json.encode(),
                    ),
                )

            def configure_from_file(self, config_path: str) -> None:
                """Apply configuration from a YAML/JSON file.

                @brief Configure engine from file after creation.
                @version 1
                """
                _check_error(
                    self._handle,
                    self._lib.entropic_configure_from_file(
                        self._handle, config_path.encode(),
                    ),
                )

            def storage_open(self, db_path: str) -> None:
                """Open SQLite storage backend.

                @brief Initialize persistent storage for conversations.
                @version 1
                """
                _check_error(
                    self._handle,
                    self._lib.entropic_storage_open(
                        self._handle, db_path.encode(),
                    ),
                )

            def storage_close(self) -> None:
                """Close the storage backend.

                @brief Flush and close storage.
                @version 1
                """
                _check_error(
                    self._handle,
                    self._lib.entropic_storage_close(self._handle),
                )

            def register_mcp_server(self, name: str, config_json: str) -> None:
                """Register an external MCP server.

                @brief Connect and register an external MCP server at runtime.
                @version 1
                """
                _check_error(
                    self._handle,
                    self._lib.entropic_register_mcp_server(
                        self._handle, name.encode(), config_json.encode(),
                    ),
                )

            def deregister_mcp_server(self, name: str) -> None:
                """Deregister an external MCP server.

                @brief Disconnect and remove a registered MCP server.
                @version 1
                """
                _check_error(
                    self._handle,
                    self._lib.entropic_deregister_mcp_server(
                        self._handle, name.encode(),
                    ),
                )

            def list_mcp_servers(self) -> dict[str, Any]:
                """List all MCP servers with status.

                @brief Get server info JSON from C engine.
                @version 1
                """
                result_ptr = self._lib.entropic_list_mcp_servers(self._handle)
                if not result_ptr:
                    return {}
                try:
                    return json.loads(
                        ctypes.cast(result_ptr, ctypes.c_char_p).value.decode()
                    )
                finally:
                    self._lib.entropic_free(result_ptr)

            def version(self) -> str:
                """Get the library version string.

                @brief Return semver string from C engine.
                @version 1
                """
                return self._lib.entropic_version().decode()

            def api_version(self) -> int:
                """Get the C API version integer.

                @brief Return API version for compatibility checks.
                @version 1
                """
                return self._lib.entropic_api_version()

            def destroy(self) -> None:
                """Destroy the engine handle and free resources.

                @brief Release all engine resources. Handle becomes invalid.
                @version 1
                """
                if self._handle:
                    self._lib.entropic_destroy(self._handle)
                    self._handle = None

            def __del__(self) -> None:
                """Destructor — ensures handle is freed.

                @brief Release handle on garbage collection.
                @version 1
                """
                self.destroy()

            def __enter__(self) -> "EntropicEngine":
                """Context manager entry.

                @brief Return self for with-statement usage.
                @version 1
                """
                return self

            def __exit__(self, *args: object) -> None:
                """Context manager exit — destroys handle.

                @brief Clean up engine on context exit.
                @version 1
                """
                self.destroy()

    ''')


# ── Main Entry Point ─────────────────────────────────────


def _validate_inputs(args: argparse.Namespace) -> str | None:
    """Validate CLI arguments, returning error message or None.

    @brief Check header and type directory paths exist.
    @version 2
    """
    checks = [
        (args.header.is_file(), f"header not found: {args.header}"),
        (args.types_dir.is_dir(), f"types directory not found: {args.types_dir}"),
        *[
            (
                (args.types_dir / f).is_file(),
                f"required type header not found: {args.types_dir / f}",
            )
            for f in ["error.h", "hooks.h"]
        ],
    ]
    failures = [msg for ok, msg in checks if not ok]
    return failures[0] if failures else None


def main() -> int:
    """Parse headers and generate wrapper module.

    @brief CLI entry point for wrapper generation.
    @version 2
    """
    parser = argparse.ArgumentParser(
        description="Generate Python ctypes wrapper from entropic C headers",
    )
    parser.add_argument("--header", required=True, type=Path, help="Path to entropic.h")
    parser.add_argument("--types-dir", required=True, type=Path, help="Path to types/ directory")
    parser.add_argument("--output", required=True, type=Path, help="Output path")
    parser.add_argument("--lib-name", default="librentropic.so", help="Shared library filename")
    args = parser.parse_args()

    error = _validate_inputs(args)
    if error:
        print(f"Error: {error}", file=sys.stderr)
        return 1

    api = parse_headers(args.header, args.types_dir)
    if not api.functions:
        print("Error: no ENTROPIC_EXPORT functions found in header", file=sys.stderr)
        return 1

    output = generate_wrapper(api, lib_name=args.lib_name)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(output)

    print(f"Generated {args.output}:")
    print(f"  {len(api.functions)} functions")
    print(f"  {len(api.enums)} enums ({sum(len(e.values) for e in api.enums)} values)")
    print(f"  {len(api.typedefs)} typedefs")
    return 0


if __name__ == "__main__":
    sys.exit(main())
