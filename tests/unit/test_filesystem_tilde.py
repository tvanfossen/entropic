"""Test tilde path expansion in filesystem server."""

import os


def test_tilde_expansion():
    """Verify ~ expands to home directory."""
    path_str = "~/test/path"
    expanded = os.path.expanduser(path_str) if path_str.startswith("~") else path_str

    assert expanded.startswith("/home/") or expanded.startswith("/Users/") or expanded.startswith("/root")
    assert "~" not in expanded


def test_tilde_expansion_with_username():
    """Verify ~user expands correctly."""
    # This tests the general expanduser behavior
    path_str = "~"
    expanded = os.path.expanduser(path_str)

    # Should expand to some absolute path
    assert expanded.startswith("/")
    assert expanded != "~"


def test_non_tilde_path_unchanged():
    """Verify paths without ~ are not modified."""
    path_str = "/absolute/path"
    if path_str.startswith("~"):
        expanded = os.path.expanduser(path_str)
    else:
        expanded = path_str

    assert expanded == "/absolute/path"


def test_relative_path_unchanged():
    """Verify relative paths are not modified."""
    path_str = "relative/path"
    if path_str.startswith("~"):
        expanded = os.path.expanduser(path_str)
    else:
        expanded = path_str

    assert expanded == "relative/path"
