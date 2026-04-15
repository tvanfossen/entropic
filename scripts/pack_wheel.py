#!/usr/bin/env python3
"""
Build a PyPI wheel from an entropic release tarball.

The entropic distribution model is a single GitHub Release tarball per
backend (see .github/workflows/release.yml). This script re-packages
one of those tarballs into a Python wheel so `pip install
entropic-engine` pulls the exact same librentropic.so that `install.sh`
would have fetched. One binary, two distribution channels.

Wheel layout:

    entropic/                          Python package (from python/entropic/)
        __init__.py                    ctypes wrapper — loads lib/librentropic.so
        cli.py                         click-based CLI (entry point)
        cli_download.py                model downloader helper
        py.typed
        lib/
            librentropic.so            the native facade (stripped of SOVERSION
                                       symlinks — the wrapper loads this exact
                                       path via os.path.dirname(__file__)/lib/)
        data/                          bundled prompts, grammars, tools, models.yaml
                                       (from the tarball's share/entropic/)
    entropic_engine-<version>.dist-info/
        METADATA                       PEP 621 metadata
        WHEEL                          tag + generator
        RECORD                         sha256 + size of every file
        LICENSE                        LGPL-3.0
        entry_points.txt               entropic = entropic.cli:main

@brief Repackage an entropic release tarball into a PyPI wheel.
@version 2.0.5
"""

import argparse
import base64
import csv
import hashlib
import io
import shutil
import sys
import tarfile
import tempfile
import zipfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent


WHEEL_METADATA = """\
Metadata-Version: 2.1
Name: entropic-engine
Version: {version}
Summary: Local-first agentic inference engine with tier-based model routing
License-Expression: LGPL-3.0-or-later
License-File: LICENSE
Author: Tristan VanFossen
Requires-Python: >=3.10
Classifier: Development Status :: 4 - Beta
Classifier: Environment :: Console
Classifier: Intended Audience :: Developers
Classifier: License :: OSI Approved :: GNU Lesser General Public License v3 (LGPLv3)
Classifier: Programming Language :: C
Classifier: Programming Language :: C++
Classifier: Programming Language :: Python :: 3.10
Classifier: Programming Language :: Python :: 3.11
Classifier: Programming Language :: Python :: 3.12
Classifier: Programming Language :: Python :: 3.13
Classifier: Topic :: Software Development
Requires-Dist: click>=8.0.0

Local-first LLM inference engine with built-in MCP tool support.
Ships bundled librentropic.so + runtime data; no external native
dependencies beyond system libraries (libc, libstdc++, libssl,
libcrypto, libsqlite3, libz, libgomp, libm).

See https://github.com/tvanfossen/entropic for docs and CUDA builds.
"""


WHEEL_FILE = """\
Wheel-Version: 1.0
Generator: entropic-pack-wheel
Root-Is-Purelib: false
Tag: py3-none-{platform_tag}
"""


ENTRY_POINTS = """\
[console_scripts]
entropic = entropic.cli:main
"""


## @brief urlsafe-base64-without-padding sha256 hash (wheel RECORD format).
## @internal
## @return Hash string.
## @version 1
def _sha256_b64(data: bytes) -> str:
    """Return the urlsafe-b64-without-padding sha256 hash (wheel RECORD format)."""
    digest = hashlib.sha256(data).digest()
    return base64.urlsafe_b64encode(digest).rstrip(b"=").decode("ascii")


## @brief Extract the release tarball; return the `entropic/` root inside.
## @internal
## @return Path to the extracted entropic/ directory.
## @version 1
def _extract_tarball(tarball: Path, dest: Path) -> Path:
    """Extract the release tarball and return the entropic/ root inside it."""
    with tarfile.open(tarball, "r:gz") as tf:
        tf.extractall(dest, filter="data")
    root = dest / "entropic"
    if not root.is_dir():
        raise RuntimeError(
            f"{tarball}: expected top-level 'entropic/' directory, " f"got {list(dest.iterdir())}"
        )
    return root


## @brief Copy python/entropic/ into the wheel staging dir as entropic/.
## @internal
## @version 1
def _copy_python_package(stage: Path) -> None:
    """Copy python/entropic/ into the wheel staging dir as entropic/.

    Skips __pycache__. Preserves py.typed marker.
    """
    src = REPO_ROOT / "python" / "entropic"
    dst = stage / "entropic"
    shutil.copytree(
        src,
        dst,
        ignore=shutil.ignore_patterns("__pycache__", "*.pyc", "data"),
    )


## @brief Lay out the native bits from the tarball under entropic/.
## @internal
## @version 1
def _populate_lib_and_data(stage: Path, tarball_root: Path) -> None:
    """Lay out the native bits from the tarball under entropic/.

    - entropic/lib/librentropic.so   ← the real .so.X.Y.Z file (no symlinks)
    - entropic/data/                 ← contents of tarball share/entropic/
    """
    pkg = stage / "entropic"

    # Native library. Find the concrete file (librentropic.so.2.0.5) and copy
    # it to entropic/lib/librentropic.so — the exact path the ctypes wrapper
    # loads via os.path.join(os.path.dirname(__file__), 'lib', ...). The wheel
    # doesn't ship SOVERSION symlinks because they're redundant here: the
    # wheel's own version locks the ABI, and zip/wheel semantics handle
    # symlinks poorly across pip versions.
    lib_src = tarball_root / "lib"
    real_so = None
    for candidate in lib_src.iterdir():
        if candidate.is_file() and candidate.name.startswith("librentropic.so"):
            real_so = candidate
            break
    if real_so is None:
        raise RuntimeError(f"no librentropic.so found in {lib_src}")
    lib_dst = pkg / "lib"
    lib_dst.mkdir(parents=True, exist_ok=True)
    shutil.copy2(real_so, lib_dst / "librentropic.so")

    # Bundled data (prompts, grammars, tools, bundled_models.yaml).
    data_src = tarball_root / "share" / "entropic"
    data_dst = pkg / "data"
    if data_src.is_dir():
        shutil.copytree(data_src, data_dst)
    else:
        raise RuntimeError(f"no share/entropic/ found in {tarball_root}")


