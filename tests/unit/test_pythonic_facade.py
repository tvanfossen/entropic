# SPDX-License-Identifier: LGPL-3.0-or-later
"""Unit tests for the Pythonic facade modules (#8, v2.1.4).

Tests entropic.hooks, entropic.streams, and entropic.mcp without
requiring librentropic.so to be loaded. All C ABI calls are
monkey-patched at the _bindings module level.

Coverage:
- @hook decorator registers callables in the pending list
- register_hooks binds pending entries to a handle and keeps trampolines
  alive (per-handle dict)
- register_token_observer wires a Python str observer onto a handle
- register_server marshals kwargs to JSON, raises on missing/conflicting
  command/url

@version 2.1.4
"""

from __future__ import annotations

import ctypes
import json
import sys
from pathlib import Path
from unittest.mock import MagicMock

import pytest

_REPO_ROOT = Path(__file__).resolve().parents[2]
_PY_SRC = _REPO_ROOT / "python" / "src"
sys.path.insert(0, str(_PY_SRC))


@pytest.fixture(autouse=True)
def stub_native_bindings(monkeypatch: pytest.MonkeyPatch) -> None:
    """Stub out _bindings before each test.

    The facade modules import from entropic._bindings at module load.
    To avoid librentropic.so being required, we import _bindings via a
    fresh stub module the first time and patch its symbols on every
    test.
    """
    import importlib

    if "entropic._bindings" not in sys.modules:
        # Build a minimal stub matching the symbols the facade imports.
        stub = type(sys)("entropic._bindings")
        stub.HOOK_CB = ctypes.CFUNCTYPE(
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_char_p,
            ctypes.POINTER(ctypes.c_char_p),
            ctypes.c_void_p,
        )
        stub.STREAM_OBSERVER_CB = ctypes.CFUNCTYPE(
            None,
            ctypes.c_char_p,
            ctypes.c_size_t,
            ctypes.c_void_p,
        )
        stub.entropic_alloc = MagicMock(return_value=0xDEAD_BEEF)
        stub.entropic_register_hook = MagicMock(return_value=0)
        stub.entropic_register_mcp_server = MagicMock(return_value=0)
        stub.entropic_set_stream_observer = MagicMock(return_value=0)

        import enum

        class EntropicHookPoint(enum.IntEnum):
            PRE_TOOL_CALL = 3
            POST_TOOL_CALL = 4

        stub.EntropicHookPoint = EntropicHookPoint
        sys.modules["entropic._bindings"] = stub

    # Re-import the facade modules each test to reset their per-module
    # state (the @hook pending list, the streams keepalive dict).
    for name in ("entropic.hooks", "entropic.streams", "entropic.mcp"):
        if name in sys.modules:
            importlib.reload(sys.modules[name])


# ── @hook decorator ─────────────────────────────────────


def test_hook_decorator_appends_to_pending() -> None:
    from entropic import hooks
    from entropic._bindings import EntropicHookPoint

    hooks._clear_pending()

    @hooks.hook(EntropicHookPoint.PRE_TOOL_CALL)
    def my_hook(ctx: dict) -> None:
        return None

    assert len(hooks._pending) == 1
    assert hooks._pending[0].point == EntropicHookPoint.PRE_TOOL_CALL
    assert hooks._pending[0].func is my_hook


def test_hook_decorator_preserves_priority() -> None:
    from entropic import hooks
    from entropic._bindings import EntropicHookPoint

    hooks._clear_pending()

    @hooks.hook(EntropicHookPoint.POST_TOOL_CALL, priority=42)
    def my_hook(ctx: dict) -> None:
        return None

    assert hooks._pending[0].priority == 42


def test_register_hooks_binds_pending_and_keeps_trampoline_alive() -> None:
    from entropic import _bindings, hooks

    hooks._clear_pending()
    fake_handle = ctypes.c_void_p(0xCAFE)

    @hooks.hook(_bindings.EntropicHookPoint.PRE_TOOL_CALL)
    def my_hook(ctx: dict) -> None:
        return None

    bound = hooks.register_hooks(fake_handle)
    assert bound == 1

    # The trampoline must be retained per-handle to outlive the call.
    handle_key = ctypes.cast(fake_handle, ctypes.c_void_p).value or 0
    assert handle_key in hooks._bound
    assert len(hooks._bound[handle_key]) == 1
    assert hooks._bound[handle_key][0].trampoline is not None

    # entropic_register_hook was called with our hook point.
    _bindings.entropic_register_hook.assert_called_once()
    call_args = _bindings.entropic_register_hook.call_args
    assert call_args.args[0] is fake_handle
    assert call_args.args[1] == int(_bindings.EntropicHookPoint.PRE_TOOL_CALL)


def test_register_hooks_is_idempotent_for_already_bound() -> None:
    from entropic import _bindings, hooks

    hooks._clear_pending()

    @hooks.hook(_bindings.EntropicHookPoint.PRE_TOOL_CALL)
    def my_hook(ctx: dict) -> None:
        return None

    fake_handle = ctypes.c_void_p(0xCAFE)
    hooks.register_hooks(fake_handle)
    bound_again = hooks.register_hooks(fake_handle)
    assert bound_again == 0  # no NEW pending hooks


# ── streams.register_token_observer ─────────────────────


def test_register_token_observer_keeps_callback_alive() -> None:
    from entropic import _bindings, streams

    fake_handle = ctypes.c_void_p(0xBABE)
    captured = []

    def my_observer(text: str) -> None:
        captured.append(text)

    streams.register_token_observer(fake_handle, my_observer)

    # Per-handle keepalive populated.
    handle_key = ctypes.cast(fake_handle, ctypes.c_void_p).value or 0
    assert handle_key in streams._observers

    # entropic_set_stream_observer was called with the trampoline.
    _bindings.entropic_set_stream_observer.assert_called_once()


# ── mcp.register_server ─────────────────────────────────


def test_register_server_requires_command_xor_url() -> None:
    from entropic import mcp

    fake_handle = ctypes.c_void_p(0xFEED)
    with pytest.raises(ValueError, match="exactly one"):
        mcp.register_server(fake_handle, name="bad")
    with pytest.raises(ValueError, match="exactly one"):
        mcp.register_server(fake_handle, name="bad", command="cmd", url="http://x")


def test_register_server_marshals_full_config_to_json() -> None:
    from entropic import _bindings, mcp

    fake_handle = ctypes.c_void_p(0xFEED)
    rc = mcp.register_server(
        fake_handle,
        name="docs",
        command="/usr/local/bin/docs-mcp",
        args=["--mode=server"],
        env={"DOCS_DB": "/srv/docs.db"},
    )
    assert rc == 0
    _bindings.entropic_register_mcp_server.assert_called_once()
    call_args = _bindings.entropic_register_mcp_server.call_args
    assert call_args.args[0] is fake_handle
    assert call_args.args[1] == b"docs"
    config = json.loads(call_args.args[2].decode("utf-8"))
    assert config["command"] == "/usr/local/bin/docs-mcp"
    assert config["args"] == ["--mode=server"]
    assert config["env"] == {"DOCS_DB": "/srv/docs.db"}
    assert config["transport"] == "stdio"


def test_register_server_url_path_uses_sse_transport() -> None:
    from entropic import _bindings, mcp

    fake_handle = ctypes.c_void_p(0xFEED)
    mcp.register_server(fake_handle, name="remote", url="https://x.example/sse")
    config = json.loads(_bindings.entropic_register_mcp_server.call_args.args[2].decode("utf-8"))
    assert config["transport"] == "sse"
    assert config["url"] == "https://x.example/sse"
    assert config["command"] == ""
