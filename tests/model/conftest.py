"""Model test fixtures for headless Application testing.

Architecture:
- Orchestrator is module-scoped: model loaded once per test file
- headless_app is function-scoped: fresh session per test, reuses model
- with_timeout computes timeout from expected turn count
- Pytest hooks generate per-test PlantUML sequence diagrams
"""

import asyncio
import json
import logging
import re
import shutil
import subprocess
import time
from collections.abc import AsyncGenerator
from dataclasses import dataclass, field
from pathlib import Path

import pytest
from entropic.app import Application
from entropic.config.schema import EntropyConfig
from entropic.core.base import Message
from entropic.core.logging import setup_logging, setup_model_logger
from entropic.inference.orchestrator import ModelOrchestrator
from entropic.ui.headless import HeadlessPresenter

logger = logging.getLogger(__name__)

# Timing constants derived from observed performance
AVG_SECONDS_PER_TURN = 15
TURN_TIME_BUFFER = 2.5  # buffer for tool-call path (model writes full files)
ROUTING_TARGET_S = 1.0  # routing tests are sub-second; 1s is a safe ceiling

REPORT_DIR = Path("test-reports")

# Accumulated timeout target for the currently-running test.
# with_timeout() adds to this; the pytest hook reads and resets it.
_accumulated_target: float = 0.0


async def with_timeout(coro, expected_turns: int = 1, name: str = "operation"):
    """Run a coroutine with a timeout scaled to expected agent turns.

    Args:
        coro: Coroutine to run
        expected_turns: How many agent loop turns we expect
        name: Label for error messages

    Returns:
        Tuple of (result, elapsed_seconds)
    """
    global _accumulated_target
    timeout = expected_turns * AVG_SECONDS_PER_TURN * TURN_TIME_BUFFER
    _accumulated_target += timeout
    start = time.perf_counter()
    try:
        async with asyncio.timeout(timeout):
            result = await coro
    except TimeoutError as e:
        elapsed = time.perf_counter() - start
        raise TimeoutError(
            f"Model test timeout: {name} took {elapsed:.1f}s, "
            f"expected <{timeout:.0f}s ({expected_turns} turns × "
            f"{AVG_SECONDS_PER_TURN}s × {TURN_TIME_BUFFER}x buffer)"
        ) from e
    elapsed = time.perf_counter() - start
    return result, elapsed


# =============================================================================
# Test Report Data
# =============================================================================


@dataclass
class TestInteraction:
    """Captured interaction data from a single model test."""

    test_name: str
    duration_s: float = 0.0
    target_s: float = 0.0
    passed: bool = False
    prompt: str = ""
    full_response: str = ""
    tier: str = ""
    messages: list[Message] = field(default_factory=list)
    tool_calls: list[dict] = field(default_factory=list)
    is_routing_test: bool = False


_report_entries: list[TestInteraction] = []


# =============================================================================
# Fixtures
# =============================================================================


@pytest.fixture(scope="module")
async def shared_orchestrator(config: EntropyConfig, models_available: dict[str, bool]):
    """Module-scoped orchestrator. Model loaded once, shared across tests in a file."""
    if not any(models_available.values()):
        pytest.skip("No models available for testing")

    orch = ModelOrchestrator(config)
    await orch.initialize()

    yield orch

    await orch.shutdown()


@pytest.fixture
def headless_presenter() -> HeadlessPresenter:
    """Create a headless presenter for testing."""
    return HeadlessPresenter(auto_approve=True)


@pytest.fixture
async def headless_app(
    config: EntropyConfig,
    shared_orchestrator: ModelOrchestrator,
    headless_presenter: HeadlessPresenter,
    tmp_project_dir: Path,
) -> AsyncGenerator[Application, None]:
    """Create an Application with fresh session state, reusing loaded model.

    - Orchestrator (model) is shared across tests in the module
    - Session state (messages, conversation) is fresh per test
    - Logging wired to tmp_project_dir for per-test log capture
    """
    setup_logging(config, project_dir=tmp_project_dir)
    setup_model_logger(project_dir=tmp_project_dir)

    app = Application(
        config=config,
        project_dir=tmp_project_dir,
        presenter=headless_presenter,
        orchestrator=shared_orchestrator,
    )
    await app.initialize()

    yield app

    await app.shutdown()


