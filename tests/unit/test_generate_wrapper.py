# SPDX-License-Identifier: LGPL-3.0-or-later
"""Unit tests for scripts/generate_wrapper.py — header parser and code generator.

Tests the parser against the real entropic.h and type headers to ensure
all API elements are correctly extracted and the generated module is valid.

@brief Validate wrapper generator parsing and code generation.
@version 1
"""

import sys
import textwrap
from pathlib import Path

import pytest

# Import the generator module directly
_SCRIPTS_DIR = Path(__file__).resolve().parents[2] / "scripts"
sys.path.insert(0, str(_SCRIPTS_DIR))

from generate_wrapper import (  # noqa: E402
    generate_wrapper,
    parse_callback_typedefs,
    parse_enums,
    parse_exported_functions,
    parse_handle_typedefs,
    parse_headers,
)

# Paths to real headers
_INCLUDE_DIR = Path(__file__).resolve().parents[2] / "include" / "entropic"
_HEADER_PATH = _INCLUDE_DIR / "entropic.h"
_TYPES_DIR = _INCLUDE_DIR / "types"


# ── Parser Tests ─────────────────────────────────────────


class TestParseEnums:
    """Tests for enum parsing from C headers.

    @brief Verify enum extraction from error.h and hooks.h.
    @version 1
    """

    def test_parses_error_enum(self) -> None:
        """Parse entropic_error_t from types/error.h.

        @brief Verify all error enum values are extracted.
        @version 1
        """
        text = (_TYPES_DIR / "error.h").read_text()
        enums = parse_enums(text)
        error_enums = [e for e in enums if e.name == "entropic_error_t"]
        assert len(error_enums) == 1
        error_enum = error_enums[0]
        assert error_enum.values[0].name == "ENTROPIC_OK"
        assert error_enum.values[0].value == 0
        # Should have all error codes
        assert len(error_enum.values) >= 20

    def test_parses_hook_enum(self) -> None:
        """Parse entropic_hook_point_t from types/hooks.h.

        @brief Verify all hook point enum values are extracted.
        @version 1
        """
        text = (_TYPES_DIR / "hooks.h").read_text()
        enums = parse_enums(text)
        hook_enums = [e for e in enums if e.name == "entropic_hook_point_t"]
        assert len(hook_enums) == 1
        hook_enum = hook_enums[0]
        assert hook_enum.values[0].name == "ENTROPIC_HOOK_PRE_GENERATE"
        assert len(hook_enum.values) >= 15

    def test_extracts_inline_docs(self) -> None:
        """Inline //< docs are captured in EnumValue.doc.

        @brief Verify doc comments are extracted from enum members.
        @version 1
        """
        text = (_TYPES_DIR / "error.h").read_text()
        enums = parse_enums(text)
        error_enum = [e for e in enums if e.name == "entropic_error_t"][0]
        ok_val = error_enum.values[0]
        assert ok_val.doc  # Should have "Success" or similar

    def test_enum_values_are_sequential(self) -> None:
        """Enum values without explicit assignment are sequential.

        @brief Verify auto-incrementing enum value assignment.
        @version 1
        """
        text = textwrap.dedent("""\
            typedef enum {
                ENTROPIC_A = 0,
                ENTROPIC_B,
                ENTROPIC_C,
                ENTROPIC_D = 10,
                ENTROPIC_E,
            } entropic_test_t;
        """)
        enums = parse_enums(text)
        assert len(enums) == 1
        vals = {v.name: v.value for v in enums[0].values}
        assert vals["ENTROPIC_A"] == 0
        assert vals["ENTROPIC_B"] == 1
        assert vals["ENTROPIC_C"] == 2
        assert vals["ENTROPIC_D"] == 10
        assert vals["ENTROPIC_E"] == 11


class TestParseHandleTypedefs:
    """Tests for opaque handle typedef parsing.

    @brief Verify handle typedef extraction.
    @version 1
    """

    def test_parses_handle_typedef(self) -> None:
        """Parse typedef struct entropic_engine* entropic_handle_t.

        @brief Verify handle pointer typedef is found.
        @version 1
        """
        text = (_TYPES_DIR / "error.h").read_text()
        handles = parse_handle_typedefs(text)
        handle_names = [h.name for h in handles]
        assert "entropic_handle_t" in handle_names

    def test_handle_kind_is_handle(self) -> None:
        """Handle typedefs have kind='handle'.

        @brief Verify handle typedef kind field.
        @version 1
        """
        text = "typedef struct entropic_engine* entropic_handle_t;"
        handles = parse_handle_typedefs(text)
        assert handles[0].kind == "handle"


