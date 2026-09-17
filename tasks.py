# SPDX-License-Identifier: Apache-2.0
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


## @brief Parallel job count, capped by memory rather than by core count.
## @utility
## @version 2.12.2
def _default_jobs():
    """Jobs to run in parallel, scaled to free memory, not just to cores.

    os.cpu_count() alone twice killed a session on this box, and NOT via
    the kernel OOM killer: systemd-oomd watches PSI memory pressure on the
    user slice and, past 50% sustained for 20s, kills an entire cgroup
    SCOPE. The scope holding a build is the terminal — so it takes the
    editor, the shell and every child with it, and no individual compiler
    is ever sacrificed in their place.

    That makes this a CONCURRENCY problem, not a total-work problem. The
    same build survives at lower -j because pressure never stays above the
    threshold long enough to trip the killer; it just takes longer.

    Each C++ TU here can take 1-2 GB (llama_cpp_backend.cpp pulls the
    vendored llama.cpp headers), so budget ~2 GB per job against
    MemAvailable and never exceed the core count. ENTROPIC_JOBS overrides
    for anyone who knows better than this heuristic.
    """
    override = os.environ.get("ENTROPIC_JOBS")
    if override and override.isdigit() and int(override) > 0:
        return int(override)
    cores = os.cpu_count() or 4
    try:
        with open("/proc/meminfo") as f:
            avail_kb = next(int(line.split()[1]) for line in f if line.startswith("MemAvailable:"))
    except (OSError, StopIteration, ValueError):
        return cores
    # Reserve headroom before dividing. Spending ALL of MemAvailable is
    # what trips the killer: MemAvailable counts reclaimable page cache, so
    # consuming it forces the reclaim activity that systemd-oomd's
    # threshold explicitly requires ("> 50% for > 20s WITH reclaim
    # activity"). Leave the desktop its cache and take what is left.
    usable_kb = avail_kb - (4 * 1024 * 1024)  # keep 4 GB for the rest of the box
    by_memory = int(usable_kb / (2 * 1024 * 1024))  # ~2 GB per C++ TU
    return max(1, min(cores, by_memory))


JOBS = _default_jobs()

# ctest parallelism is deliberately NOT JOBS. The memory hog is COMPILATION —
# a C++ TU here costs 1-2 GB — while a unit-test binary costs tens of MB, so
# capping tests by the same budget would serialise 1754 fast tests to protect
# against a cost they do not have. Capped anyway rather than left at
# os.cpu_count(): a few of these load vocab GGUFs.
TEST_JOBS = min(os.cpu_count() or 4, max(JOBS, 4))

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


## @brief Read the canonical project version from the repo-root VERSION file.
## @utility
## @return Version string or "unknown".
## @version 2
def _get_version():
    """Read the canonical project version (post-2.1.2 single-source).

    Sibling of ``_cmake_project_version`` — both read the same VERSION
    file. This variant is non-fatal (returns ``"unknown"`` on read
    error) since it's used to populate the model-test results.json
    schema where a missing version shouldn't abort a 30-minute test
    run.
    """
    try:
        version = Path("VERSION").read_text().strip()
        return version if re.fullmatch(r"\d+\.\d+\.\d+", version) else "unknown"
    except OSError:
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


DEFAULT_MODEL_TEST_TIMEOUT_S = 600
#: Seconds to wait between model tests so host pages are reclaimed (gh#142).
#: 25s was proven sufficient in an A/B; 30 leaves margin on a busier machine.
MODEL_TEST_SETTLE_S = 30


