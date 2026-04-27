# SPDX-License-Identifier: LGPL-3.0-or-later
"""``entropic`` console script — dispatches to the bundled native binary.

Two paths:
    * ``entropic install-engine [--version V] [--backend cpu|cuda]`` —
      handled in-process by :mod:`entropic.install_engine`.
    * Anything else (``entropic ask "..."``, ``entropic version``, …) —
      ``execvp``'d to the ``bin/entropic`` binary that ``install-engine``
      laid down. The Python process is replaced; the native binary
      receives stdin/stdout/stderr verbatim.

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


## @brief Console-script entry point. argv excludes the program name.
## @utility
## @version 2.1.0
def main(argv: list[str] | None = None) -> int:
    """Console-script entry point. argv excludes the program name."""
    args = list(sys.argv[1:] if argv is None else argv)
    if args and args[0] == "install-engine":
        return install_engine.main(args[1:])
    return _exec_native(args)


if __name__ == "__main__":
    sys.exit(main())