# =============================================================================
# Pytest Hooks - Per-Test PlantUML Report
# =============================================================================


def _puml_escape(text: str) -> str:
    """Escape text for safe inclusion in PlantUML note blocks."""
    text = text.replace("&", "&amp;")
    text = text.replace("<", "&lt;")
    text = text.replace(">", "&gt;")
    return text


@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_makereport(item, call):  # noqa: ARG001
    """Capture test interaction data after each model test."""
    global _accumulated_target
    outcome = yield
    report = outcome.get_result()

    if report.when != "call":
        return

    # Read and reset the accumulated target from with_timeout() calls.
    # Falls back to ROUTING_TARGET_S for tests that don't use with_timeout.
    target = _accumulated_target if _accumulated_target > 0 else ROUTING_TARGET_S
    _accumulated_target = 0.0

    entry = TestInteraction(
        test_name=item.name,
        duration_s=report.duration,
        target_s=target,
        passed=report.passed,
    )

    # Extract data from headless presenter
    presenter = item.funcargs.get("headless_presenter")
    if presenter and isinstance(presenter, HeadlessPresenter):
        entry.full_response = presenter.get_stream_content()
        entry.tool_calls = presenter.get_tool_calls()

    # Extract data from headless app
    app = item.funcargs.get("headless_app")
    if app and isinstance(app, Application):
        entry.messages = list(app._messages)
        # First user message is the actual test prompt
        user_msgs = [m for m in app._messages if m.role == "user"]
        if user_msgs:
            entry.prompt = user_msgs[0].content
        if app._orchestrator and app._orchestrator.last_used_tier:
            entry.tier = app._orchestrator.last_used_tier.name

    # Detect routing tests
    if "orchestrator" in item.funcargs and "headless_app" not in item.funcargs:
        entry.is_routing_test = True

    _report_entries.append(entry)

    # Stash logs from tmp_project_dir before fixture teardown cleans it
    _stash_test_logs(item, entry)