## @brief Enumerate the model tests exactly as ctest has them registered.
## @utility
## @return List of {name, command, timeout} dicts; empty on any failure.
## @version 2.12.0
def _model_ctest_tests(build_dir, name_filter=""):
    """Enumerate model tests from ctest — argv, timeout and all.

    v2.11.0: ctest's registration is the SINGLE SOURCE OF TRUTH for what a
    model test is. A directory glob of executables is not the same set, and the
    difference is not cosmetic: it includes [.]-hidden bench binaries that
    collect nothing, orphaned binaries whose sources were deleted, and it
    collapses the per-case ctest entries (add_model_test_per_case) back into one
    process — which is the exact VRAM accumulation those entries exist to avoid.

    Taking `command` verbatim means a per-case entry keeps its case filter and
    its own process, so results.json describes the same run as the gate.

    gh#111: TIMEOUT comes from each test's own CMake property, never a blanket
    constant, so a legitimately slow test is not killed and misreported.

    gh#144 (v2.12.0): `name_filter` is a ctest -R regex applied to the MODEL
    phase. Without it `inv test --model --filter X` filtered only the CPU
    phase and then ran the entire model suite anyway — which on a developer
    box means loading every GGUF in the registry to debug one test, and
    reliably reaching the OOM killer. An empty filter keeps the full-suite
    behaviour the release gate depends on.
    """
    cmd = ["ctest", "--test-dir", build_dir, "--show-only=json-v1", "-L", "model"]
    if name_filter:
        cmd += ["-R", name_filter]
    try:
        out = subprocess.check_output(cmd, stderr=subprocess.DEVNULL)
    except (subprocess.CalledProcessError, FileNotFoundError):
        return []

    tests = []
    for test in json.loads(out).get("tests", []):
        command = test.get("command") or []
        if not command:
            continue
        timeout_s = DEFAULT_MODEL_TEST_TIMEOUT_S
        for prop in test.get("properties", []):
            if prop.get("name") == "TIMEOUT" and prop.get("value"):
                timeout_s = int(prop["value"])
                break
        tests.append(
            {
                "name": test.get("name", "").rsplit("/", 1)[-1],
                "command": command,
                "timeout": timeout_s,
            }
        )
    return tests


## @brief De-wrap the indented message Catch2 prints under a "SKIPPED:" line.
## @utility
## @return Single-line message, or "" when the skip carried no message.
## @version 2.13.0
def _catch2_skip_message(tail):
    """Join the word-wrapped message body following a Catch2 SKIPPED: line.

    Catch2 prints the skip message indented by two spaces and wrapped to the
    console width, so the text has to be re-joined to land in results.json as
    one readable string.
    """
    words = []
    for line in tail:
        if line.startswith("explicitly with message:"):
            continue
        if not line.startswith("  "):
            if words:
                break
            continue
        words.extend(line.split())
    return " ".join(words)


## @brief Extract the reason a model test skipped from its captured output.
## @utility
## @return Reason text, or "" when none was found.
## @version 2.13.0
def _skip_reason_from_log(log_path):
    """First Catch2 skip message in a model test's captured stdout.

    gh#149: results.json recorded only ``status: "skipped"``, so the release
    audit record said a test was skipped and never why. The reason existed —
    Catch2 printed it, and the runner already captures stdout per attempt —
    it just never reached the artifact. Model binaries have 1-2 scenarios and
    a whole binary skips for one reason, so the first message is the reason.
    """
    try:
        lines = Path(log_path).read_text(errors="replace").splitlines()
    except OSError:
        return ""
    for idx, line in enumerate(lines):
        if line.endswith("SKIPPED:"):
            return _catch2_skip_message(lines[idx + 1 :])
    return ""


