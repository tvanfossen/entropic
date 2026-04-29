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
    inv test --model       # + model tests (GPU recommended, writes results.json)
    inv test --coverage    # coverage preset + gcovr report
    inv clean              # remove build dirs
"""

import glob
import json
import os
import re
import shutil
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

from invoke import task

JOBS = os.cpu_count() or 4

# Tests excluded from quick runs (slow or require special setup)
CTEST_EXCLUDE = (
    "worktree|ensure_develop|run_git|ScopedWorktree" "|C API (full|delegation|snapshot|search)"
)

MAX_MODEL_RETRIES = 2
MODEL_RESULTS_FILE = "build/test-reports/model/results.json"

# Per-library coverage paths (used by the check-coverage gate).
COVERAGE_BUILD_DIR = Path("build/coverage")
COVERAGE_REPORT_DIR = Path("build/test-reports/coverage")
# gcovr lives next to the python running invoke (i.e. inside .venv/bin
# when invoke is launched from the venv). Resolve it explicitly so the
# pre-commit hook works regardless of the caller's PATH.
GCOVR_BIN = str(Path(sys.executable).parent / "gcovr")


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


## @brief Write build/test-reports/model/results.json.
## @utility
## @version 2
def _write_results_json(test_results, duration_ms):
    """Write build/test-reports/model/results.json."""
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
        "model": "Include model tests (GPU recommended, writes results.json)",
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


## @brief Discover example directories under examples/.
## @utility
## @return Sorted list of example directory names that have a CMakeLists.txt.
## @version 1
def _discover_examples():
    """Discover example directories under examples/."""
    root = os.path.abspath("examples")
    if not os.path.isdir(root):
        return []
    return sorted(
        entry
        for entry in os.listdir(root)
        if os.path.isdir(os.path.join(root, entry))
        and os.path.isfile(os.path.join(root, entry, "CMakeLists.txt"))
    )


## @brief Find the built executable inside an example's build directory.
## @utility
## @return Absolute path to the executable, or None if not found.
## @version 1
def _find_example_binary(example_dir):
    """Find the built binary in <example_dir>/build/."""
    build_dir = os.path.join(example_dir, "build")
    if not os.path.isdir(build_dir):
        return None
    for entry in sorted(os.listdir(build_dir)):
        full = os.path.join(build_dir, entry)
        if os.path.isfile(full) and os.access(full, os.X_OK):
            return full
    return None


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


## @brief Build (and optionally run) an example discovered under examples/.
## @utility
## @version 2
@task(
    help={
        "name": "Example name (omit to list discovered examples)",
        "build_only": "Build but do not run (useful for server-style examples)",
        "cpu": "Use CPU dev build instead of full CUDA",
        "preset": "CMake preset (overrides --cpu)",
        "jobs": f"Parallel build jobs (default: {JOBS})",
    }
)
def example(c, name="", build_only=False, cpu=False, preset="", jobs=JOBS):
    """Build (and optionally run) an example. Builds engine first if needed."""
    discovered = _discover_examples()
    if not name:
        if not discovered:
            raise SystemExit("No examples found under examples/")
        print("Available examples: " + ", ".join(discovered))
        return

    if name not in discovered:
        raise SystemExit(f"Unknown example '{name}'. Available: {', '.join(discovered)}")

    if not preset:
        preset = "dev" if cpu else "full"

    lib_so = _lib_path(preset)
    if not os.path.isfile(lib_so):
        print(f"Engine not built ({lib_so}). Building...")
        build(c, cpu=cpu, preset=preset, jobs=jobs)

    example_dir = os.path.abspath(os.path.join("examples", name))
    _build_c_example(c, name, preset, jobs)

    binary = _find_example_binary(example_dir)
    if not binary:
        raise SystemExit(f"Build succeeded but no binary found in {example_dir}/build/")

    if build_only:
        print(f"Built: {binary}")
        return

    env = {**os.environ, "LD_LIBRARY_PATH": _ld_library_path(preset)}
    print(f"Running {name}...")
    c.run(f"cd {example_dir} && {binary}", env=env, pty=True)


## @brief Remove all build directories.
## @utility
## @version 2
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
    # Also clean example build dirs (discovered dynamically)
    for name in _discover_examples():
        dirs.append(os.path.join("examples", name, "build"))

    for d in dirs:
        if os.path.isdir(d):
            print(f"Removing {d}")
            shutil.rmtree(d)


## @brief Extract project VERSION from CMakeLists.txt (X.Y.Z triple).
## @utility
## @return Version string (e.g. "2.0.5").
## @version 1
def _cmake_project_version():
    """Return the X.Y.Z from project(... VERSION <triple> ...)."""
    with open("CMakeLists.txt") as f:
        for line in f:
            m = re.search(r"VERSION\s+(\d+\.\d+\.\d+)", line)
            if m and "cmake_minimum_required" not in line:
                return m.group(1)
    raise SystemExit("Could not extract project VERSION from CMakeLists.txt")


## @brief Configure + build + install one release backend to a scratch prefix.
## @utility
## @version 2
def _build_and_stage(c, backend, build_dir, stage_dir, jobs):
    """Run cmake configure → build → install for one backend."""
    cuda_flag = "-DENTROPIC_CUDA=ON" if backend == "cuda" else "-DENTROPIC_CUDA=OFF"
    cpu_flag = "-DENTROPIC_CPU_ONLY=ON" if backend == "cpu" else "-DENTROPIC_CPU_ONLY=OFF"
    extra = ""
    if backend == "cuda":
        # Match release.yaml's comprehensive arch list so local pre-flight
        # mirrors what CI ships. Maxwell through Blackwell; requires
        # CUDA toolkit >= 12.8 for sm_100/sm_120.
        extra = ' "-DCMAKE_CUDA_ARCHITECTURES=50;52;60;61;70;75;80;86;89;90;100;120"'

    c.run(
        f"cmake -B {build_dir} -S ."
        f" {cuda_flag} {cpu_flag}"
        f" -DENTROPIC_SHARED=ON -DENTROPIC_STATIC=OFF"
        f" -DENTROPIC_BUILD_TESTS=OFF"
        f" -DCMAKE_BUILD_TYPE=Release"
        f" -DCMAKE_INSTALL_PREFIX={stage_dir}"
        f"{extra}"
    )
    c.run(f"cmake --build {build_dir} --parallel {jobs}")
    c.run(f"cmake --install {build_dir}")


## @brief Validate a staged install via find_package + CLI launch.
## @utility
## @version 2
def _smoke_staged_install(c, stage_dir, build_dir):
    """Run tests/distribution-smoke-consumer + bin/entropic version against stage."""
    consumer_build = os.path.join(build_dir, "consumer")
    shutil.rmtree(consumer_build, ignore_errors=True)
    c.run(
        f"cmake -B {consumer_build} -S tests/distribution-smoke-consumer"
        f" -Dentropic_DIR={stage_dir}/lib/cmake/entropic"
    )
    c.run(f"cmake --build {consumer_build}")
    c.run(f"{consumer_build}/entropic-smoke")
    c.run(f"{stage_dir}/bin/entropic version")


## @brief Verify installed .so has no unresolved / surprise runtime deps.
## @utility
## @version 2
def _check_linkage(stage_dir, backend):
    """Inspect ldd output for the installed facade and CLI.

    Fails if librentropic.so has `not found` entries, or if CLI
    doesn't resolve librentropic.so via the $ORIGIN/../lib RPATH.
    """
    lib = os.path.join(stage_dir, "lib", f"librentropic.so.{_cmake_project_version()}")
    bin_entropic = os.path.join(stage_dir, "bin", "entropic")

    lib_ldd = subprocess.check_output(["ldd", lib], text=True)
    if "not found" in lib_ldd:
        print(lib_ldd)
        raise SystemExit(f"{lib}: unresolved runtime dependency")

    bin_ldd = subprocess.check_output(["ldd", bin_entropic], text=True)
    if "not found" in bin_ldd:
        print(bin_ldd)
        raise SystemExit(f"{bin_entropic}: unresolved runtime dependency")
    # CLI must resolve librentropic via the bin-relative path (install RPATH),
    # not by finding it in the system loader cache.
    if f"{stage_dir}/bin/../lib/librentropic.so" not in bin_ldd:
        print(bin_ldd)
        raise SystemExit(
            f"{bin_entropic}: install RPATH not resolving — expected "
            f"librentropic.so.2 via {stage_dir}/bin/../lib/"
        )

    # CUDA tarball: confirm libcudart is actually linked. Not an error
    # if missing — just flag prominently.
    if backend == "cuda" and "libcudart" not in lib_ldd:
        print(
            f"WARNING: {lib} is CUDA backend but libcudart not in ldd output "
            f"— build may not have included CUDA code."
        )


## @brief Pack a staged install into a release-format tar.gz + sha256.
## @utility
## @return Path to the produced .tar.gz.
## @version 1
def _pack_tarball(c, stage_dir, version, backend, outdir):
    """Tar the stage as entropic-<version>-linux-x86_64-<backend>.tar.gz."""
    artifact = f"entropic-{version}-linux-x86_64-{backend}.tar.gz"
    artifact_path = os.path.join(outdir, artifact)
    stage_parent = os.path.dirname(stage_dir)
    stage_name = os.path.basename(stage_dir)
    c.run(f"tar -C {stage_parent} -czf {artifact_path} {stage_name}")
    c.run(f"cd {outdir} && sha256sum {artifact} > {artifact}.sha256")
    return artifact_path


## @brief Pre-flight the tag-driven release workflow locally (no publish).
## @utility
## @version 2
@task(
    help={
        "version": "Artifact version string (default: CMakeLists project VERSION)",
        "skip_cuda": "Skip the CUDA matrix entry (faster iteration)",
        "outdir": "Output directory for tarballs (default: dist/)",
        "jobs": f"Parallel build jobs (default: {JOBS})",
    }
)
def release_check(c, version="", skip_cuda=False, outdir="dist", jobs=JOBS):
    """Pre-flight the tag-driven release workflow locally.

    Mirrors .github/workflows/release.yaml's build-package jobs on the
    local machine — no tag push, no publish. Confidence gate for "the
    release will succeed" before burning hosted-runner minutes on a
    paid tier or cutting a real tag.

    Produces .tar.gz per backend and verifies:
      - CMake install tree is self-contained (no unresolved deps)
      - CLI launches via install RPATH (no LD_LIBRARY_PATH needed)
      - find_package(entropic 2.0 REQUIRED) consumer build+links+runs
      - CUDA tarball has libcudart linked (Blackwell native SASS present)
    """
    if not version:
        version = _cmake_project_version()
    os.makedirs(outdir, exist_ok=True)

    backends = ["cpu"] if skip_cuda else ["cpu", "cuda"]
    tarballs = {}

    for backend in backends:
        print(f"\n══ Release check: {backend} backend ══")
        build_dir = os.path.abspath(os.path.join("build", f"release-{backend}"))
        stage_dir = os.path.abspath(os.path.join(build_dir, "stage", "entropic"))
        shutil.rmtree(build_dir, ignore_errors=True)

        _build_and_stage(c, backend, build_dir, stage_dir, jobs)
        _smoke_staged_install(c, stage_dir, build_dir)
        _check_linkage(stage_dir, backend)
        tarballs[backend] = _pack_tarball(c, stage_dir, version, backend, outdir)
        print(f"  ✓ {tarballs[backend]}")

    print("\n══ Release pre-flight: PASSED ══")
    for backend, path in tarballs.items():
        size = os.path.getsize(path) / (1024 * 1024)
        print(f"  {backend:<5} {path}  ({size:.1f} MB)")
    print(f"\nVersion: {version}")
    print(f"Next: git tag v{version} && git push origin v{version}")


## @brief Parse a coverage threshold spec into a tuple.
## @utility
## @version 1
def _parse_coverage_threshold(spec):
    """Parse a "name:source_filter:percent" spec into (name, filter, int)."""
    name, filt, pct = spec.rsplit(":", 2)
    return name, filt, int(pct)


## @brief True if .gcda files exist from a prior coverage run.
## @utility
## @version 1
def _has_gcov_data():
    """True if .gcda files exist from a prior coverage run."""
    pattern = str(COVERAGE_BUILD_DIR / "**" / "*.gcda")
    return bool(glob.glob(pattern, recursive=True))


## @brief Configure, build, and run tests under the coverage preset.
## @utility
## @version 2
def _run_coverage_build(c):
    """Configure + build + run tests under the coverage preset.

    Build parallelism is capped at 2 because the coverage preset is
    debug + gcov-instrumented: each compile peaks ~1.5 GB and the
    librentropic.so link step (debug + coverage symbols + statically
    absorbed llama.cpp) peaks 4-8 GB. Unlimited --parallel on a
    16 GB hosted runner OOM-kills the linker (exit 143 / SIGTERM)
    when 4 compiles are mid-flight as the link step starts. Local
    builds with 24+ cores and 64+ GB are unaffected; the cap matters
    only for CI.
    """
    c.run("cmake --preset coverage")
    c.run(f"cmake --build {COVERAGE_BUILD_DIR} --parallel 2")
    # ctest exit code is not fatal here — failed tests still produce
    # gcov data for the libraries that did run, and the gate is
    # coverage, not pass/fail.
    c.run(
        f"ctest --test-dir {COVERAGE_BUILD_DIR} --output-on-failure "
        f'--parallel 4 -E "{CTEST_EXCLUDE}"',
        warn=True,
    )


## @brief gcovr one source filter; print PASS/FAIL/SKIP; return ok bool.
## @utility
## @version 1
def _check_library_coverage(name, source_filter, pct_min):
    """gcovr one filter; print PASS/FAIL/SKIP; return ok bool."""
    out = subprocess.run(
        [
            GCOVR_BIN,
            "-r",
            ".",
            "--object-directory",
            str(COVERAGE_BUILD_DIR),
            "--filter",
            source_filter,
            "--print-summary",
        ],
        capture_output=True,
        text=True,
    ).stdout
    m = re.search(r"lines:\s+([0-9.]+)%", out)
    if m is None:
        print(f"  SKIP  {name} — no coverage data")
        return True
    pct = float(m.group(1))
    status = "PASS" if pct >= pct_min else "FAIL"
    print(f"  {status}  {name}: {pct:.1f}% vs {pct_min}%")
    return pct >= pct_min


## @brief Generate the gcovr HTML report under build/test-reports/coverage.
## @utility
## @version 1
def _generate_coverage_report():
    """Generate the gcovr HTML report under build/test-reports/coverage."""
    COVERAGE_REPORT_DIR.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [
            GCOVR_BIN,
            "-r",
            ".",
            "--object-directory",
            str(COVERAGE_BUILD_DIR),
            "--html-details",
            str(COVERAGE_REPORT_DIR / "index.html"),
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


## @brief Per-library coverage gate via gcovr. Pre-commit entry.
## @utility
## @version 1
@task(iterable=["threshold"])
def check_coverage(c, threshold):
    """Per-library coverage gate via gcovr.

    Each --threshold is "name:source_filter:percent". Thresholds live
    in .pre-commit-config.yaml so the coverage gate config sits next
    to the rest of pre-commit's hook config. If .gcda files are
    already present from a prior coverage run, build+test is skipped.
    """
    if not _has_gcov_data():
        _run_coverage_build(c)
    print("Per-library coverage check")
    print("==========================")
    parsed = [_parse_coverage_threshold(t) for t in threshold]
    all_pass = all(_check_library_coverage(*p) for p in parsed)
    print("==========================")
    _generate_coverage_report()
    if not all_pass:
        print(f"Coverage FAILED. See {COVERAGE_REPORT_DIR}/index.html")
        sys.exit(1)
    print("All libraries meet coverage thresholds.")


## @brief Distribution smoke — build + install + find_package consumer.
## @utility
## @version 1
@task(
    help={
        "prefix": "Install prefix for the staged tree (default: /tmp/entropic-smoke)",
        "build-dir": "Build directory for the smoke configure (default: build/smoke)",
        "jobs": f"Parallel build jobs (default: {JOBS})",
    }
)
def smoke(c, prefix="/tmp/entropic-smoke", build_dir="build/smoke", jobs=JOBS):
    """Distribution smoke: shared CPU build → install → find_package + CLI.

    Lightweight cousin of `inv release-check` that exercises just the
    consumer-experience bits: cmake configure with shared/CPU-only,
    install to a temp prefix, build tests/distribution-smoke-consumer
    against that install via find_package(entropic), run the consumer,
    then launch the installed `entropic version` to validate RPATH.
    """
    shutil.rmtree(prefix, ignore_errors=True)
    shutil.rmtree(build_dir, ignore_errors=True)
    c.run(
        f"cmake -B {build_dir} -S ."
        f" -DENTROPIC_SHARED=ON -DENTROPIC_STATIC=OFF"
        f" -DENTROPIC_CPU_ONLY=ON -DENTROPIC_BUILD_TESTS=OFF"
        f" -DCMAKE_BUILD_TYPE=Release"
        f" -DCMAKE_INSTALL_PREFIX={prefix}"
    )
    c.run(f"cmake --build {build_dir} --parallel {jobs}")
    c.run(f"cmake --install {build_dir}")
    _smoke_staged_install(c, prefix, build_dir)
    print(f"OK — distribution smoke passed (prefix={prefix})")


# C → ctypes type map for the bindings codegen.
_BINDINGS_TYPE_MAP = {
    "void": "None",
    "int": "ctypes.c_int",
    "size_t": "ctypes.c_size_t",
    "const char*": "ctypes.c_char_p",
    "char*": "ctypes.c_char_p",
    "char**": "ctypes.POINTER(ctypes.c_char_p)",
    "void*": "ctypes.c_void_p",
    "int*": "ctypes.POINTER(ctypes.c_int)",
    "size_t*": "ctypes.POINTER(ctypes.c_size_t)",
    "entropic_handle_t": "entropic_handle_t",
    "entropic_handle_t*": "ctypes.POINTER(entropic_handle_t)",
    "entropic_error_t": "ctypes.c_int",
}

# Functions whose signatures contain inline function-pointer types or
# struct-by-value parameters cannot be auto-bound. Listed by name and
# emitted as a TODO comment so a human can hand-wire them.
_BINDINGS_INLINE_CB = frozenset(
    {
        "entropic_run_streaming",
        "entropic_set_state_observer",
        "entropic_set_stream_observer",
        "entropic_register_hook",
        "entropic_deregister_hook",
    }
)

_BINDINGS_DECL_RE = re.compile(
    r"ENTROPIC_EXPORT\s+([\w\s\*]+?)\s+(entropic_\w+)\s*\(([^)]*)\)\s*;",
    re.MULTILINE | re.DOTALL,
)

_BINDINGS_PROLOGUE = '''# SPDX-License-Identifier: LGPL-3.0-or-later
# AUTO-GENERATED by `inv gen-bindings` — do not edit.
"""ctypes bindings for librentropic.so (auto-generated)."""
from __future__ import annotations

import ctypes
import enum

from entropic._loader import load

_lib = load()
entropic_handle_t = ctypes.c_void_p


class EntropicError(enum.IntEnum):
    # Mirrors entropic_error_t in include/entropic/types/error.h.
    # Verified by tests/unit/test_bindings_abi.py against the parsed header.
    OK = 0
    INVALID_ARGUMENT = 1
    INVALID_CONFIG = 2
    INVALID_STATE = 3
    MODEL_NOT_FOUND = 4
    LOAD_FAILED = 5
    GENERATE_FAILED = 6
    TOOL_NOT_FOUND = 7
    PERMISSION_DENIED = 8
    PLUGIN_VERSION_MISMATCH = 9
    PLUGIN_LOAD_FAILED = 10
    TIMEOUT = 11
    CANCELLED = 12
    OUT_OF_MEMORY = 13
    IO = 14
    INTERNAL = 15
    SERVER_ALREADY_EXISTS = 16
    SERVER_NOT_FOUND = 17
    CONNECTION_FAILED = 18
    INVALID_HANDLE = 19
    TOOL_EXECUTION_FAILED = 20
    STORAGE_FAILED = 21
    IDENTITY_NOT_FOUND = 22
    ALREADY_RUNNING = 23
    NOT_RUNNING = 24
    NOT_IMPLEMENTED = 25
    INTERRUPTED = 26
    ADAPTER_NOT_FOUND = 27
    ADAPTER_LOAD_FAILED = 28
    ADAPTER_SWAP_FAILED = 29
    ADAPTER_CANCELLED = 30
    GRAMMAR_NOT_FOUND = 31
    GRAMMAR_INVALID = 32
    MCP_KEY_DENIED = 33
    LIMIT_REACHED = 34
    ALREADY_EXISTS = 35
    IN_USE = 36
    PROFILE_NOT_FOUND = 37
    TIME_LIMIT_EXCEEDED = 38
    VALIDATION_FAILED = 39
    COMPACTION_FAILED = 40
    MODEL_NOT_ACTIVE = 41
    EVAL_CONTEXT_FULL = 42
    EVAL_FAILED = 43
    IMAGE_LOAD_FAILED = 44
    IMAGE_TOO_LARGE = 45
    MMPROJ_LOAD_FAILED = 46
    UNSUPPORTED_URL = 47
    NOT_SUPPORTED = 48
    STATE_INCOMPATIBLE = 49


class AgentState(enum.IntEnum):
    # Mirrors entropic_agent_state_t in include/entropic/types/enums.h.
    # Verified by tests/unit/test_bindings_abi.py against the parsed header.
    IDLE = 0
    PLANNING = 1
    EXECUTING = 2
    WAITING_TOOL = 3
    VERIFYING = 4
    DELEGATING = 5
    COMPLETE = 6
    ERROR = 7
    INTERRUPTED = 8
    PAUSED = 9


TOKEN_CB = ctypes.CFUNCTYPE(
    None, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p
)
STATE_OBSERVER_CB = ctypes.CFUNCTYPE(None, ctypes.c_int, ctypes.c_void_p)


def _bind(name, restype, *argtypes):
    fn = getattr(_lib, name)
    fn.restype = restype
    fn.argtypes = list(argtypes)
    return fn


'''


## @brief Collapse whitespace and fold pointer suffix in a C type spelling.
## @utility
## @version 1
def _bindings_normalize_type(raw):
    """Collapse whitespace, fold `* x` → `*`, return canonical type spelling."""
    t = raw.strip()
    t = re.sub(r"\s+", " ", t)
    t = re.sub(r"\s*\*", "*", t)
    return t


## @brief Split a top-level C argument list at commas.
## @utility
## @version 1
def _bindings_split_args(arglist):
    """Plain comma split; the bindable surface has no inline structs."""
    arglist = arglist.strip()
    if not arglist or arglist == "void":
        return []
    return [a.strip() for a in arglist.split(",")]


## @brief Drop the parameter name and return the ctypes string, or None.
## @utility
## @version 1
def _bindings_arg_type(arg):
    """Drop the parameter name; return the ctypes string or None."""
    arg = re.sub(r"/\*.*?\*/", "", arg).strip()
    parts = arg.rsplit(maxsplit=1)
    raw = _bindings_normalize_type(parts[0] if len(parts) == 2 else arg)
    return _BINDINGS_TYPE_MAP.get(raw)


## @brief Translate an arg list; return (py_args, None) or (None, bad).
## @utility
## @version 1
def _bindings_try_translate_args(args):
    """Translate args; return (py_args, None) on success or (None, bad_arg)."""
    py_args = []
    for a in args:
        t = _bindings_arg_type(a)
        if t is None:
            return None, a
        py_args.append(t)
    return py_args, None


## @brief Return a comment string if the function should be skipped, else None.
## @utility
## @version 1
def _bindings_skip_reason(name, ret_norm, py_ret, bad_arg):
    """Return a skip comment string if applicable, else None."""
    reasons = []
    if name in _BINDINGS_INLINE_CB:
        reasons.append("hand-wired (inline function-pointer in signature)")
    elif py_ret is None:
        reasons.append(f"skipped (unknown return type {ret_norm!r})")
    elif bad_arg is not None:
        reasons.append(f"skipped (unknown arg type in {bad_arg!r})")
    return f"# {name}: {reasons[0]}" if reasons else None


## @brief Emit a single binding line or skip-comment for one declaration.
## @utility
## @version 1
def _bindings_emit_function(name, ret, args):
    """Return a `name = _bind(...)` line, or a skip comment."""
    ret_norm = _bindings_normalize_type(ret)
    py_ret = _BINDINGS_TYPE_MAP.get(ret_norm)
    py_args, bad_arg = _bindings_try_translate_args(args)
    skip = _bindings_skip_reason(name, ret_norm, py_ret, bad_arg)
    if skip is not None:
        return skip
    parts = [f'"{name}"', str(py_ret), *(py_args or [])]
    return f"{name} = _bind({', '.join(parts)})"


## @brief Parse ENTROPIC_EXPORT declarations from a header.
## @utility
## @version 1
def _bindings_parse_header(header_path):
    """Return a list of (name, return_type, args) from the header file."""
    text = Path(header_path).read_text()
    out = []
    for match in _BINDINGS_DECL_RE.finditer(text):
        ret_raw, name, args_raw = match.groups()
        ret = re.sub(r"^\s*(static|inline)\s+", "", ret_raw).strip()
        out.append((name, ret, _bindings_split_args(args_raw)))
    return out


## @brief Regenerate python/src/entropic/_bindings.py from entropic.h.
## @utility
## @version 1
@task(
    help={
        "header": "Path to entropic.h (default: include/entropic/entropic.h)",
        "out": "Output bindings file (default: python/src/entropic/_bindings.py)",
    }
)
def gen_bindings(
    c,
    header="include/entropic/entropic.h",
    out="python/src/entropic/_bindings.py",
):
    """Regenerate python/src/entropic/_bindings.py from the C header.

    Run at release time. Output is checked into the repo so the wheel
    ships with the bindings already prepared. Functions whose
    signatures we can't auto-translate (inline function pointers,
    float args) are emitted as TODO comments — keep the hand-written
    versions of those in sync separately.
    """
    decls = _bindings_parse_header(header)
    if not decls:
        print(f"error: no ENTROPIC_EXPORT declarations found in {header}", file=sys.stderr)
        sys.exit(1)
    body_lines = [_bindings_emit_function(n, r, a) for (n, r, a) in decls]
    out_path = Path(out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(_BINDINGS_PROLOGUE + "\n".join(b for b in body_lines if b) + "\n")
    print(f"  wrote {len(decls)} declarations → {out_path}", file=sys.stderr)