def _stash_test_logs(item: pytest.Item, entry: TestInteraction) -> None:
    """Copy session logs from tmp_project_dir to test-reports/logs/<test_name>/.

    Must run during call phase (before fixture teardown cleans tmp_project_dir).
    Writes metadata.json alongside logs for training data labeling.
    """
    tmp_dir = item.funcargs.get("tmp_project_dir")
    if not tmp_dir:
        return

    log_src = Path(tmp_dir) / ".entropic"
    if not log_src.exists():
        return

    log_dest = REPORT_DIR / "logs" / entry.test_name
    log_dest.mkdir(parents=True, exist_ok=True)

    # Copy log files
    for log_file in ("session.log", "session_model.log"):
        src = log_src / log_file
        if src.exists() and src.stat().st_size > 0:
            shutil.copy2(src, log_dest / log_file)

    # Write metadata (actual timing preserved here for analysis)
    metadata = {
        "test_name": entry.test_name,
        "passed": entry.passed,
        "tier": entry.tier,
        "duration_s": round(entry.duration_s, 2),
        "target_s": round(entry.target_s, 1),
        "prompt": entry.prompt,
        "tool_count": len(entry.tool_calls),
        "is_routing_test": entry.is_routing_test,
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S"),
    }
    (log_dest / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")


def pytest_sessionstart(session):  # noqa: ARG001
    """Clean test-reports/logs/ at the start of each run (latest-only)."""
    logs_dir = REPORT_DIR / "logs"
    if logs_dir.exists():
        shutil.rmtree(logs_dir)


def pytest_sessionfinish(session, exitstatus):  # noqa: ARG001
    """Generate per-test PlantUML diagrams, PNGs, and text summary."""
    if not _report_entries:
        return

    REPORT_DIR.mkdir(exist_ok=True)

    puml_paths = []
    for entry in _report_entries:
        path = _write_test_puml(entry)
        puml_paths.append(path)

    _generate_pngs(puml_paths)
    _write_text_summary()


def _write_test_puml(entry: TestInteraction) -> Path:
    """Write a single .puml file into the test's log directory."""
    status_text = "PASSED" if entry.passed else "FAILED"
    color = "palegreen" if entry.passed else "salmon"
    target_label = f"< {entry.target_s:.0f}s"

    lines = [
        "@startuml",
        "skinparam backgroundColor #FEFEFE",
        "skinparam noteBorderColor #999999",
        "skinparam noteBackgroundColor #FFFFEE",
        f"title {entry.test_name}\\n<size:11>[{status_text}] {target_label}</size>",
        "",
    ]

    if entry.is_routing_test:
        _build_routing_puml(lines, entry, color, target_label)
    elif entry.prompt:
        _build_headless_puml(lines, entry, color, target_label)
    else:
        _build_minimal_puml(lines, entry, color)

    lines.append("")
    lines.append("@enduml")

    out_dir = REPORT_DIR / "logs" / entry.test_name
    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / f"{entry.test_name}.puml"
    path.write_text("\n".join(lines))
    return path


def _build_routing_puml(
    lines: list[str], entry: TestInteraction, color: str, target_label: str
) -> None:
    """Build PlantUML for a routing classification test."""
    lines.extend(
        [
            'participant "Test" as T',
            'participant "Orchestrator" as Orch',
            'participant "Router Model" as Rtr',
            "",
            "T -> Orch: classify prompt",
            "Orch -> Rtr: route",
            "Rtr --> Orch: tier result",
            "Orch --> T: result",
            f"note right #{color}: {target_label}",
        ]
    )


def _build_minimal_puml(lines: list[str], entry: TestInteraction, color: str) -> None:
    """Build PlantUML for tests without model interaction (e.g. error tests)."""
    status_text = "PASSED" if entry.passed else "FAILED"
    lines.extend(
        [
            'participant "Test" as T',
            'participant "Application" as App',
            "",
            "T -> App: (internal test)",
            f"App --> T: {status_text}",
            f"note right #{color}",
        ]
    )


def _build_headless_puml(
    lines: list[str], entry: TestInteraction, color: str, target_label: str
) -> None:
    """Build PlantUML for a headless Application test with full output."""
    lines.extend(
        [
            'participant "Test" as T',
            'participant "Application" as App',
            'participant "Engine" as Eng',
            'participant "Orchestrator" as Orch',
            'participant "Model" as Mdl',
            'participant "MCP Tools" as MCP',
            "",
        ]
    )

    # Prompt
    lines.append("T -> App: process_message")
    lines.extend(
        [
            "note right of T",
            "  **Prompt:**",
            f"  {_puml_escape(entry.prompt)}",
            "end note",
        ]
    )

    # Routing
    if entry.tier:
        lines.append("App -> Orch: route")
        lines.append(f"note right: **{entry.tier.upper()}** tier")

    # Build turn-by-turn view from messages
    _build_turns(lines, entry)

    # Final result
    status_text = "PASSED" if entry.passed else "FAILED"
    tool_count = len(entry.tool_calls)
    tool_note = f", {tool_count} tool calls" if tool_count else ""
    lines.append(f"note over T #{color}")
    lines.append(f"  **{status_text}** ({target_label}{tool_note})")
    lines.append("end note")


def _build_turns(lines: list[str], entry: TestInteraction) -> None:
    """Build turn-by-turn sequence from captured messages."""
    turn = 0
    tool_idx = 0

    for msg in entry.messages:
        if msg.role == "assistant":
            turn += 1
            lines.append(f"== Turn {turn} ==")
            lines.append("Orch -> Mdl: generate_stream")

            # Full model output in a note block
            lines.extend(
                [
                    "note right of Mdl",
                    "  **Model Output:**",
                    *_format_note_content(msg.content),
                    "end note",
                ]
            )

            # Check if this turn had tool calls
            _add_turn_tools(lines, entry.tool_calls, tool_idx)
            # Count how many tool calls were in this turn
            # (heuristic: tool calls between this assistant msg and next)
            while tool_idx < len(entry.tool_calls):
                tc = entry.tool_calls[tool_idx]
                if tc.get("status") == "complete":
                    tool_idx += 1
                else:
                    break

        elif msg.role == "user" and turn > 0:
            # Tool result or follow-up (not the initial prompt)
            _add_tool_result_msg(lines, msg)

    # If no assistant messages, show the full response from presenter
    if turn == 0 and entry.full_response:
        lines.append("== Turn 1 ==")
        lines.append("Orch -> Mdl: generate_stream")
        lines.extend(
            [
                "note right of Mdl",
                "  **Model Output:**",
                *_format_note_content(entry.full_response),
                "end note",
            ]
        )
        lines.append("Mdl --> App: response complete")


def _add_turn_tools(lines: list[str], tool_calls: list[dict], start_idx: int) -> None:
    """Add tool call arrows for tools executed in this turn."""
    idx = start_idx
    while idx < len(tool_calls):
        tc = tool_calls[idx]
        if tc.get("status") != "complete":
            break
        name = tc.get("name", "unknown")
        lines.append(f"Mdl --> Eng: tool_call({_puml_escape(name)})")
        lines.append(f"Eng -> MCP: {_puml_escape(name)}")

        # Show tool arguments if present
        args = tc.get("arguments", {})
        if args:
            lines.append("note right of MCP")
            for k, v in args.items():
                lines.append(f"  {_puml_escape(k)}: {_puml_escape(str(v)[:200])}")
            lines.append("end note")

        lines.append("MCP --> Eng: result")

        # Show tool result if present
        result = tc.get("result", "")
        if result:
            lines.extend(
                [
                    "note right of MCP",
                    "  **Tool Result:**",
                    *_format_note_content(str(result)[:500]),
                    "end note",
                ]
            )

        idx += 1


def _add_tool_result_msg(lines: list[str], msg: Message) -> None:
    """Add a user message that carries a tool result back to the model."""
    # Tool results are injected as user messages by the engine
    if msg.content.startswith("Tool "):
        lines.append("Eng -> Mdl: tool result")
    else:
        lines.extend(
            [
                "note right of Eng",
                "  **Follow-up message:**",
                *_format_note_content(msg.content[:300]),
                "end note",
            ]
        )


def _format_note_content(text: str) -> list[str]:
    """Format text content as indented lines inside a PlantUML note block."""
    escaped = _puml_escape(text)
    # Wrap long lines for readability in the diagram
    wrapped = _wrap_text(escaped, width=80)
    return [f"  {line}" for line in wrapped.split("\n")]


def _wrap_text(text: str, width: int = 80) -> str:
    """Soft-wrap text at word boundaries."""
    result_lines = []
    for line in text.split("\n"):
        if len(line) <= width:
            result_lines.append(line)
            continue
        # Wrap at word boundaries
        current = ""
        for word in re.split(r"(\s+)", line):
            if len(current) + len(word) > width and current:
                result_lines.append(current)
                current = word.lstrip()
            else:
                current += word
        if current:
            result_lines.append(current)
    return "\n".join(result_lines)


def _generate_pngs(puml_paths: list[Path]) -> None:
    """Generate PNG images from PlantUML files if plantuml is available."""
    if not shutil.which("plantuml"):
        return

    for path in puml_paths:
        try:
            subprocess.run(
                ["plantuml", "-tpng", str(path)],
                capture_output=True,
                timeout=30,
                check=False,
            )
        except (subprocess.TimeoutExpired, OSError):
            logger.warning("Failed to generate PNG for %s", path.name)


def _write_text_summary() -> None:
    """Generate human-readable text summary with actual timing data."""
    lines = [
        "=== Model Test Report ===",
        "Date: {}".format(time.strftime("%Y-%m-%d %H:%M:%S")),
        "",
    ]

    passed = sum(1 for e in _report_entries if e.passed)
    failed = sum(1 for e in _report_entries if not e.passed)
    total_time = sum(e.duration_s for e in _report_entries)
    lines.append(f"Results: {passed} passed, {failed} failed ({total_time:.1f}s)")
    lines.append("")

    for entry in _report_entries:
        icon = "PASS" if entry.passed else "FAIL"
        tier_label = f" [{entry.tier}]" if entry.tier else ""
        tools_label = f" ({len(entry.tool_calls)} tools)" if entry.tool_calls else ""
        lines.append(
            f"  [{icon}] {entry.test_name}{tier_label}{tools_label} ({entry.duration_s:.1f}s)"
        )
        if entry.prompt:
            lines.append(f"         Prompt: {entry.prompt[:100]}")

    lines.append("")
    lines.append("Per-test artifacts: test-reports/logs/<test_name>/")

    (REPORT_DIR / "model-tests-latest.txt").write_text("\n".join(lines))
