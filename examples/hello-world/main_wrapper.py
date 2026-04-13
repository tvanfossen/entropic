"""Hello World — minimal entropic integration via Python wrapper.

Demonstrates:
    - Single-tier config with bundled lead identity
    - Streaming output via callback
    - App context injection (consumer personality)

Usage:
    1. Set ENTROPIC_LIB_PATH to point to librentropic.so
    2. Edit config.yaml with your model path
    3. python main_wrapper.py

@brief Minimal entropic example using auto-generated Python wrapper.
@version 1
"""

from __future__ import annotations

import sys
from pathlib import Path

from entropic import EntropicEngine

EXAMPLE_ROOT = Path(__file__).resolve().parent


## @brief Run interactive streaming prompt loop with the C engine.
## @utility
## @version 2.0.2
def main() -> None:
    """Interactive prompt loop using the Python wrapper.

    @brief Load engine via wrapper, run streaming prompt loop, clean shutdown.
    @version 1
    """
    project_dir = str(EXAMPLE_ROOT / ".hello-world")
    engine = EntropicEngine()
    engine.configure_dir(project_dir)

    print("entropic hello-world (C engine, Python wrapper)")
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

    engine.destroy()
    print("Bye!")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nInterrupted.")
        sys.exit(0)
