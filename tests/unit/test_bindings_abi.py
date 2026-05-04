# SPDX-License-Identifier: LGPL-3.0-or-later
"""ABI conformance tests for python/src/entropic/_bindings.py enums.

The hand-curated IntEnums in ``_bindings.py`` mirror C enums declared in
``include/entropic/types/`` headers. Drift silently mislabels states by
integer value (issue #1: 7-state AgentState wrapper against 10-state C
ABI in v2.1.0). These tests parse the canonical C headers and assert
that every (name, value) pair declared on the C side has a matching
entry in the Python enum.

The parse uses Python's ``ast`` module against ``_bindings.py`` source
rather than importing the module — importing triggers ``_loader.load()``
which requires librentropic.so to be present, which is not the case in
the unit-test environment.

@brief Verify Python IntEnum bindings match C ABI enum declarations.
@version 2.1.1
"""

from __future__ import annotations

import ast
import re
from pathlib import Path

import pytest

_REPO_ROOT = Path(__file__).resolve().parents[2]
_BINDINGS_PATH = _REPO_ROOT / "python" / "src" / "entropic" / "_bindings.py"
_TYPES_DIR = _REPO_ROOT / "include" / "entropic" / "types"


## @brief Parse a typedef enum block from C header text into {name: value}.
## @utility
## @version 2.1.1
def _parse_c_enum(header_text: str, c_typedef_name: str) -> dict[str, int]:
    """Extract members of a single ``typedef enum { ... } <name>;`` block.

    Returns a dict mapping ``ENTROPIC_FOO_BAR`` → integer value, with
    auto-incrementing applied for members lacking an explicit ``= N``.
    The C side uses ``ENTROPIC_<FAMILY>_<MEMBER>`` and the Python side
    drops the family prefix (e.g. ``ENTROPIC_AGENT_STATE_IDLE`` → ``IDLE``);
    callers that compare enums apply the prefix-strip themselves.

    Comments are stripped BEFORE the enum-body regex matches, so that
    doc-block content containing literal ``{`` / ``}`` braces (e.g.
    @code{.json} examples in hooks.h) does not break the body capture.
    """
    # Strip comments globally first so brace-balanced doc blocks inside
    # the typedef body don't terminate the regex prematurely. (#8 v2.1.4)
    cleaned = re.sub(r"//.*?$", "", header_text, flags=re.MULTILINE)
    cleaned = re.sub(r"/\*.*?\*/", "", cleaned, flags=re.DOTALL)
    pattern = re.compile(
        r"typedef\s+enum\s*\{([^}]*)\}\s*" + re.escape(c_typedef_name) + r"\s*;",
        re.DOTALL,
    )
    m = pattern.search(cleaned)
    if m is None:
        raise AssertionError(f"typedef enum {c_typedef_name} not found in header")
    body = m.group(1)
    members: dict[str, int] = {}
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
        members[name] = value
        next_value = value + 1
    return members


## @brief Extract a (name, int_value) pair from an AST assignment, or None.
## @utility
## @version 2.1.1
def _intenum_member_from_assign(stmt: ast.stmt) -> tuple[str, int] | None:
    """Return ``(NAME, value)`` if ``stmt`` is an ``IntEnum`` member, else None.

    Filters out non-Assign nodes, multi-target assignments, non-Name
    targets, non-Constant or non-int values. Returning None lets the
    caller skip the entry without nesting more conditionals.
    """
    if not isinstance(stmt, ast.Assign) or len(stmt.targets) != 1:
        return None
    target = stmt.targets[0]
    value = stmt.value
    valid = (
        isinstance(target, ast.Name)
        and isinstance(value, ast.Constant)
        and isinstance(value.value, int)
    )
    # ``valid`` is a single boolean so the function has only two return
    # statements (max-returns ≤ 3). Type narrowing below is via assert.
    if not valid:
        return None
    assert isinstance(target, ast.Name)
    assert isinstance(value, ast.Constant) and isinstance(value.value, int)
    return target.id, value.value


## @brief Find the first top-level ClassDef matching the given name.
## @utility
## @version 2.1.1
def _find_class_def(source: str, class_name: str) -> ast.ClassDef:
    """Return the AST ClassDef for ``class_name``; raise if absent."""
    tree = ast.parse(source)
    for node in tree.body:
        if isinstance(node, ast.ClassDef) and node.name == class_name:
            return node
    raise AssertionError(f"class {class_name} not found in {_BINDINGS_PATH.name}")


