"""
Entropic build & test tasks.

Reference hardware: RTX PRO 4000 Blackwell 16GB, CUDA 13.0/nvcc 12.8, 24 cores.
Default build is CUDA (full preset, includes model tests).
Use --cpu for CPU-only dev builds (no model tests, no GPU).

Usage:
    inv build              # full preset (CUDA + model tests)
    inv build --cpu        # dev preset (CPU debug)
    inv test               # unit + regression (CUDA build)
    inv test --cpu         # unit + regression (CPU, used by pre-commit)
    inv test --model       # + model tests (GPU required, writes results.json)
    inv test --coverage    # coverage preset + gcovr report
    inv clean              # remove build dirs
"""

import json
import os
import re
import shutil
import subprocess
import time
from datetime import datetime, timezone

from invoke import task

JOBS = os.cpu_count() or 4

# Tests excluded from quick runs (slow or require special setup)
CTEST_EXCLUDE = (
    "worktree|ensure_develop|run_git|ScopedWorktree" "|C API (full|delegation|snapshot|search)"
)

MAX_MODEL_RETRIES = 2
MODEL_RESULTS_FILE = "test-reports/model/results.json"


@task(
    help={
        "cpu": "CPU-only debug build (dev preset)",
        "preset": "CMake preset name (overrides --cpu)",
        "jobs": f"Parallel jobs (default: {JOBS})",
        "clean": "Remove build dir before configure",
    }
)
def build(c, cpu=False, preset="", jobs=JOBS, clean=False):
    """Configure and build the project."""
    if not preset:
        preset = "dev" if cpu else "full"

    build_dir = f"build/{preset}"
    if clean and os.path.isdir(build_dir):
        shutil.rmtree(build_dir)

    c.run(f"cmake --preset {preset}")
    c.run(f"cmake --build {build_dir} --parallel {jobs}")


def _get_version():
    """Extract project version from CMakeLists.txt."""
    with open("CMakeLists.txt") as f:
        for line in f:
            m = re.search(r"VERSION\s+(\d+\.\d+\.\d+)", line)
            if m:
                return m.group(1)
    return "unknown"


def _get_git_sha():
    """Get current git HEAD sha."""
    try:
        return (
            subprocess.check_output(
                ["git", "rev-parse", "HEAD"],
                stderr=subprocess.DEVNULL,
            )
            .decode()
            .strip()
        )
    except (subprocess.CalledProcessError, FileNotFoundError):
        return "unknown"


def _get_gpu_name():
    """Get GPU name via nvidia-smi."""
    try:
        return (
            subprocess.check_output(
                ["nvidia-smi", "--query-gpu=name", "--format=csv,noheader"],
                stderr=subprocess.DEVNULL,
            )
            .decode()
            .strip()
            .split("\n")[0]
        )
    except (subprocess.CalledProcessError, FileNotFoundError):
        return "unknown"


def _run_model_tests(build_dir):
    """Run model tests 1:1 with retries. Returns (results, failed_count)."""
    test_dir = os.path.join(build_dir, "tests", "model")
    executables = sorted(
        f
        for f in os.listdir(test_dir)
        if os.path.isfile(os.path.join(test_dir, f))
        and os.access(os.path.join(test_dir, f), os.X_OK)
        and f.startswith("test-")
    )

    if not executables:
        print("ERROR: No model test executables found")
        return [], 1

    results = []
    passed = 0
    failed = 0
    flaky = 0

    for exe in executables:
        exe_path = os.path.join(test_dir, exe)
        retries = 0
        status = "fail"
        t0 = time.monotonic()

        for attempt in range(MAX_MODEL_RETRIES + 1):
            rc = subprocess.call(
                [exe_path],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            if rc == 0:
                status = "pass"
                retries = attempt
                break
            retries = attempt + 1

        duration_ms = int((time.monotonic() - t0) * 1000)

        if status == "pass":
            if retries > 0:
                flaky += 1
                print(f"  FLAKY  {exe} (retry {retries})")
            else:
                print(f"  PASS   {exe}")
            passed += 1
        else:
            print(f"  FAIL   {exe} (after {MAX_MODEL_RETRIES} retries)")
            failed += 1

        results.append(
            {
                "name": exe,
                "status": status,
                "retries": retries,
                "duration_ms": duration_ms,
            }
        )

    print(f"\n{passed}/{len(executables)} passed, " f"{flaky} flaky, {failed} failed")
    return results, failed


def _write_results_json(test_results, duration_ms):
    """Write test-reports/model/results.json."""
    os.makedirs(os.path.dirname(MODEL_RESULTS_FILE), exist_ok=True)

    total = len(test_results)
    passed = sum(1 for t in test_results if t["status"] == "pass")
    flaky_count = sum(1 for t in test_results if t["retries"] > 0)

    data = {
        "schema_version": 1,
        "version": _get_version(),
        "git_sha": _get_git_sha(),
        "timestamp": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "model": "Qwen3.5-35B-A3B-UD-IQ3_XXS",
        "gpu": _get_gpu_name(),
        "duration_ms": duration_ms,
        "tests": test_results,
        "summary": {
            "total": total,
            "passed": passed,
            "failed": total - passed,
            "flaky": flaky_count,
        },
    }

    with open(MODEL_RESULTS_FILE, "w") as f:
        json.dump(data, f, indent=2)
        f.write("\n")

    print(f"Written: {MODEL_RESULTS_FILE}")


@task(
    help={
        "model": "Include model tests (GPU required, writes results.json)",
        "cpu": "CPU-only (dev preset, used by pre-commit hook)",
        "coverage": "Run under coverage preset with gcovr report",
        "preset": "CMake preset (default: auto-selected)",
        "jobs": f"Parallel test jobs (default: {JOBS})",
        "filter": "CTest -R regex filter",
        "no-build": "Skip build step (assumes already built)",
    }
)
def test(  # noqa: CFQ002
    c, model=False, cpu=False, coverage=False, preset="", jobs=JOBS, filter="", no_build=False
):
    """Run tests. Builds first unless --no-build."""
    if not preset:
        if coverage:
            preset = "coverage"
        elif cpu:
            preset = "dev"
        else:
            preset = "full"

    if not no_build:
        build(c, preset=preset, jobs=jobs)

    build_dir = f"build/{preset}"
    ctest_args = f"--output-on-failure --parallel {jobs}"

    if filter:
        ctest_args += f' -R "{filter}"'
    else:
        ctest_args += f' -E "{CTEST_EXCLUDE}"'

    if model:
        # CPU tests first (exclude model label)
        c.run(f"ctest --test-dir {build_dir} {ctest_args} -LE model")

        # Model tests: run 1:1 with retries, write results.json
        print("\n── Model tests (GPU) ──")
        t0 = time.monotonic()
        results, failed = _run_model_tests(build_dir)
        duration_ms = int((time.monotonic() - t0) * 1000)

        if results:
            _write_results_json(results, duration_ms)

        if failed > 0:
            raise SystemExit(1)
    else:
        c.run(f"ctest --test-dir {build_dir} {ctest_args}")

    if coverage:
        c.run(".venv/bin/python scripts/check_coverage.py")


@task
def clean(c):
    """Remove all build directories."""
    for d in ("build/dev", "build/full", "build/coverage", "build/minimal-static", "build/game"):
        if os.path.isdir(d):
            print(f"Removing {d}")
            shutil.rmtree(d)