## @brief Write the *.dist-info/ metadata directory.
## @internal
## @return Path to the created dist-info directory.
## @version 1
def _write_dist_info(stage: Path, version: str, platform_tag: str) -> Path:
    """Write the *.dist-info/ metadata directory. Returns its path."""
    dist_info = stage / f"entropic_engine-{version}.dist-info"
    dist_info.mkdir()
    (dist_info / "METADATA").write_text(WHEEL_METADATA.format(version=version))
    (dist_info / "WHEEL").write_text(WHEEL_FILE.format(platform_tag=platform_tag))
    (dist_info / "entry_points.txt").write_text(ENTRY_POINTS)

    license_src = REPO_ROOT / "LICENSE"
    if not license_src.is_file():
        raise RuntimeError(f"LICENSE not found at {license_src}")
    shutil.copy2(license_src, dist_info / "LICENSE")

    return dist_info


## @brief Build the wheel RECORD content (PEP 376).
## @internal
## @return Encoded RECORD bytes ready to write.
## @version 1
def _build_record(stage: Path, dist_info: Path) -> bytes:
    """Build the wheel RECORD content. Returns bytes to write to RECORD.

    RECORD format (PEP 376): `<path>,sha256=<hash>,<size>` per file.
    RECORD itself is listed with empty hash + size.
    """
    rows = []
    dist_info_name = dist_info.name
    for path in sorted(stage.rglob("*")):
        if not path.is_file():
            continue
        rel = path.relative_to(stage).as_posix()
        if rel == f"{dist_info_name}/RECORD":
            continue
        data = path.read_bytes()
        rows.append([rel, f"sha256={_sha256_b64(data)}", str(len(data))])
    rows.append([f"{dist_info_name}/RECORD", "", ""])

    buf = io.StringIO()
    writer = csv.writer(buf, lineterminator="\n")
    writer.writerows(rows)
    return buf.getvalue().encode("utf-8")


## @brief Zip the staging dir into the final wheel archive.
## @internal
## @version 1
def _zip_wheel(stage: Path, output: Path) -> None:
    """Zip the staging dir into the final wheel archive."""
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as zf:
        for path in sorted(stage.rglob("*")):
            if not path.is_file():
                continue
            arcname = path.relative_to(stage).as_posix()
            zf.write(path, arcname)


## @brief Build one wheel from a release tarball.
## @utility
## @return Path to the produced .whl file.
## @version 1
def build_wheel(tarball: Path, version: str, platform_tag: str, outdir: Path) -> Path:
    """Assemble a wheel and return its path."""
    outdir.mkdir(parents=True, exist_ok=True)
    wheel_name = f"entropic_engine-{version}-py3-none-{platform_tag}.whl"
    wheel_path = outdir / wheel_name

    with tempfile.TemporaryDirectory(prefix="entropic-wheel-") as tmp:
        tmpd = Path(tmp)
        extract_dir = tmpd / "extract"
        extract_dir.mkdir()
        tarball_root = _extract_tarball(tarball, extract_dir)

        stage = tmpd / "stage"
        stage.mkdir()
        _copy_python_package(stage)
        _populate_lib_and_data(stage, tarball_root)
        dist_info = _write_dist_info(stage, version, platform_tag)

        # RECORD must be written last (after all files exist) then included
        # in the zip itself.
        record_bytes = _build_record(stage, dist_info)
        (dist_info / "RECORD").write_bytes(record_bytes)

        _zip_wheel(stage, wheel_path)

    return wheel_path


## @brief CLI entry point.
## @utility
## @return Process exit code.
## @version 1
def main() -> int:
    """Parse args and build the wheel."""
    ap = argparse.ArgumentParser(
        description="Repackage an entropic release tarball into a PyPI wheel."
    )
    ap.add_argument(
        "--tarball",
        required=True,
        type=Path,
        help="Path to entropic-<ver>-linux-x86_64-<backend>.tar.gz",
    )
    ap.add_argument("--version", required=True, help="Wheel version (e.g. 2.0.5)")
    ap.add_argument(
        "--platform-tag", default="linux_x86_64", help="Wheel platform tag (default: linux_x86_64)"
    )
    ap.add_argument(
        "--outdir",
        default=Path("dist"),
        type=Path,
        help="Output directory for the .whl (default: dist/)",
    )
    args = ap.parse_args()

    if not args.tarball.is_file():
        print(f"tarball not found: {args.tarball}", file=sys.stderr)
        return 1

    wheel = build_wheel(args.tarball, args.version, args.platform_tag, args.outdir)
    print(f"built {wheel}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