## @brief Run one model test's ctest argv with retries + a per-attempt timeout.
## @utility
## @return Tuple of (status, retries, duration_ms, skip_reason).
## @version 2.13.0
def _run_one_model_test(command, timeout_s=DEFAULT_MODEL_TEST_TIMEOUT_S, name="model-test"):
    """Run one model test's argv (retries + a per-attempt timeout).

    v2.11.0: takes the full argv ctest registered rather than a bare executable
    path, so a per-case entry carries its own case filter and stays in its own
    process.

    gh#89-C: a Catch2 SKIP (all scenarios SKIP — e.g. a GGUF/VRAM-gated test or
    an intentionally-disabled gate) exits 4 with no assertions. Model tests have
    1-2 scenarios, so a real failure exits 1-2 — making rc==4 an unambiguous SKIP
    signal here, NOT a failure. A hung GPU test is killed at timeout_s
    (TimeoutExpired → rc 124 → fail) so it cannot wedge the suite.

    rc==2 is Catch2's "no tests ran" (all tests hidden via [.] opt-in tag).
    Treated as SKIP — the binary has no runnable tests in the standard suite.

    gh#111 fallout: timeout_s must come from the test's own CMake TIMEOUT
    property (see _model_ctest_tests), not a blanket constant — otherwise
    legitimately slow tests are killed and misreported as failed.
    """
    t0 = time.monotonic()
    retries = 0
    # gh#144 (v2.12.0): keep each attempt's output. It used to go to DEVNULL,
    # so a FAILING model test reported pass/fail and discarded every assertion
    # message, backtrace and log line that said why — leaving a hand-rolled
    # re-run as the only way to see a failure. The GPU time is already spent;
    # throwing away its diagnosis is the expensive part.
    log_dir = Path("build/test-reports/model/logs")
    log_dir.mkdir(parents=True, exist_ok=True)
    for attempt in range(MAX_MODEL_RETRIES + 1):
        # gh#144 (v2.12.0): one log PER ATTEMPT. A single {name}.log was
        # reopened in "w" on each retry, so a flaky test overwrote the failing
        # attempt with the passing one — destroying precisely the output worth
        # having. Attempt 0 keeps the plain name so the common case is
        # unchanged.
        suffix = "" if attempt == 0 else f".retry{attempt}"
        log_path = log_dir / f"{name}{suffix}.log"
        try:
            with open(log_path, "w") as fh:
                rc = subprocess.call(
                    command,
                    stdout=fh,
                    stderr=subprocess.STDOUT,
                    timeout=timeout_s,
                )
        except subprocess.TimeoutExpired:
            rc = 124
        if rc == 0:
            return "pass", attempt, int((time.monotonic() - t0) * 1000), ""
        if rc in (2, 4):
            # gh#149: carry the reason out of the captured output so the audit
            # record states it. rc==2 has no Catch2 message to carry — the
            # binary collected nothing — so name that condition explicitly
            # rather than leaving the reader to guess at a bare SKIP.
            reason = (
                _skip_reason_from_log(log_path)
                if rc == 4
                else (
                    "binary collected no tests — every case is [.]-hidden "
                    "(opt-in suite), so nothing ran"
                )
            )
            return "skipped", attempt, int((time.monotonic() - t0) * 1000), reason
        retries = attempt + 1
    return "fail", retries, int((time.monotonic() - t0) * 1000), ""


## @brief Print one roster line for a finished model test.
## @utility
## @version 2.13.0
def _print_model_test_line(name, status, retries, skip_reason):
    """One roster line. gh#149: a SKIP states its reason here, not just in
    the artifact — the roster is what a reader looks at first."""
    if status == "pass" and retries > 0:
        print(f"  FLAKY  {name} (retry {retries})")
    elif status == "pass":
        print(f"  PASS   {name}")
    elif status == "skipped":
        print(f"  SKIP   {name}" + (f" — {skip_reason}" if skip_reason else ""))
    else:
        print(f"  FAIL   {name} (after {MAX_MODEL_RETRIES} retries)")


## @brief Build one results.json entry for a finished model test.
## @utility
## @return Dict for the results.json `tests` array.
## @version 2.13.0
def _model_result_entry(name, status, retries, duration_ms, skip_reason):
    """One results.json row.

    gh#149: `skip_reason` is present only when a reason was actually
    recovered. An empty field would assert "no reason", where the truth is
    "not recovered" — stating something nothing verified is the whole defect
    this issue is about.
    """
    entry = {
        "name": name,
        "status": status,
        "retries": retries,
        "duration_ms": duration_ms,
    }
    if skip_reason:
        entry["skip_reason"] = skip_reason
    return entry