## @brief Extract an IntEnum class body from _bindings.py source via ast.
## @utility
## @version 2.1.1
def _parse_python_intenum(source: str, class_name: str) -> dict[str, int]:
    """Return ``{NAME: value}`` for a top-level ``class X(enum.IntEnum)``.

    Parses the file as AST so the ABI test does not depend on
    ``librentropic.so`` being importable at test time.
    """
    cls = _find_class_def(source, class_name)
    members: dict[str, int] = {}
    for stmt in cls.body:
        pair = _intenum_member_from_assign(stmt)
        if pair is not None:
            members[pair[0]] = pair[1]
    return members


## @brief Strip the ENTROPIC_<family>_ prefix from a C enum member name.
## @utility
## @version 2.1.1
def _strip_prefix(c_name: str, prefix: str) -> str:
    assert c_name.startswith(prefix), f"{c_name!r} missing prefix {prefix!r}"
    return c_name[len(prefix) :]


@pytest.fixture(scope="module")
def bindings_source() -> str:
    """Read _bindings.py source once per module.

    @brief Source text fixture for AST-based enum extraction.
    @version 2.1.1
    """
    return _BINDINGS_PATH.read_text()


# ── AgentState ──────────────────────────────────────────


class TestAgentStateAbiConformance:
    """Pin Python AgentState IntEnum to entropic_agent_state_t.

    @brief Issue #1 regression: 7-state Python wrapper drifted from
           10-state C ABI in v2.1.0.
    @version 2.1.1
    """

    @pytest.fixture(scope="class")
    def c_members(self) -> dict[str, int]:
        text = (_TYPES_DIR / "enums.h").read_text()
        return _parse_c_enum(text, "entropic_agent_state_t")

    @pytest.fixture(scope="class")
    def py_members(self, bindings_source: str) -> dict[str, int]:
        return _parse_python_intenum(bindings_source, "AgentState")

    def test_every_c_value_has_python_entry(
        self, c_members: dict[str, int], py_members: dict[str, int]
    ) -> None:
        """Each C value/name pair appears in the Python IntEnum.

        @brief Forward conformance: C → Python.
        @version 2.1.1
        """
        missing: list[str] = []
        wrong_value: list[tuple[str, int, int]] = []
        for c_name, c_val in c_members.items():
            py_name = _strip_prefix(c_name, "ENTROPIC_AGENT_STATE_")
            if py_name not in py_members:
                missing.append(py_name)
            elif py_members[py_name] != c_val:
                wrong_value.append((py_name, c_val, py_members[py_name]))
        assert not missing, f"AgentState missing C states: {missing}"
        assert not wrong_value, (
            "AgentState value drift (name, c_value, py_value): " f"{wrong_value}"
        )

    def test_no_extra_python_entries(
        self, c_members: dict[str, int], py_members: dict[str, int]
    ) -> None:
        """Python IntEnum has no entries absent from the C ABI.

        @brief Reverse conformance: prevents Python from inventing states
               the engine doesn't emit.
        @version 2.1.1
        """
        c_short_names = {_strip_prefix(n, "ENTROPIC_AGENT_STATE_") for n in c_members}
        extra = set(py_members) - c_short_names
        assert not extra, f"AgentState has Python-only entries: {extra}"

    def test_count_matches(self, c_members: dict[str, int], py_members: dict[str, int]) -> None:
        """The two enums declare the same number of members.

        @brief Cardinality sanity check.
        @version 2.1.1
        """
        assert len(c_members) == len(py_members)


# ── EntropicError ───────────────────────────────────────


class TestEntropicErrorAbiConformance:
    """Pin Python EntropicError IntEnum to entropic_error_t.

    @brief Same regression class as AgentState — error codes mismatch
           silently translate to wrong error names.
    @version 2.1.1
    """

    @pytest.fixture(scope="class")
    def c_members(self) -> dict[str, int]:
        text = (_TYPES_DIR / "error.h").read_text()
        return _parse_c_enum(text, "entropic_error_t")

    @pytest.fixture(scope="class")
    def py_members(self, bindings_source: str) -> dict[str, int]:
        return _parse_python_intenum(bindings_source, "EntropicError")

    def test_every_c_value_has_python_entry(
        self, c_members: dict[str, int], py_members: dict[str, int]
    ) -> None:
        """Each C error code/name appears in the Python IntEnum.

        @brief Forward conformance: C → Python.
        @version 2.1.1
        """
        missing: list[str] = []
        wrong_value: list[tuple[str, int, int]] = []
        for c_name, c_val in c_members.items():
            py_name = "OK" if c_name == "ENTROPIC_OK" else _strip_prefix(c_name, "ENTROPIC_ERROR_")
            if py_name not in py_members:
                missing.append(py_name)
            elif py_members[py_name] != c_val:
                wrong_value.append((py_name, c_val, py_members[py_name]))
        assert not missing, f"EntropicError missing C codes: {missing}"
        assert not wrong_value, (
            "EntropicError value drift (name, c_value, py_value): " f"{wrong_value}"
        )


