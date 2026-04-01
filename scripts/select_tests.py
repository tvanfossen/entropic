#!/usr/bin/env python3
"""Git diff-based test selection for entropic CI.

Maps changed files to the minimal set of ctest targets that need to run.
Mirror-path mapping only (doxygen-guard impact analysis deferred to v1.10.x).

Usage:
    # PR builds: changes vs base branch
    select_tests.py --diff-against origin/develop

    # Pre-commit: staged changes
    select_tests.py --staged

    # Explicit commit range
    select_tests.py --range abc123..def456

    # Output: ctest command to run selected tests
    # If all tests needed: prints full ctest command
    # If no tests needed: prints nothing, exits 0

@brief Select minimal ctest targets based on git diff.
@version 1.0
"""

import argparse
import fnmatch
import logging
import subprocess
import sys

logger = logging.getLogger(__name__)

# Unified prefix → targets mapping. Order matters: most specific first.
# Covers test files, source files, and headers in one scan.
_PREFIX_MAPPINGS: list[tuple[str, list[str]]] = [
    # Test files (specific before general within each subdir)
    ("tests/unit/api/header_c99", ["entropic-c99-test"]),
    ("tests/unit/api/", ["entropic-api-tests"]),
    ("tests/unit/config/", ["entropic-config-tests"]),
    ("tests/unit/prompts/", ["entropic-config-tests"]),
    ("tests/unit/core/identity_", ["entropic-core-tests", "entropic-identity-tests"]),
    ("tests/unit/core/", ["entropic-core-tests"]),
    ("tests/unit/inference/adapter_manager", ["entropic-adapter-tests"]),
    ("tests/unit/inference/adapter_orchestrator", ["entropic-adapter-tests"]),
    ("tests/unit/inference/grammar_", ["entropic-grammar-tests"]),
    ("tests/unit/inference/profile_registry", ["entropic-cap-tests"]),
    ("tests/unit/inference/throughput_tracker", ["entropic-cap-tests"]),
    ("tests/unit/inference/", ["entropic-inference-tests"]),
    ("tests/unit/mcp/", ["entropic-mcp-tests"]),
    ("tests/unit/storage/", ["entropic-storage-tests"]),
    ("tests/unit/types/", ["entropic-tests"]),
    ("tests/", []),  # non-unit test dirs → no targets
    # Source files (specific files before directories)
    ("src/core/identity_manager.cpp", ["entropic-core-tests", "entropic-identity-tests"]),
    ("src/core/", ["entropic-core-tests"]),
    ("src/inference/adapter_manager.cpp", ["entropic-inference-tests", "entropic-adapter-tests"]),
    (
        "src/inference/grammar_registry.cpp",
        ["entropic-inference-tests", "entropic-grammar-tests", "entropic-identity-tests"],
    ),
    ("src/inference/profile_registry.cpp", ["entropic-cap-tests"]),
    ("src/inference/throughput_tracker.cpp", ["entropic-cap-tests"]),
    ("src/inference/", ["entropic-inference-tests"]),
    ("src/mcp/mcp_authorization.cpp", ["entropic-mcp-tests", "entropic-identity-tests"]),
    ("src/mcp/mcp_key_set.cpp", ["entropic-mcp-tests", "entropic-identity-tests"]),
    ("src/mcp/", ["entropic-mcp-tests"]),
    ("src/config/", ["entropic-config-tests"]),
    ("src/prompts/", ["entropic-config-tests"]),
    ("src/storage/", ["entropic-storage-tests"]),
    ("src/facade/", ["entropic-api-tests"]),
    ("src/types/", ["entropic-tests"]),
    # Headers
    ("include/entropic/core/", ["entropic-core-tests"]),
    (
        "include/entropic/inference/",
        [
            "entropic-inference-tests",
            "entropic-adapter-tests",
            "entropic-grammar-tests",
            "entropic-cap-tests",
        ],
    ),
    ("include/entropic/mcp/", ["entropic-mcp-tests"]),
    ("include/entropic/config/", ["entropic-config-tests"]),
    ("include/entropic/prompts/", ["entropic-config-tests"]),
    ("include/entropic/storage/", ["entropic-storage-tests"]),
    ("include/entropic/types/", ["entropic-tests"]),
]

# All test targets in the project.
ALL_TARGETS = [
    "entropic-tests",
    "entropic-config-tests",
    "entropic-inference-tests",
    "entropic-adapter-tests",
    "entropic-grammar-tests",
    "entropic-cap-tests",
    "entropic-core-tests",
    "entropic-mcp-tests",
    "entropic-storage-tests",
    "entropic-api-tests",
    "entropic-identity-tests",
    "entropic-c99-test",
]

