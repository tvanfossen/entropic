#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Per-library coverage enforcement via gcovr.

Pre-commit hook entry point. Builds with coverage instrumentation,
runs tests, then checks per-library line coverage against thresholds.

Thresholds are defined in THRESHOLDS below — this script IS the
enforcement configuration. No external config files.
"""

import glob
import re
import subprocess
import sys
from pathlib import Path

# ── Thresholds (source of truth) ──────────────────────
# {display_name: (source_filter, min_line_coverage_percent)}
THRESHOLDS = {
    "librentropic-types": ("src/types/", 95),
    "librentropic-config": ("src/config/", 90),
    "librentropic-core": ("src/core/", 85),
    "librentropic-mcp": ("src/mcp/", 85),
    "librentropic-storage": ("src/storage/", 85),
    "librentropic-inference": ("src/inference/", 70),
    "librentropic (facade)": ("src/facade/", 80),
}

BUILD_DIR = Path("build/coverage")
REPORT_DIR = Path("build/test-reports/coverage")


## @brief Run a subprocess, exit on failure.
## @utility
## @version 1.10.1
def run(cmd, **kwargs):
    """@brief Run a subprocess, exit on failure.
    @param cmd Command list.
    @version 1.10.1
    """
    result = subprocess.run(cmd, **kwargs)
    if result.returncode != 0:
        sys.exit(result.returncode)


## @brief Configure, build, and run tests with coverage.
## @utility
## @version 1.10.1
def build_and_test():
    """@brief Configure, build, and run tests with coverage.
    @version 1.10.1
    """
    run(["cmake", "--preset", "coverage"])
    run(["cmake", "--build", str(BUILD_DIR), "--parallel"])
    result = subprocess.run(
        [
            "ctest",
            "--output-on-failure",
            "--parallel",
            "4",
            "-E",
            "worktree|ensure_develop|run_git|"
            "ScopedWorktree|C API (full|delegation|snapshot|search)",
        ],
        cwd=str(BUILD_DIR),
    )
    if result.returncode != 0:
        print("WARNING: Some tests failed. Continuing with coverage check.")
        print("         Failed tests do not generate coverage data.")


## @brief Check one library's coverage against its threshold.
## @utility
## @return True if meets threshold or no data, False if below.
## @version 1.10.1
def check_library(name, source_filter, threshold):
    """@brief Check one library's coverage against its threshold.
    @param name Display name.
    @param source_filter Source directory filter for gcovr.
    @param threshold Minimum line coverage percent.
    @return True if meets threshold or no data, False if below.
    @version 1.10.1
    """
    result = subprocess.run(
        [
            "gcovr",
            "-r",
            ".",
            "--object-directory",
            str(BUILD_DIR),
            "--filter",
            source_filter,
            "--print-summary",
        ],
        capture_output=True,
        text=True,
    )

    output = result.stdout + result.stderr
    match = re.search(r"lines:\s+([0-9.]+)%", output)

    if not match:
        print(f"  SKIP  {name} — no coverage data")
        return True

    pct = float(match.group(1))
    if pct < threshold:
        print(f"  FAIL  {name}: {pct:.1f}% < {threshold}%")
        return False

    print(f"  PASS  {name}: {pct:.1f}% >= {threshold}%")
    return True


## @brief Generate HTML coverage report for local inspection.
## @utility
## @version 1.10.1
def generate_report():
    """@brief Generate HTML coverage report for local inspection.
    @version 1.10.1
    """
    REPORT_DIR.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [
            "gcovr",
            "-r",
            ".",
            "--object-directory",
            str(BUILD_DIR),
            "--html-details",
            str(REPORT_DIR / "index.html"),
            "--exclude",
            r"extern/.*",
            "--exclude",
            r"build/.*",
            "--exclude",
            r"tests/.*",
            "--print-summary",
        ],
        capture_output=True,
    )


## @brief Check if .gcda files exist from a prior test run.
## @utility
## @return True if gcov data is present.
## @version 1.10.1
def has_gcov_data():
    """@brief Check if .gcda files exist from a prior test run.
    @return True if gcov data is present.
    @version 1.10.1
    """
    return bool(glob.glob(str(BUILD_DIR / "**/*.gcda"), recursive=True))


## @brief Entry point: build, test, check coverage, report.
## @utility
## @version 1.10.1
def main():
    """@brief Entry point: build, test, check coverage, report.
    @version 1.10.1
    """
    if has_gcov_data():
        print("Coverage data found — skipping build+test.")
    else:
        build_and_test()

    print("Per-library coverage check")
    print("==========================")

    all_pass = True
    for name, (source_filter, threshold) in THRESHOLDS.items():
        if not check_library(name, source_filter, threshold):
            all_pass = False

    print("==========================")

    generate_report()

    if not all_pass:
        print(f"Coverage check FAILED. " f"See {REPORT_DIR}/index.html for details.")
        sys.exit(1)

    print("All libraries meet coverage thresholds.")


if __name__ == "__main__":
    main()