class TestParseCallbackTypedefs:
    """Tests for callback function pointer typedef parsing.

    @brief Verify callback typedef extraction from type headers.
    @version 1
    """

    def test_parses_error_callback(self) -> None:
        """Parse entropic_error_callback_t from error.h.

        @brief Verify error callback typedef extraction.
        @version 1
        """
        text = (_TYPES_DIR / "error.h").read_text()
        callbacks = parse_callback_typedefs(text)
        cb_names = [c.name for c in callbacks]
        assert "entropic_error_callback_t" in cb_names

    def test_parses_hook_callback(self) -> None:
        """Parse entropic_hook_callback_t from hooks.h.

        @brief Verify hook callback typedef extraction.
        @version 1
        """
        text = (_TYPES_DIR / "hooks.h").read_text()
        callbacks = parse_callback_typedefs(text)
        cb_names = [c.name for c in callbacks]
        assert "entropic_hook_callback_t" in cb_names

    def test_callback_return_type(self) -> None:
        """Callback return type is correctly extracted.

        @brief Verify callback return type parsing.
        @version 1
        """
        text = "typedef int (*my_callback_t)(int code, const char* msg);"
        callbacks = parse_callback_typedefs(text)
        assert callbacks[0].callback_return == "int"


class TestParseExportedFunctions:
    """Tests for ENTROPIC_EXPORT function parsing.

    @brief Verify exported function extraction from entropic.h.
    @version 1
    """

    def test_parses_all_exported_functions(self) -> None:
        """All ENTROPIC_EXPORT functions in entropic.h are found.

        @brief Verify function count matches expected API surface.
        @version 1
        """
        text = _HEADER_PATH.read_text()
        funcs = parse_exported_functions(text)
        func_names = [f.name for f in funcs]
        # Key functions that must be present
        assert "entropic_create" in func_names
        assert "entropic_destroy" in func_names
        assert "entropic_configure" in func_names
        assert "entropic_run" in func_names
        assert "entropic_run_streaming" in func_names
        assert "entropic_interrupt" in func_names
        assert "entropic_free" in func_names
        assert "entropic_version" in func_names

    def test_run_streaming_has_callback_param(self) -> None:
        """entropic_run_streaming function pointer param is parsed.

        @brief Verify nested-paren function pointer param is captured.
        @version 1
        """
        text = _HEADER_PATH.read_text()
        funcs = parse_exported_functions(text)
        streaming = [f for f in funcs if f.name == "entropic_run_streaming"]
        assert len(streaming) == 1
        # Should have 5 params: handle, input, on_token, user_data, cancel_flag
        assert len(streaming[0].params) == 5

    def test_return_types_are_correct(self) -> None:
        """Return types match expected C API conventions.

        @brief Verify return type extraction for key functions.
        @version 1
        """
        text = _HEADER_PATH.read_text()
        funcs = {f.name: f for f in parse_exported_functions(text)}
        assert funcs["entropic_create"].return_type == "entropic_error_t"
        assert funcs["entropic_destroy"].return_type == "void"
        assert funcs["entropic_version"].return_type == "const char*"

    def test_void_params_produce_empty_list(self) -> None:
        """Functions with (void) produce empty param list.

        @brief Verify void parameter handling.
        @version 1
        """
        text = _HEADER_PATH.read_text()
        funcs = {f.name: f for f in parse_exported_functions(text)}
        assert funcs["entropic_version"].params == []


class TestParseHeaders:
    """Integration test for full header parsing.

    @brief Verify parse_headers produces complete API surface.
    @version 1
    """

    def test_parses_complete_api(self) -> None:
        """parse_headers extracts functions, enums, and typedefs.

        @brief End-to-end parse of all headers.
        @version 1
        """
        api = parse_headers(_HEADER_PATH, _TYPES_DIR)
        assert len(api.functions) >= 15
        assert len(api.enums) >= 2
        assert len(api.typedefs) >= 3