# ── EntropicHookPoint (#8, v2.1.4) ──────────────────────


class TestEntropicHookPointAbiConformance:
    """Pin Python EntropicHookPoint IntEnum to entropic_hook_point_t.

    Same regression class as AgentState/EntropicError. Issue #8 (v2.1.4).
    Excludes the sentinel `ENTROPIC_HOOK_COUNT_` — it's not a valid hook
    point, just a count marker.

    @version 2.1.4
    """

    @pytest.fixture(scope="class")
    def c_members(self) -> dict[str, int]:
        text = (_TYPES_DIR / "hooks.h").read_text()
        all_members = _parse_c_enum(text, "entropic_hook_point_t")
        # Drop the sentinel — Python intentionally does NOT mirror it.
        return {k: v for k, v in all_members.items() if k != "ENTROPIC_HOOK_COUNT_"}

    @pytest.fixture(scope="class")
    def py_members(self, bindings_source: str) -> dict[str, int]:
        return _parse_python_intenum(bindings_source, "EntropicHookPoint")

    def test_every_c_value_has_python_entry(
        self, c_members: dict[str, int], py_members: dict[str, int]
    ) -> None:
        """Each C hook value/name appears in the Python IntEnum.

        @version 2.1.4
        """
        missing: list[str] = []
        wrong_value: list[tuple[str, int, int]] = []
        for c_name, c_val in c_members.items():
            py_name = _strip_prefix(c_name, "ENTROPIC_HOOK_")
            if py_name not in py_members:
                missing.append(py_name)
            elif py_members[py_name] != c_val:
                wrong_value.append((py_name, c_val, py_members[py_name]))
        assert not missing, f"EntropicHookPoint missing C points: {missing}"
        assert not wrong_value, (
            "EntropicHookPoint value drift (name, c_value, py_value): " f"{wrong_value}"
        )

    def test_no_extra_python_entries(
        self, c_members: dict[str, int], py_members: dict[str, int]
    ) -> None:
        """Python IntEnum has no entries absent from the C ABI.

        @version 2.1.4
        """
        c_short = {_strip_prefix(n, "ENTROPIC_HOOK_") for n in c_members}
        extra = set(py_members) - c_short
        assert not extra, f"EntropicHookPoint has Python-only entries: {extra}"


# ── #8 (v2.1.4): missing-symbol presence test ───────────


class TestPipWrapperSymbols:
    """Pin the four 2.1.4 ABI symbols + their CFUNCTYPEs to _bindings.py.

    Issue #8: pre-2.1.4 the pip wrapper didn't expose entropic_alloc /
    entropic_set_stream_observer / entropic_register_mcp_server /
    entropic_register_hook (or HOOK_CB / STREAM_OBSERVER_CB). This test
    asserts they are now declared so a future refactor doesn't silently
    drop them.

    @version 2.1.4
    """

    REQUIRED_SYMBOLS = (
        "entropic_alloc",
        "entropic_set_stream_observer",
        "entropic_register_mcp_server",
        "entropic_register_hook",
        "STREAM_OBSERVER_CB",
        "HOOK_CB",
        "EntropicHookPoint",
    )

    def test_all_2_1_4_symbols_declared(self, bindings_source: str) -> None:
        """Every required v2.1.4 symbol must appear in _bindings.py.

        Uses simple string presence (each symbol shows up once at
        declaration). Avoids importing _bindings (which requires
        librentropic.so).

        @version 2.1.4
        """
        missing = [sym for sym in self.REQUIRED_SYMBOLS if sym not in bindings_source]
        assert not missing, "_bindings.py missing v2.1.4 symbols (#8): " f"{missing}"
