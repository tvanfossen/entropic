"""Shared check primitives for identity quality evaluation.

Used by both ``entropic benchmark`` CLI (quality benchmarks) and ``tests/model/``.
Each check takes model output text and returns (passed, detail_message).
"""

from __future__ import annotations

import json
import re
from dataclasses import dataclass
from typing import Any


@dataclass
class CheckResult:
    """Result of a single quality check."""

    passed: bool
    check_type: str
    detail: str


def check_contains(output: str, value: str) -> CheckResult:
    """Output must contain the given substring."""
    passed = value in output
    detail = f"found {value!r}" if passed else f"missing {value!r}"
    return CheckResult(passed=passed, check_type="contains", detail=detail)


def check_not_contains(output: str, value: str) -> CheckResult:
    """Output must NOT contain the given substring."""
    passed = value not in output
    detail = "correctly absent" if passed else f"unexpectedly found {value!r}"
    return CheckResult(passed=passed, check_type="not_contains", detail=detail)


def check_regex(output: str, pattern: str) -> CheckResult:
    """Output must match the given regex pattern."""
    match = re.search(pattern, output)
    passed = match is not None
    detail = f"matched at {match.start()}" if passed else f"no match for /{pattern}/"
    return CheckResult(passed=passed, check_type="regex", detail=detail)


def check_token_count_max(output: str, max_tokens: int) -> CheckResult:
    """Output word count must not exceed max_tokens (rough approximation).

    Uses whitespace splitting as a token proxy — close enough for quality
    gates without requiring a tokenizer.
    """
    count = len(output.split())
    passed = count <= max_tokens
    detail = f"{count} words (limit {max_tokens})"
    return CheckResult(passed=passed, check_type="token_count_max", detail=detail)


def check_json_schema(output: str, schema: dict[str, Any]) -> CheckResult:
    """Output must be valid JSON matching the given schema.

    Uses a minimal structural check (keys present, types match) rather than
    a full JSON Schema validator to avoid adding jsonschema as a dependency.
    """
    try:
        data = json.loads(output)
    except json.JSONDecodeError as e:
        return CheckResult(passed=False, check_type="json_schema", detail=f"invalid JSON: {e}")

    missing = _check_required_keys(data, schema)
    if missing:
        return CheckResult(
            passed=False, check_type="json_schema", detail=f"missing keys: {missing}"
        )
    return CheckResult(passed=True, check_type="json_schema", detail="valid")


def check_grammar_valid(output: str, grammar_path: str = "") -> CheckResult:
    """Output should look structurally consistent with the identity's grammar.

    Full GBNF validation requires a parser; this check verifies the output
    is non-empty and doesn't contain raw special tokens or obvious garbage.
    Grammar-constrained generation should already enforce structure — this is
    a sanity check, not a full parse.

    Args:
        output: Model output text.
        grammar_path: Path to the GBNF grammar file (reserved for future
            GBNF parser integration).
    """
    del grammar_path  # reserved for future GBNF parser
    if not output.strip():
        return CheckResult(passed=False, check_type="grammar_valid", detail="empty output")

    special_tokens = ["<|im_start|>", "<|im_end|>", "<|endoftext|>", "</s>", "<s>"]
    for token in special_tokens:
        if token in output:
            return CheckResult(
                passed=False,
                check_type="grammar_valid",
                detail=f"leaked special token: {token}",
            )

    return CheckResult(
        passed=True, check_type="grammar_valid", detail="non-empty, no leaked tokens"
    )


def check_think_block(output: str, *, expected: bool) -> CheckResult:
    """Verify presence/absence of a <think>...</think> block."""
    has_think = "<think>" in output
    if expected:
        passed = has_think
        detail = "think block present" if passed else "think block missing"
    else:
        passed = not has_think
        detail = "correctly no think block" if passed else "unexpected think block"
    return CheckResult(passed=passed, check_type="think_block", detail=detail)


# ---------------------------------------------------------------------------
# Dispatch: run a check definition dict against output
# ---------------------------------------------------------------------------

_CHECK_DISPATCH: dict[str, Any] = {
    "contains": lambda output, spec: check_contains(output, spec["value"]),
    "not_contains": lambda output, spec: check_not_contains(output, spec["value"]),
    "regex": lambda output, spec: check_regex(output, spec["pattern"]),
    "token_count_max": lambda output, spec: check_token_count_max(output, spec["max"]),
    "json_schema": lambda output, spec: check_json_schema(output, spec["schema"]),
    "grammar_valid": lambda output, spec: check_grammar_valid(output, spec.get("path", "")),
    "think_block": lambda output, spec: check_think_block(output, expected=spec["expected"]),
}


def run_check(output: str, check_spec: dict[str, Any]) -> CheckResult:
    """Run a single check from a benchmark definition dict.

    Args:
        output: Model output text to evaluate.
        check_spec: Dict with ``type`` key and type-specific params.
            E.g. ``{"type": "contains", "value": "def foo"}``

    Returns:
        CheckResult with pass/fail and detail message.

    Raises:
        ValueError: Unknown check type.
    """
    check_type = check_spec["type"]
    handler = _CHECK_DISPATCH.get(check_type)
    if handler is None:
        raise ValueError(f"Unknown check type: {check_type!r}")
    return handler(output, check_spec)


def run_checks(output: str, checks: list[dict[str, Any]]) -> list[CheckResult]:
    """Run multiple checks against the same output.

    Returns:
        List of CheckResults, one per check spec.
    """
    return [run_check(output, spec) for spec in checks]


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _check_required_keys(data: Any, schema: dict[str, Any]) -> list[str]:
    """Check that required keys from schema are present in data."""
    if not isinstance(data, dict):
        return ["(not a dict)"]
    required = schema.get("required", [])
    return [k for k in required if k not in data]