# Patterns that force a full test run.
FULL_RUN_PATTERNS = [
    "CMakeLists.txt",
    "*.cmake",
    "entropic_config.h.in",
    "include/entropic/interfaces/i_*.h",
    "include/entropic/entropic.h",
    "include/entropic/entropic_export.h",
    "scripts/select_tests.py",
]

# Patterns where no tests are needed.
SKIP_PATTERNS = [
    "docs/*",
    "*.md",
    ".claude/*",
    "test-reports/*",
    ".github/*",
    ".gitignore",
    ".pre-commit-config.yaml",
    ".doxygen-guard.yaml",
    "LICENSE*",
]


def get_changed_files(args: argparse.Namespace) -> list[str]:
    """Get list of changed files from git.

    @brief Extract changed file paths from git diff.
    @version 1.0
    """
    cmd = ["git", "diff", "--name-only", "--diff-filter=ACMR"]
    if args.staged:
        cmd.append("--cached")
    elif args.range:
        cmd.extend(args.range.split(".."))
    elif args.diff_against:
        cmd.append(args.diff_against)
    else:
        logger.error("No diff source specified")
        sys.exit(1)

    result = subprocess.run(cmd, capture_output=True, text=True, check=True)
    return [f for f in result.stdout.strip().split("\n") if f]


def matches_any(path: str, patterns: list[str]) -> bool:
    """Check if path matches any glob pattern.

    @brief Test path against a list of glob patterns.
    @version 1.0
    """
    return any(fnmatch.fnmatch(path, p) for p in patterns)


def _resolve_prefix(filepath: str) -> set[str] | None:
    """Table-driven prefix scan across _PREFIX_MAPPINGS.

    @brief First-match prefix lookup for test target resolution.
    @version 1.0
    """
    for prefix, targets in _PREFIX_MAPPINGS:
        if filepath.startswith(prefix):
            return set(targets)
    logger.warning("Unknown file pattern: %s — selecting all tests", filepath)
    return None


def map_file_to_targets(filepath: str) -> set[str] | None:
    """Map a single changed file to test targets.

    Returns None for full-run triggers, empty set for skip patterns,
    or a set of target names for normal files.

    @brief Resolve a changed file path to ctest target names.
    @version 1.0
    """
    if matches_any(filepath, FULL_RUN_PATTERNS):
        return None
    if matches_any(filepath, SKIP_PATTERNS):
        return set()
    return _resolve_prefix(filepath)


def select_tests(changed_files: list[str]) -> list[str]:
    """Select test targets based on changed files.

    @brief Aggregate file mappings into a deduplicated target list.
    @version 1.0
    """
    targets: set[str] = set()

    for filepath in changed_files:
        result = map_file_to_targets(filepath)
        if result is None:
            logger.info("Full run triggered by: %s", filepath)
            return ALL_TARGETS
        targets.update(result)

    return sorted(targets)


def build_ctest_command(targets: list[str], build_dir: str = "build") -> list[str]:
    """Build a ctest command that runs only the selected targets.

    @brief Construct ctest invocation for selected targets.
    @version 1.0
    """
    if not targets:
        return []

    if set(targets) >= set(ALL_TARGETS):
        return ["ctest", "--test-dir", build_dir, "--output-on-failure"]

    regex = "|".join(targets)
    return [
        "ctest",
        "--test-dir",
        build_dir,
        "--tests-regex",
        regex,
        "--output-on-failure",
    ]


def main() -> int:
    """Entry point.

    @brief Parse args, compute selected tests, print ctest command.
    @version 1.0
    """
    parser = argparse.ArgumentParser(description="Select tests based on git diff")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--staged", action="store_true", help="Use staged changes")
    group.add_argument("--range", help="Git commit range (e.g., abc..def)")
    group.add_argument("--diff-against", help="Branch to diff against (e.g., origin/develop)")
    parser.add_argument("--build-dir", default="build", help="Build directory (default: build)")
    parser.add_argument("--verbose", "-v", action="store_true", help="Verbose output")

    args = parser.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(levelname)s: %(message)s",
    )

    changed_files = get_changed_files(args)
    if not changed_files:
        logger.info("No changed files — no tests to run")
        return 0

    logger.info("Changed files: %d", len(changed_files))
    for f in changed_files:
        logger.debug("  %s", f)

    targets = select_tests(changed_files)
    if not targets:
        logger.info("No test-relevant changes detected")
        return 0

    logger.info("Selected targets: %s", ", ".join(targets))

    cmd = build_ctest_command(targets, args.build_dir)
    print(" ".join(cmd))
    return 0


if __name__ == "__main__":
    sys.exit(main())