## @brief Run model tests 1:1; a Catch2 SKIP (rc=4) is reported, not failed.
## @utility
## @return Tuple of (results list, failed count). Skips do NOT count as failures.
## @version 2.13.0
def _run_model_tests(build_dir, name_filter="", resume=False):
    """Run model tests 1:1. Returns (results, failed_count). gh#89: a Catch2
    SKIP (GGUF/VRAM-gated or a disabled gate) reports SKIP, not PASS/FAIL.

    gh#111 fallout: per-test timeout comes from each test's own CMake
    TIMEOUT property (_model_ctest_tests), not a blanket constant.
    """
    # v2.11.0: enumerate from CTEST, not from a directory glob. The glob was a
    # SECOND, DIVERGING GATE and it was wrong three ways at once:
    #   - it picked up the [.]-hidden bench binaries, which collect no tests on
    #     a bare run, exit 2, and were recorded as SKIPs
    #   - it picked up ORPHANED binaries whose sources had been deleted
    #     (test-gh87-verify-qwen35, test-speculative-*), which a rebuild does
    #     not remove, and reported those as SKIPs too
    #   - it ran the per-case-split binaries (gh106, gh108-mtp-stream-grammar)
    #     as ONE process, re-creating the VRAM accumulation that splitting them
    #     into separate ctest entries fixed in 1ca75b9 — so they FAILED here
    #     while passing the real gate
    # ctest's registration is the single source of truth for what a model test
    # IS, including its argv and its per-case isolation. results.json is the
    # release audit record; it has to describe the same run the gate describes.
    tests = _model_ctest_tests(build_dir, name_filter)

    if not tests:
        if name_filter:
            print(f"ERROR: No model tests match -R '{name_filter}'")
        else:
            print("ERROR: No model tests registered in ctest")
        return [], 1

    # A model gate that cannot finish inside one invocation is not a gate.
    # This host kills a long run part-way, so --resume carries completed
    # PASSes forward and runs only what is left; several bounded invocations
    # then produce the same roster one long one would have.
    carried, pending = _partition_resume(tests, resume)

    results = list(carried)
    t_suite = time.monotonic()

    for idx, test in enumerate(pending):
        name = test["name"]
        # gh#142: settle between model tests so the previous process's pages are
        # reclaimed before the next one maps its model.
        #
        # PROVEN by controlled A/B, same binary and same predecessor, only the
        # gap varying:
        #   no delay  -> test-gh87-backend-common-chat FAILS
        #   25s delay -> PASSES
        # GPU offload was identical (0/31 layers) in both arms, so this is HOST
        # memory, not VRAM: that test mmaps a ~12.6 GB CPU-mapped model, and
        # back-to-back it cannot be satisfied before the kernel reclaims.
        #
        # Three of the five v2.11.0 model-suite failures were this, and every
        # one of them passes in isolation.
        if idx > 0:
            time.sleep(MODEL_TEST_SETTLE_S)
        # argv comes from ctest, so a per-case entry carries its own case
        # filter and runs in its own process — the isolation that
        # add_model_test_per_case exists to provide.
        status, retries, duration_ms, skip_reason = _run_one_model_test(
            test["command"], test["timeout"], test["name"]
        )
        _print_model_test_line(name, status, retries, skip_reason)
        results.append(_model_result_entry(name, status, retries, duration_ms, skip_reason))
        # gh#144 (v2.12.0): persist after EVERY test, not only at the end.
        # results.json used to be written once the whole suite finished, so a
        # run killed part-way through lost every completed result — 19 passes
        # were nearly lost that way, and the audit artifact for an interrupted
        # gate was simply absent. Writing incrementally costs one small file
        # write per model test, against minutes of GPU time each.
        _write_results_json(results, int((time.monotonic() - t_suite) * 1000))

    # Counted from `results` (which includes any carried-forward passes)
    # rather than incremented in the loop — same numbers, one source.
    passed = sum(1 for t in results if t["status"] == "pass")
    skipped = sum(1 for t in results if t["status"] == "skipped")
    failed = sum(1 for t in results if t["status"] == "fail")
    flaky = sum(1 for t in results if t["retries"] > 0)
    print(f"\n{passed}/{len(tests)} passed, " f"{skipped} skipped, {flaky} flaky, {failed} failed")
    return results, failed


