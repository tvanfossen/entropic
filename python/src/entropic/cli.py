# SPDX-License-Identifier: LGPL-3.0-or-later
"""``entropic`` console script — dispatches to the bundled native binary.

Two paths:
    * ``entropic install-engine [--version V] [--backend cpu|cuda]`` —
      handled in-process by :mod:`entropic.install_engine`.
    * Everything else (``entropic version``, ``entropic mcp-bridge``,
      ``entropic mcp-connect``, ``entropic download``) — ``execvp``'d
      to the ``bin/entropic`` binary that ``install-engine`` laid
      down. The Python process is replaced; the native binary
      receives stdin/stdout/stderr verbatim.

The native binary's full subcommand surface is documented in
``docs/cli-install-routes.md``. There is no ``entropic ask`` — that
example was a doc-bug from earlier drafts; the engine drives a
conversation loop via the MCP bridge or via direct ctypes use, not
via a one-shot ``ask`` subcommand.

If the binary cannot be located, the user is told to run
``entropic install-engine`` and the process exits with code 2.
"""

from __future__ import annotations

import os
import sys

from entropic import _loader, install_engine


## @brief Replace this process with bin/entropic argv...; return on failure.
## @utility
## @version 2.1.0
def _exec_native(argv: list[str]) -> int:
    """Replace this process with ``bin/entropic argv...``; return on failure."""
    bin_path = _loader.find_bin("entropic")
    if bin_path is None:
        print(
            "error: entropic native binary not found.\n"
            "       run `entropic install-engine` to fetch a release, "
            "or set $ENTROPIC_LIB to point at an in-tree build.",
            file=sys.stderr,
        )
        return 2
    os.execvp(str(bin_path), [str(bin_path), *argv])
    return 1  # unreachable; execvp does not return on success


## @brief One-line summary of the wrapper-only install-engine subcommand.
## @utility
## @version 2.1.1-rc1
_INSTALL_ENGINE_HELP = (
    "  install-engine          Fetch and install librentropic.so from "
    "GitHub Releases\n"
    "                          (handled by the pip wrapper, not the native "
    "binary).\n"
    "                          Usage: entropic install-engine [--version V] "
    "[--backend cpu|cuda]\n"
)


## @brief Console-script entry point. argv excludes the program name.
## @utility
## @version 2.1.1-rc1
def main(argv: list[str] | None = None) -> int:
    """Console-script entry point. argv excludes the program name."""
    args = list(sys.argv[1:] if argv is None else argv)
    if args and args[0] == "install-engine":
        if any(a in ("-h", "--help") for a in args[1:]):
            print(
                "Usage: entropic install-engine [--version V] [--backend cpu|cuda]\n"
                "\n"
                "  Fetch and verify the librentropic.so + entropic CLI tarball\n"
                "  for this wrapper version (or the version specified by\n"
                "  --version) from GitHub Releases, and extract under\n"
                "  ~/.entropic/ (override via $ENTROPIC_HOME).\n"
                "\n"
                "  --version V        Engine version to install (default: this\n"
                "                     wrapper's version).\n"
                "  --backend cpu|cuda Force a backend; default auto-detects\n"
                "                     CUDA via nvidia-smi."
            )
            return 0
        return install_engine.main(args[1:])
    if args and args[0] in ("-h", "--help"):
        # Surface the wrapper-only subcommand alongside the native binary's help.
        print(_INSTALL_ENGINE_HELP)
        sys.stdout.flush()
    return _exec_native(args)


if __name__ == "__main__":
    sys.exit(main())