# ── Generator Tests ──────────────────────────────────────


class TestGenerateWrapper:
    """Tests for the code generation output.

    @brief Verify generated Python module structure and content.
    @version 1
    """

    @pytest.fixture
    def generated_source(self) -> str:
        """Generate wrapper source from real headers.

        @brief Run generator against real headers and return source.
        @version 1
        """
        api = parse_headers(_HEADER_PATH, _TYPES_DIR)
        return generate_wrapper(api)

    def test_generated_source_compiles(self, generated_source: str) -> None:
        """Generated source compiles without syntax errors.

        @brief Verify generated code is syntactically valid Python.
        @version 1
        """
        compile(generated_source, "<generated>", "exec")

    def test_contains_error_enum(self, generated_source: str) -> None:
        """Generated source contains Error enum class.

        @brief Verify Error IntEnum is present in output.
        @version 1
        """
        assert "class Error(enum.IntEnum):" in generated_source
        assert "OK = 0" in generated_source

    def test_contains_hook_enum(self, generated_source: str) -> None:
        """Generated source contains HookPoint enum class.

        @brief Verify HookPoint IntEnum is present in output.
        @version 1
        """
        assert "class HookPoint(enum.IntEnum):" in generated_source
        assert "PRE_GENERATE = 0" in generated_source

    def test_contains_engine_class(self, generated_source: str) -> None:
        """Generated source contains EntropicEngine class.

        @brief Verify EntropicEngine wrapper class is present.
        @version 1
        """
        assert "class EntropicEngine:" in generated_source
        assert "def run(" in generated_source
        assert "def run_streaming(" in generated_source
        assert "def interrupt(" in generated_source
        assert "def __enter__(" in generated_source
        assert "def __exit__(" in generated_source

    def test_contains_error_exception(self, generated_source: str) -> None:
        """Generated source contains EntropicError exception.

        @brief Verify EntropicError exception class is present.
        @version 1
        """
        assert "class EntropicError(Exception):" in generated_source

    def test_contains_generation_result(self, generated_source: str) -> None:
        """Generated source contains GenerationResult dataclass.

        @brief Verify GenerationResult is present in output.
        @version 1
        """
        assert "class GenerationResult:" in generated_source
        assert "def from_json(" in generated_source

    def test_contains_library_discovery(self, generated_source: str) -> None:
        """Generated source contains library discovery function.

        @brief Verify _find_library function is present.
        @version 1
        """
        assert "def _find_library()" in generated_source
        assert "ENTROPIC_LIB_PATH" in generated_source

    def test_contains_setup_bindings(self, generated_source: str) -> None:
        """Generated source contains function binding setup.

        @brief Verify _setup_bindings configures all functions.
        @version 1
        """
        assert "def _setup_bindings(" in generated_source
        assert "entropic_create" in generated_source
        assert "entropic_run_streaming" in generated_source

    def test_contains_callback_types(self, generated_source: str) -> None:
        """Generated source contains CFUNCTYPE callback definitions.

        @brief Verify callback type definitions are present.
        @version 1
        """
        assert "entropic_error_callback_t = ctypes.CFUNCTYPE" in generated_source
        assert "entropic_hook_callback_t = ctypes.CFUNCTYPE" in generated_source


class TestGeneratorFailure:
    """Tests for generator error handling.

    @brief Verify generator fails gracefully on bad input.
    @version 1
    """

    def test_no_functions_produces_empty_list(self) -> None:
        """Header with no ENTROPIC_EXPORT produces empty function list.

        @brief Verify parser handles headers with no exports.
        @version 1
        """
        text = "// No exports here\nint foo(void);\n"
        funcs = parse_exported_functions(text)
        assert funcs == []

    def test_malformed_enum_skipped(self) -> None:
        """Malformed enum body doesn't crash parser.

        @brief Verify parser resilience to malformed enums.
        @version 1
        """
        text = "typedef enum { ??? broken } bad_t;"
        enums = parse_enums(text)
        # Should either produce an empty enum or skip it
        assert isinstance(enums, list)