## @brief Parsed JSON from a file, or {} when absent or unreadable.
## @utility
## @version 2.12.0
def _read_json_file(path):
    """Parsed JSON object, or {} when the file is absent or unreadable."""
    if not os.path.isfile(path):
        return {}
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


## @brief Prior model results eligible for reuse by a resumed run.
## @utility
## @version 2.12.0
def _prior_model_results():
    """Name -> prior result, but ONLY for the exact code under test.

    A resumed suite that stitched results across commits would be a
    FABRICATED audit record - results.json is the release evidence, so a
    version or git_sha mismatch discards the prior file entirely rather
    than reusing part of it.

    Only PASSes carry. A prior SKIP or FAIL is re-run: a skip is a
    failure until something proves otherwise, and carrying one forward
    would let an unrun test look settled.
    """
    data = _read_json_file(MODEL_RESULTS_FILE)
    stale = data.get("version") != _get_version() or data.get("git_sha") != _get_git_sha()
    if data and stale:
        print("  resume: prior results are from a different build - discarding")
    if not data or stale:
        return {}
    return {t["name"]: t for t in data.get("tests", []) if t.get("status") == "pass"}


## @brief Split a model roster into carried-forward passes and work remaining.
## @utility
## @version 2.12.0
def _partition_resume(tests, resume):
    """Return (carried prior results, tests still to run)."""
    prior = _prior_model_results() if resume else {}
    carried = [prior[t["name"]] for t in tests if t["name"] in prior]
    pending = [t for t in tests if t["name"] not in prior]
    if carried:
        print(f"  resume: {len(carried)} prior pass(es) carried, {len(pending)} to run")
    return carried, pending


## @brief Resolve the lead-tier model key from the active local config.
## @return Registry key string (e.g. "qwen3_5_4b") or "unknown" on failure.
## @utility
## @version 2.4.0
def _get_lead_model_key():
    """Read the lead tier's path: key from .entropic/config.local.yaml.

    The model-test ceremony loads the lead tier as the default; recording
    its registry key is the honest value for results.json `model`. v219
    family tests override per-test, but the default-tier load is the
    bulk of the suite.
    """
    local = ".entropic/config.local.yaml"
    if not os.path.isfile(local):
        return "unknown"
    try:
        import yaml

        with open(local) as f:
            data = yaml.safe_load(f) or {}
        lead = (data.get("models") or {}).get("lead") or {}
        return str(lead.get("path") or "unknown")
    except Exception:
        return "unknown"


## @brief Write build/test-reports/model/results.json.
## @utility
## @version 4
def _write_results_json(test_results, duration_ms):
    """Write build/test-reports/model/results.json."""
    os.makedirs(os.path.dirname(MODEL_RESULTS_FILE), exist_ok=True)

    total = len(test_results)
    passed = sum(1 for t in test_results if t["status"] == "pass")
    skipped = sum(1 for t in test_results if t["status"] == "skipped")
    # gh#89-C: a Catch2 SKIP (rc=4) is NOT a failure. Count it distinctly so
    # the audit artifact matches the console roster; the prior
    # `failed = total - passed` mis-bucketed every SKIP as a failure.
    failed = sum(1 for t in test_results if t["status"] == "fail")
    flaky_count = sum(1 for t in test_results if t["retries"] > 0)

    data = {
        "schema_version": 1,
        "version": _get_version(),
        "git_sha": _get_git_sha(),
        "timestamp": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "model": _get_lead_model_key(),
        "gpu": _get_gpu_name(),
        "duration_ms": duration_ms,
        "tests": test_results,
        "summary": {
            "total": total,
            "passed": passed,
            "skipped": skipped,
            "failed": failed,
            "flaky": flaky_count,
        },
    }

    with open(MODEL_RESULTS_FILE, "w") as f:
        json.dump(data, f, indent=2)
        f.write("\n")

    print(f"Written: {MODEL_RESULTS_FILE}")


