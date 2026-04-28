# SPDX-License-Identifier: LGPL-3.0-or-later
"""librentropic.so resolution.

The release tarball extracts as ``<root>/entropic/{bin,lib,include,share}``
(GitHub release layout) or as ``<root>/{bin,lib,...}`` when manually
extracted with ``tar --strip-components=1`` per docs/dist-README.md.
Both layouts are searched.

Resolution order (first match wins):
    1. ``$ENTROPIC_LIB`` — explicit path to a librentropic.so. Useful for
       development against an in-tree build (``build/dev/lib/...``).
    2. ``$ENTROPIC_HOME/entropic/lib/librentropic.so`` (nested form,
       what ``entropic install-engine`` produces) when ``ENTROPIC_HOME``
       is set.
    3. ``$ENTROPIC_HOME/lib/librentropic.so`` (flat form, what manual
       tarball extraction with ``--strip-components=1`` produces).
    4. ``~/.entropic/entropic/lib/librentropic.so`` — install-engine
       default extraction path.
    5. ``~/.entropic/lib/librentropic.so`` — flat-form fallback.
    6. ``ctypes.util.find_library("rentropic")`` — system ldconfig cache.

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
## @version 2.1.1-rc1
def _candidate_paths() -> list[Path]:
    """Yield candidate librentropic.so paths in resolution order.

    Each install root contributes TWO candidates: nested
    (``<root>/entropic/lib/...`` — install-engine layout) and flat
    (``<root>/lib/...`` — strip-components=1 layout). Nested checked
    first since that's what install-engine actually produces.
    """
    paths: list[Path] = []
    explicit = os.environ.get("ENTROPIC_LIB")
    if explicit:
        paths.append(Path(explicit))
    home = os.environ.get("ENTROPIC_HOME")
    if home:
        paths.append(Path(home) / "entropic" / "lib" / "librentropic.so")
        paths.append(Path(home) / "lib" / "librentropic.so")
    default_root = Path.home() / ".entropic"
    paths.append(default_root / "entropic" / "lib" / "librentropic.so")
    paths.append(default_root / "lib" / "librentropic.so")
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
