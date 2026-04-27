# SPDX-License-Identifier: LGPL-3.0-or-later
"""librentropic.so resolution.

Resolution order (first match wins):
    1. ``$ENTROPIC_LIB`` — explicit path to a librentropic.so. Useful for
       development against an in-tree build (``build/dev/lib/...``).
    2. ``$ENTROPIC_HOME/lib/librentropic.so`` — install-engine target when
       ``ENTROPIC_HOME`` is set.
    3. ``~/.entropic/lib/librentropic.so`` — install-engine default.
    4. ``ctypes.util.find_library("rentropic")`` — system ldconfig cache.

If none resolve, ``find()`` returns ``None``. Callers (notably
:mod:`entropic.cli`) translate that into a user-facing
"run ``entropic install-engine``" error.
"""

from __future__ import annotations

import ctypes
import ctypes.util
import os
from pathlib import Path


## @brief Yield candidate librentropic.so paths in resolution order.
## @utility
## @version 2.1.0
def _candidate_paths() -> list[Path]:
    """Yield candidate librentropic.so paths in resolution order."""
    paths: list[Path] = []
    explicit = os.environ.get("ENTROPIC_LIB")
    if explicit:
        paths.append(Path(explicit))
    home = os.environ.get("ENTROPIC_HOME")
    if home:
        paths.append(Path(home) / "lib" / "librentropic.so")
    paths.append(Path.home() / ".entropic" / "lib" / "librentropic.so")
    return paths


## @brief Return the first existing librentropic.so path, or None.
## @utility
## @version 2.1.0
def find() -> Path | None:
    """Return the first existing librentropic.so path, or None."""
    for path in _candidate_paths():
        if path.is_file():
            return path
    cached = ctypes.util.find_library("rentropic")
    if cached:
        return Path(cached)
    return None


## @brief Return the path to the bundled CLI executable, or None.
## @utility
## @version 2.1.0
def find_bin(name: str = "entropic") -> Path | None:
    """Return the path to the bundled CLI executable, or None.

    The install-engine subcommand lays out the tarball under
    ``<root>/{bin,lib,include,share}``; given a known ``lib/librentropic.so``
    we infer ``bin/<name>`` adjacent to it.
    """
    lib = find()
    if lib is None:
        return None
    candidate = lib.parent.parent / "bin" / name
    return candidate if candidate.is_file() else None


## @brief Load librentropic.so, raising OSError if missing.
## @utility
## @version 2.1.0
def load() -> ctypes.CDLL:
    """Load librentropic.so, raising :class:`OSError` if missing."""
    lib = find()
    if lib is None:
        raise OSError(
            "librentropic.so not found. "
            "Run `entropic install-engine` to download a release, "
            "or set $ENTROPIC_LIB to point at a custom build."
        )
    return ctypes.CDLL(str(lib))
