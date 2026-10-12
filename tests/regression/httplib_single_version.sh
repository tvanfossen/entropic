#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# v2.14.0 (gh#204): exactly ONE cpp-httplib in librentropic.
#
# entropic uses cpp-httplib 0.18.3, header-only (FetchContent). Vendored
# llama.cpp also builds cpp-httplib 0.56.0 as a static archive, linked into
# llama-common for its model-download code, which entropic absorbs. Two
# versions of one library under one namespace in one .so is an ODR
# violation waiting for a reference to pull the second in.
#
# Today nothing does: 0 of the archive's strong (T) definitions are in our
# library — all httplib code we ship is 0.18.3's weak inline instantiations.
# v2.14.0 builds the engine's HTTP server on 0.18.3 because of that measured
# fact; this test keeps it a fact. If it fails, something now references
# llama-common's download path (or similar) and the two versions collide —
# settle that before shipping, do not relax this test.
#
# Usage: httplib_single_version.sh <librentropic.so> <libcpp-httplib.a>
set -euo pipefail
so="$1"
archive="$2"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
nm --defined-only "$archive" 2>/dev/null | awk '$2=="T"{print $3}' | sort -u > "$tmp/archive"
nm --defined-only "$so" | awk '$2=="T"{print $3}' | sort -u > "$tmp/so"
if [ ! -s "$tmp/archive" ]; then
    echo "FAIL: no strong definitions read from $archive — wrong path?" >&2
    exit 1
fi
overlap="$(comm -12 "$tmp/archive" "$tmp/so" | wc -l)"
echo "vendored httplib strong defs: $(wc -l < "$tmp/archive"); linked into our library: $overlap"
if [ "$overlap" -ne 0 ]; then
    echo "FAIL: vendored cpp-httplib code is linked into $so:" >&2
    comm -12 "$tmp/archive" "$tmp/so" | head -5 | c++filt >&2
    exit 1
fi