## @brief Run the CPU phase (unless skipped) then the model gate.
## @utility
## @version 2.12.0
def _run_model_phase(c, build_dir, ctest_args, name_filter, model_only, resume):
    """CPU tests then model tests 1:1; writes results.json, exits non-zero on failure."""
    # The CPU phase runs --parallel JOBS, and that fan-out is the peak-memory
    # moment of the whole command - well above the model phase, which runs
    # 1:1. On a box already near its ceiling it is what gets a long run
    # killed, and re-running a suite that is already green to reach the model
    # phase spends that risk for nothing.
    if not model_only:
        c.run(f'ctest --test-dir {build_dir} {ctest_args} -LE "model|bench"')

    print("\n-- Model tests (GPU) --")
    t0 = time.monotonic()
    results, failed = _run_model_tests(build_dir, name_filter, resume=resume)
    # A resumed run's wall clock covers only the final invocation, which would
    # understate the gate in the audit record. Sum the per-test durations
    # instead, so the recorded figure describes the whole roster.
    duration_ms = (
        sum(r["duration_ms"] for r in results) if resume else int((time.monotonic() - t0) * 1000)
    )

    if results:
        _write_results_json(results, duration_ms)

    if failed > 0:
        raise SystemExit(1)


## @brief Run tests. Builds first unless --no-build.
## @utility
## @version 2.12.2
@task(
    help={
        "model": "Include model tests (GPU recommended, writes results.json)",
        "cpu": "CPU-only (dev preset, used by pre-commit hook)",
        "coverage": "Run under coverage preset with gcovr report",
        "preset": "CMake preset (default: auto-selected)",
        "jobs": f"Parallel test jobs (default: {JOBS})",
        "filter": "CTest -R regex filter",
        "no-build": "Skip build step (assumes already built)",
        "model-only": "With --model, skip the CPU phase and run only model tests",
        "resume": "With --model, carry forward prior passes from results.json",
    }
)
def test(  # noqa: CFQ002
    c,
    model=False,
    cpu=False,
    coverage=False,
    preset="",
    jobs=JOBS,
    filter="",
    no_build=False,
    model_only=False,
    resume=False,
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
    ctest_args = f"--output-on-failure --parallel {TEST_JOBS}"

    if filter:
        ctest_args += f' -R "{filter}"'
    else:
        ctest_args += f' -E "{CTEST_EXCLUDE}"'

    if model:
        _run_model_phase(c, build_dir, ctest_args, filter, model_only, resume)
    else:
        # No --model flag: always exclude the "model" label so a stale
        # model-test registration in build_dir (e.g., leftover from a
        # prior --debug cmake reconfigure) can't get picked up and
        # try to load real GGUFs on the CPU lane. The intent of "no
        # model flag" is "fast unit tests only," and the label is
        # authoritative.
        c.run(f'ctest --test-dir {build_dir} {ctest_args} -LE "model|bench"')

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


## @brief Read the canonical project version from the repo-root VERSION file.
## @utility
## @return Version string (e.g. "2.1.2").
## @version 2
def _cmake_project_version():
    """Return the X.Y.Z from the repo-root VERSION file.

    v2.1.2 (#4 / single-VERSION refactor): version moved out of
    ``CMakeLists.txt`` into ``VERSION`` at the repo root. ``CMakeLists.txt``
    now reads VERSION via ``file(STRINGS ...)`` and feeds it to
    ``project(... VERSION ${ENTROPIC_VERSION} ...)``. The release tooling
    must also read VERSION directly so it agrees with what CMake configured
    the binary with.
    """
    path = Path("VERSION")
    if not path.exists():
        raise SystemExit("VERSION file missing at repo root")
    version = path.read_text().strip()
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        raise SystemExit(f"VERSION file content {version!r} is not a valid X.Y.Z triple")
    return version


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


## @brief Container image for the glibc-portable (Ubuntu 22.04) build.
## @utility
## @version 1
def _docker_base_image(backend):
    """Return the Ubuntu-22.04-based image for a backend.

    cpu  → plain ubuntu:22.04
    cuda → nvidia/cuda devel image on 22.04 (ships nvcc + toolkit; the
           12.8 toolkit is required for sm_100/sm_120 in the arch list).

    Both give a glibc-2.35 userspace so the produced .so runs on
    Ubuntu 22.04 and every newer glibc (24.04, Debian 12, RHEL 9, ...).
    """
    if backend == "cuda":
        return "nvidia/cuda:12.8.0-devel-ubuntu22.04"
    return "ubuntu:22.04"


## @brief Build one backend inside an Ubuntu-22.04 container for glibc portability.
## @utility
## @return Path to the produced .tar.gz.
## @version 1
def _build_release_docker(c, backend, version, outdir, jobs, cuda_arches=""):
    """Run the full build → stage → linkage-check → pack for one backend
    inside an Ubuntu 22.04 container via scripts/docker_release_build.sh.

    The native host build (24.04) links against glibc 2.39 and won't
    load on 22.04. This routes the same cmake invocation through a
    22.04 userspace so the artifact is forward-portable. nvcc
    cross-compiles for the target SASS arches without needing GPU
    access, so no --gpus passthrough is required for the build itself.

    The repo is mounted read-only at /src (build dir is container-local
    under /tmp, so the host tree stays clean of root-owned artifacts);
    dist/ is mounted read-write at /out and the tarball is chowned back
    to the invoking user.

    `cuda_arches` (e.g. "75;89;90;120") overrides the default 12-arch
    SASS list to cut CUDA compile time when targeting a known GPU fleet.
    """
    image = _docker_base_image(backend)
    repo = os.path.abspath(".")
    out = os.path.abspath(outdir)
    os.makedirs(out, exist_ok=True)
    uid, gid = os.getuid(), os.getgid()
    arch_env = f" -e CUDA_ARCHES={cuda_arches}" if cuda_arches else ""
    c.run(
        f"docker run --rm"
        f"{arch_env}"
        f" -v {repo}:/src:ro -v {out}:/out"
        f" {image}"
        f" bash /src/scripts/docker_release_build.sh"
        f" {backend} {version} {jobs} {uid} {gid}"
    )
    return os.path.join(outdir, f"entropic-{version}-linux-x86_64-{backend}.tar.gz")


## @brief Pre-flight the tag-driven release workflow locally (no publish).
## @utility
## @version 3
@task(
    help={
        "version": "Artifact version string (default: CMakeLists project VERSION)",
        "skip_cuda": "Skip the CUDA matrix entry (faster iteration)",
        "outdir": "Output directory for tarballs (default: dist/)",
        "jobs": f"Parallel build jobs (default: {JOBS})",
        "docker": (
            "Build inside an Ubuntu 22.04 container so the .so links "
            "against glibc 2.35 (runs on 22.04+ instead of 24.04-only)."
        ),
        "cuda_arches": (
            "Override the CUDA SASS arch list (e.g. '75;89;90;120') to "
            "cut compile time for a known GPU fleet. Docker builds only."
        ),
    }
)
def release_check(  # noqa: CFQ002
    c, version="", skip_cuda=False, outdir="dist", jobs=JOBS, docker=False, cuda_arches=""
):
    """Pre-flight the local release build (no tag push, no publish).

    The release is built locally (CUDA compilation needs more memory
    than hosted CI runners provide), so this task IS the release build,
    not just a mirror of it.

    Produces .tar.gz per backend and verifies:
      - CMake install tree is self-contained (no unresolved deps)
      - CLI launches via install RPATH (no LD_LIBRARY_PATH needed)
      - find_package(entropic 2.0 REQUIRED) consumer build+links+runs
      - CUDA tarball has libcudart linked (Blackwell native SASS present)

    With --docker, each backend is built inside an Ubuntu 22.04
    container instead of natively. The resulting binaries link against
    glibc 2.35 and run on Ubuntu 22.04 → 24.x, Debian 12, RHEL 9, etc.
    — the native 24.04 build only runs on glibc 2.39+ hosts. Docker +
    nvidia-container-toolkit (for the cuda backend's toolkit image)
    must be available. The in-container driver does its own linkage +
    glibc-floor check; the host-side find_package smoke is skipped
    (the staged tree is inside the container).
    """
    if not version:
        version = _cmake_project_version()
    os.makedirs(outdir, exist_ok=True)

    backends = ["cpu"] if skip_cuda else ["cpu", "cuda"]
    tarballs = {}

    for backend in backends:
        mode = "docker / Ubuntu 22.04" if docker else "native"
        print(f"\n══ Release check: {backend} backend ({mode}) ══")
        if docker:
            tarballs[backend] = _build_release_docker(
                c, backend, version, outdir, jobs, cuda_arches
            )
            print(f"  ✓ {tarballs[backend]}")
            continue
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
            # gcc bug 68080 emits a negative-hits branch entry on rare
            # instrumented lines (currently hook_registry.cpp:28). Without
            # this, gcovr raises NegativeHits and the whole library is
            # reported as "no coverage data" (SKIP). Match the same flag
            # the developer uses when running gcovr by hand.
            "--gcov-ignore-parse-errors=negative_hits.warn_once_per_file",
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


## @brief Regenerate the Python bindings from the C header.
## @utility
## @version 2.2.1
@task(
    help={
        "check": "Verify committed bindings match the header; exit 1 on drift.",
    }
)
def gen_bindings(c, check=False):
    """Regenerate python/src/entropic/_bindings.py (+ manifest) from the C header.

    The real generator lives in ``scripts/gen_bindings.py`` — this is a
    thin shim so ``inv gen-bindings`` is the standard project entry
    point. v2.2.1 replaced the partial regex generator (which skipped
    inline function-pointer params and hard-coded a stale prologue) with
    a full-fidelity bracket-aware C declaration parser. See the script
    docstring for the parser design and CTYPE_MAP coverage rules.

    With ``--check``: regenerates to in-memory buffers and diffs against
    the committed files. Exits 1 if drift is detected (used by the
    pre-commit ``gen-bindings-check`` hook).
    """
    script = Path(__file__).resolve().parent / "scripts" / "gen_bindings.py"
    flag = "--check" if check else ""
    c.run(f"{sys.executable} {script} {flag}".strip())


## @brief Requirements-traceability gate against docs/requirements.yaml.
#  @version 2.10.4
#  @utility
@task
def check_requirements(c):
    """Requirements-traceability gate against docs/requirements.yaml.

    Three checks, one exit code. Each closes a decay path this repo has
    actually taken:

    - orphan @req ids — catches dead ids that doxygen-guard cannot see,
      because it skips bodiless header declarations (that is how a stale
      ``REQ-INFER-003`` survived in ``i_inference_backend.h``)
    - uncovered requirements — a catalog entry nothing implements
    - exemption ratio — a slide back toward blanket
      ``@internal``/``@utility``/``@callback``, which doxygen-guard's own
      coverage command is structurally blind to

    The catalog itself was deleted as collateral in ``2edfb4e`` and stayed
    missing for 15 months because every consumer of it fails open. This
    gate fails closed.
    """
    script = Path(__file__).resolve().parent / "scripts" / "check_requirements.py"
    c.run(f"{sys.executable} {script}")
