"""Hello World — two-tier entropic integration.

Demonstrates entropic's core features:
    - Automatic routing: a tiny router model classifies prompts
    - Tier handoff: simple questions go to the 8B model,
      complex analysis goes to the 14B thinking model
    - VRAM management: only one main model loaded at a time

Usage:
    1. Run once to seed config: python main.py
    2. Edit .hello-world/config.local.yaml with your model paths
    3. Run again: python main.py
    4. Try simple prompts ("hello") and complex ones
       ("design a REST API for a todo app") to see routing in action
"""

from __future__ import annotations

import asyncio
import sys
from pathlib import Path

from entropic import (
    AgentEngine,
    ConfigLoader,
    EngineCallbacks,
    LoopConfig,
    ModelOrchestrator,
    setup_logging,
)

EXAMPLE_ROOT = Path(__file__).resolve().parent


async def main() -> None:
    """Interactive prompt loop with automatic tier routing."""
    # 1. Load config — seeds .hello-world/config.local.yaml on first run
    loader = ConfigLoader(
        project_root=EXAMPLE_ROOT,
        app_dir_name=".hello-world",
        default_config_path=EXAMPLE_ROOT / "config.yaml",
        global_config_dir=None,
    )
    config = loader.load()

    # 2. Set up logging (writes to .hello-world/session.log)
    setup_logging(config, project_dir=EXAMPLE_ROOT, app_dir_name=".hello-world")

    # 3. Initialize orchestrator (loads router + default tier into VRAM)
    orchestrator = ModelOrchestrator(config)
    await orchestrator.initialize()

    # 4. Create engine — default ServerManager provides entropic internal tools
    loop_config = LoopConfig(max_iterations=5, auto_approve_tools=True)
    engine = AgentEngine(orchestrator, config=config, loop_config=loop_config)

    # 5. Wire callbacks: streaming + tier selection visibility
    engine.set_callbacks(
        EngineCallbacks(
            on_stream_chunk=lambda chunk: print(chunk, end="", flush=True),
            on_tier_selected=lambda tier: print(f"\n[routed to: {tier}]"),
        )
    )

    # 6. Interactive loop
    print("entropic hello-world — two tiers: normal (8B) + thinking (14B)")
    print("The router automatically picks the right tier per prompt.")
    print("Type 'quit' to exit.\n")

    while True:
        try:
            prompt = input("You: ").strip()
        except EOFError:
            break
        if not prompt or prompt.lower() in ("quit", "exit", "q"):
            break

        async for _msg in engine.run(prompt):
            pass  # Streaming callback handles display
        print("\n")

    # 7. Clean shutdown
    await orchestrator.shutdown()
    print("Bye!")


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nInterrupted.")
        sys.exit(0)
