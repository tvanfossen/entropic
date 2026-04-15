# SPDX-License-Identifier: LGPL-3.0-or-later
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


## @brief Configure and build the project via CMake preset.
## @utility
## @version 1
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


## @brief Extract project version from CMakeLists.txt.
## @utility
## @return Version string or "unknown".
## @version 1
def _get_version():
    """Extract project version from CMakeLists.txt."""
    with open("CMakeLists.txt") as f:
        for line in f:
            m = re.search(r"VERSION\s+(\d+\.\d+\.\d+)", line)
            if m:
                return m.group(1)
    return "unknown"


## @brief Get current git HEAD sha.
## @utility
## @return Hex SHA string or "unknown".
## @version 1
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


## @brief Get GPU name via nvidia-smi.
## @utility
## @return GPU name string or "unknown".
## @version 1
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


## @brief Run model tests 1:1 with retries.
## @utility
## @return Tuple of (results list, failed count).
## @version 1
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


## @brief Write test-reports/model/results.json.
## @utility
## @version 1
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


## @brief Run tests. Builds first unless --no-build.
## @utility
## @version 1
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


EXAMPLES = {
    "hello-world": {"lang": "c", "binary": "hello-world"},
    "pychess": {"lang": "cxx", "binary": "pychess"},
    "explorer": {"lang": "cxx", "binary": "explorer"},
    "headless": {"lang": "c", "binary": "headless"},
}

# v2.0.5: the distribution is a single librentropic.so — sublibs (types,
# core, config, prompts, inference, mcp, storage) are OBJECT libraries
# absorbed into the facade, and third-party deps (spdlog, ryml, llama,
# etc.) are absorbed statically. So only one lib dir matters at
# runtime: src/facade/. Pre-v2.0.5 code listed eight dirs here to
# cover the old per-sublib .so layout; all of that is now stale.
FACADE_LIB_SUBDIR = os.path.join("src", "facade")


## @brief Return absolute path to librentropic.so in the build tree.
## @utility
## @return Absolute path string.
## @version 2
def _lib_path(preset):
    """Return absolute path to librentropic.so in the build tree."""
    return os.path.abspath(os.path.join("build", preset, FACADE_LIB_SUBDIR, "librentropic.so"))


## @brief Directory containing the built librentropic.so for a given preset.
## @utility
## @return Absolute directory path.
## @version 2
def _facade_dir(preset):
    """Absolute path to the facade output directory (src/facade/)."""
    return os.path.abspath(os.path.join("build", preset, FACADE_LIB_SUBDIR))


## @brief Single-directory LD_LIBRARY_PATH for the in-tree build.
## @utility
## @return Directory path string.
## @version 2
def _ld_library_path(preset):
    """Build LD_LIBRARY_PATH — only the facade dir is needed post-v2.0.5."""
    return _facade_dir(preset)


## @brief Configure and build a C/C++ example against the engine build tree.
## @utility
## @version 2
def _build_c_example(c, name, preset, jobs):
    """Configure and build a C/C++ example against the engine build tree."""
    example_dir = os.path.join("examples", name)
    build_dir = os.path.join(example_dir, "build")
    include_dir = os.path.abspath("include")
    build_include_dir = os.path.abspath(os.path.join("build", preset, "include"))
    lib_dir = _facade_dir(preset)

    c.run(
        f"cmake -B {build_dir}"
        f" -DENTROPIC_LIB_DIR={lib_dir}"
        f" -DENTROPIC_INCLUDE_DIR={include_dir}"
        f" -DENTROPIC_BUILD_INCLUDE_DIR={build_include_dir}"
        f" {example_dir}"
    )
    c.run(f"cmake --build {build_dir} --parallel {jobs}")


## @brief Build and run an example. Builds engine first if needed.
## @utility
## @version 1
@task(
    help={
        "name": "Example name: hello-world, pychess",
        "wrapper": "Run Python wrapper version instead of C/C++",
        "cpu": "Use CPU dev build instead of full CUDA",
        "preset": "CMake preset (overrides --cpu)",
        "jobs": f"Parallel build jobs (default: {JOBS})",
    }
)
def example(c, name, wrapper=False, cpu=False, preset="", jobs=JOBS):
    """Build and run an example. Builds engine first if needed."""
    if name not in EXAMPLES:
        names = ", ".join(EXAMPLES)
        raise SystemExit(f"Unknown example '{name}'. Available: {names}")

    if not preset:
        preset = "dev" if cpu else "full"

    lib_so = _lib_path(preset)
    if not os.path.isfile(lib_so):
        print(f"Engine not built ({lib_so}). Building...")
        build(c, cpu=cpu, preset=preset, jobs=jobs)

    example_dir = os.path.abspath(os.path.join("examples", name))
    env = {**os.environ, "LD_LIBRARY_PATH": _ld_library_path(preset)}

    if wrapper:
        _run_wrapper(c, name, example_dir, lib_so, env)
    else:
        _run_native(c, name, example_dir, preset, jobs, env)


## @brief Run the Python wrapper version of an example.
## @utility
## @version 1
def _run_wrapper(c, name, example_dir, lib_so, env):
    """Run the Python wrapper version of an example."""
    script = os.path.join(example_dir, "main_wrapper.py")
    if not os.path.isfile(script):
        raise SystemExit(f"No wrapper script: {script}")

    env["ENTROPIC_LIB_PATH"] = lib_so
    python = os.path.abspath(".venv/bin/python")
    print(f"Running {name} (Python wrapper)...")
    c.run(f"cd {example_dir} && {python} main_wrapper.py", env=env, pty=True)


## @brief Build and run the C/C++ version of an example.
## @utility
## @version 1
def _run_native(c, name, example_dir, preset, jobs, env):
    """Build and run the C/C++ version of an example."""
    _build_c_example(c, name, preset, jobs)
    binary = os.path.abspath(os.path.join(example_dir, "build", EXAMPLES[name]["binary"]))
    if not os.path.isfile(binary):
        raise SystemExit(f"Build succeeded but binary not found: {binary}")

    print(f"Running {name} (C/C++)...")
    c.run(f"cd {example_dir} && {binary}", env=env, pty=True)


## @brief Generate doxygen SQLite knowledge database from engine source.
## @utility
## @version 2
@task(
    help={
        "enrich": "Also populate architecture_topics from YAML",
        "verbose": "Enable verbose logging",
    }
)
def docs(c, enrich=False, verbose=False):
    """Generate doxygen SQLite database (docs/entropic_docs.db)."""
    python = os.path.abspath(".venv/bin/python")
    script = os.path.abspath("examples/explorer/scripts/build_docs_db.py")
    doxyfile = os.path.abspath("docs/Doxyfile")
    output = os.path.abspath("docs/entropic_docs.db")
    enrich_yaml = os.path.abspath("examples/explorer/data/architecture_topics.yaml")

    cmd = f"{python} {script} --doxyfile {doxyfile} --output {output}"
    if enrich and os.path.isfile(enrich_yaml):
        cmd += f" --enrich {enrich_yaml}"
    if verbose:
        cmd += " --verbose"
    c.run(cmd)


## @brief Remove all build directories.
## @utility
## @version 1
@task
def clean(c):
    """Remove all build directories."""
    dirs = [
        "build/dev",
        "build/full",
        "build/coverage",
        "build/minimal-static",
        "build/game",
    ]
    # Also clean example build dirs
    for name in EXAMPLES:
        dirs.append(os.path.join("examples", name, "build"))

    for d in dirs:
        if os.path.isdir(d):
            print(f"Removing {d}")
            shutil.rmtree(d)
