# SPDX-License-Identifier: LGPL-3.0-or-later
"""Hello World — minimal entropic integration.

Demonstrates:
    - Single-tier config with bundled lead identity
    - Streaming output via callback
    - App context injection (consumer personality)

Usage:
    1. Edit config.yaml with your model path
    2. python main.py

@brief Minimal entropic example using C engine via wrapper.
@version 2
"""

from __future__ import annotations

import sys
from pathlib import Path

from entropic import EntropicEngine

EXAMPLE_ROOT = Path(__file__).resolve().parent


## @brief Load engine, run streaming prompt loop, clean shutdown.
## @utility
## @version 2
def main() -> None:
    """Interactive prompt loop.

    @brief Load engine, run streaming prompt loop, clean shutdown.
    @version 2
    """
    config_path = EXAMPLE_ROOT / "config.yaml"
    engine = EntropicEngine(config_path=str(config_path))

    print("entropic hello-world (C engine)")
    print("Type 'quit' to exit.\n")

    while True:
        try:
            prompt = input("You: ").strip()
        except EOFError:
            break
        if not prompt or prompt.lower() in ("quit", "exit", "q"):
            break

        engine.run_streaming(
            prompt,
            on_token=lambda tok: print(tok, end="", flush=True),
        )
        print("\n")

    del engine
    print("Bye!")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nInterrupted.")
        sys.exit(0)
