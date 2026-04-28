# SPDX-License-Identifier: LGPL-3.0-or-later
"""``entropic install-engine`` — fetch the matching native tarball.

Behaviour:
    1. Detect backend (``cuda`` if ``nvidia-smi`` reports a GPU, else ``cpu``).
    2. Resolve install root: ``$ENTROPIC_HOME`` if set, else ``~/.entropic``.
    3. Download
       ``https://github.com/tvanfossen/entropic/releases/download/v{ver}/
       entropic-{ver}-linux-x86_64-{backend}.tar.gz`` plus ``.sha256`` sidecar.
    4. Verify checksum.
    5. Extract under the install root (tarballs lay out
       ``{root}/entropic/{bin,lib,include,share}``).

Idempotent: if a previous install matches the published checksum, the
download and extraction are skipped.
"""

from __future__ import annotations

import hashlib
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path

from entropic import __version__

RELEASE_URL_TEMPLATE = (
    "https://github.com/tvanfossen/entropic/releases/download/"
    "v{version}/entropic-{version}-linux-x86_64-{backend}.tar.gz"
)


## @brief Return "cuda" if nvidia-smi succeeds, else "cpu".
## @utility
## @version 2.1.0
def _detect_backend() -> str:
    """Return ``"cuda"`` if nvidia-smi succeeds, else ``"cpu"``."""
    try:
        result = subprocess.run(
            ["nvidia-smi", "--query-gpu=name", "--format=csv,noheader"],
            capture_output=True,
            timeout=5,
            check=False,
        )
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return "cpu"
    return "cuda" if result.returncode == 0 and result.stdout.strip() else "cpu"


## @brief Return the resolved install root ($ENTROPIC_HOME or ~/.entropic).
## @utility
## @version 2.1.0
def _install_root() -> Path:
    """Return the resolved install root (``$ENTROPIC_HOME`` or ``~/.entropic``)."""
    home = os.environ.get("ENTROPIC_HOME")
    return Path(home) if home else Path.home() / ".entropic"


## @brief Return the lowercase hex SHA256 of path.
## @utility
## @version 2.1.0
def _sha256_of(path: Path) -> str:
    """Return the lowercase hex SHA256 of ``path``."""
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


## @brief Stream url to dest; raise URLError or HTTPError on failure.
## @utility
## @version 2.1.0
def _download(url: str, dest: Path) -> None:
    """Stream ``url`` to ``dest``; raise URLError or HTTPError on failure."""
    print(f"  downloading {url}", file=sys.stderr)
    with urllib.request.urlopen(url) as response, dest.open("wb") as out:
        shutil.copyfileobj(response, out)


## @brief Fetch the .sha256 sidecar and return the bare hash.
## @utility
## @version 2.1.0
def _expected_sha256(version: str, backend: str, scratch: Path) -> str:
    """Fetch the .sha256 sidecar and return the bare hash."""
    sha_url = RELEASE_URL_TEMPLATE.format(version=version, backend=backend) + ".sha256"
    sha_file = scratch / "tarball.sha256"
    _download(sha_url, sha_file)
    # `sha256sum` output format: "<hash>  <filename>". Take the first token.
    return sha_file.read_text().split()[0].strip()


## @brief If a stamp file matches expected_hash, install is current.
## @utility
## @version 2.1.0
def _verify_existing(root: Path, expected_hash: str) -> bool:
    """If a stamp file matches expected_hash, install is current."""
    stamp = root / ".entropic-engine-sha256"
    if not stamp.is_file():
        return False
    return stamp.read_text().strip() == expected_hash


## @brief Wipe root/entropic then extract tarball into root.
## @utility
## @version 2.1.0
def _extract_to(tarball: Path, root: Path) -> None:
    """Wipe ``root/entropic`` then extract tarball into ``root``."""
    target = root / "entropic"
    if target.exists():
        shutil.rmtree(target)
    root.mkdir(parents=True, exist_ok=True)
    with tarfile.open(tarball, "r:gz") as tf:
        tf.extractall(root, filter="data")


## @brief Install the engine tarball; return process-style exit code.
## @utility
## @version 2.1.0
def install(version: str | None = None, backend: str | None = None) -> int:
    """Install the engine tarball; return process-style exit code."""
    version = version or __version__
    backend = backend or _detect_backend()
    root = _install_root()
    print(
        f"entropic install-engine: version={version} backend={backend} " f"root={root}",
        file=sys.stderr,
    )

    with tempfile.TemporaryDirectory() as tmp_str:
        tmp = Path(tmp_str)
        expected = _expected_sha256(version, backend, tmp)

        if _verify_existing(root, expected):
            print("  already installed (sha256 matches); skipping", file=sys.stderr)
            return 0

        url = RELEASE_URL_TEMPLATE.format(version=version, backend=backend)
        tarball = tmp / "entropic.tar.gz"
        _download(url, tarball)
        actual = _sha256_of(tarball)
        if actual != expected:
            print(
                f"error: sha256 mismatch (expected {expected}, got {actual})",
                file=sys.stderr,
            )
            return 2

        _extract_to(tarball, root)
        (root / ".entropic-engine-sha256").write_text(expected + "\n")
        print(f"  installed to {root / 'entropic'}", file=sys.stderr)
    return 0


## @brief `entropic install-engine` entry point. Optional --version / --backend.
## @utility
## @version 2.1.0
def main(argv: list[str] | None = None) -> int:
    """``entropic install-engine`` entry point. Optional --version / --backend."""
    args = list(sys.argv[1:] if argv is None else argv)
    version: str | None = None
    backend: str | None = None
    while args:
        flag = args.pop(0)
        if flag == "--version" and args:
            version = args.pop(0)
        elif flag == "--backend" and args:
            backend = args.pop(0)
        else:
            print(f"error: unknown argument {flag!r}", file=sys.stderr)
            return 64
    return install(version=version, backend=backend)
