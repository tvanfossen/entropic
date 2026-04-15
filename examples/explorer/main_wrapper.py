# SPDX-License-Identifier: LGPL-3.0-or-later
"""Entropic Explorer — interactive architecture guide via Python wrapper.

Three-tier delegation: guide answers questions, analyst does deep research,
quiz_master generates grammar-constrained quizzes. The docs MCP server
(docs_server.py) queries the doxygen SQLite database for knowledge.

Usage:
    1. Generate docs DB: inv docs --enrich
    2. Set ENTROPIC_LIB_PATH to point to librentropic.so
    3. python main_wrapper.py

@brief Interactive architecture guide using C engine via Python wrapper.
@version 1
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

from entropic import EntropicEngine

EXAMPLE_ROOT = Path(__file__).resolve().parent


## @brief Print streaming tokens to stderr.
## @param tok Token string.
## @callback
## @version 1
def _on_token(tok: str) -> None:
    """Forward tokens to stderr for live display.

    @brief Print streaming tokens to stderr.
    @version 1
    """
    sys.stderr.write(tok)
    sys.stderr.flush()


## @brief Read user input, return None on quit/EOF.
## @return User input string, or None to quit.
## @utility
## @version 1
def _read_input() -> str | None:
    """Prompt and read user input.

    @brief Read user input, return None on quit/EOF.
    @version 1
    """
    try:
        text = input("You: ").strip()
    except EOFError:
        return None
    if text.lower() in ("quit", "exit", "q"):
        return None
    return text


## @brief Run the interactive exploration loop.
## @param engine Entropic engine instance.
## @utility
## @version 1
def _explore_loop(engine: EntropicEngine) -> None:
    """Main interaction loop — read questions, stream responses.

    @brief Run the interactive exploration loop.
    @version 1
    """
    print()
    print("Entropic Explorer — Repo Knowledge Assistant")
    print("============================================")
    print("Ask questions, review changes, learn architecture.")
    print("  'review my changes'  — adversarial change analysis")
    print("  'teach me about X'   — learn + quiz")
    print("  'trace X'            — follow execution paths")
    print("  'quit'               — exit")
    print()

    while True:
        text = _read_input()
        if text is None:
            break
        if not text:
            continue
        sys.stderr.write("\n")
        engine.run_streaming(text, on_token=_on_token)
        print("\n")


## @brief Initialize engine with MCP server, run exploration, clean up.
## @utility
## @version 1
def explorer_loop() -> None:
    """Create engine and run the exploration session.

    @brief Initialize engine with MCP server, run exploration, clean up.
    @version 1
    """
    project_dir = str(EXAMPLE_ROOT / ".explorer")
    engine = EntropicEngine()
    engine.configure_dir(project_dir)
    server_json = json.dumps(
        {"command": "python3", "args": ["servers/docs_server.py"]},
    )
    engine.register_mcp_server("docs", server_json)
    _explore_loop(engine)
    engine.destroy()


## @brief Run explorer loop, handle keyboard interrupt.
## @utility
## @version 1
def main() -> None:
    """Entry point.

    @brief Run explorer loop, handle keyboard interrupt.
    @version 1
    """
    try:
        explorer_loop()
    except KeyboardInterrupt:
        print("\nInterrupted.")
        sys.exit(0)


if __name__ == "__main__":
    main()
