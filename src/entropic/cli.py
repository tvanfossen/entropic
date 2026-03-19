"""
CLI entry point for Entropic.

Handles command-line arguments and initializes the engine.
"""

import asyncio
import logging
import sys
import warnings
from pathlib import Path
from typing import Any

# Suppress common import warnings from dependencies
warnings.filterwarnings("ignore", category=DeprecationWarning, module="pydantic")
warnings.filterwarnings("ignore", category=DeprecationWarning, module="llama_cpp")
warnings.filterwarnings("ignore", category=RuntimeWarning, message=".*found in sys.modules.*")
warnings.filterwarnings("ignore", message=".*pkg_resources.*")

import click  # noqa: E402

from entropic import __version__  # noqa: E402
from entropic.benchmark.cli import benchmark  # noqa: E402
from entropic.config.loader import reload_config  # noqa: E402
from entropic.core.logging import (  # noqa: E402
    setup_display_logger,
    setup_logging,
    setup_model_logger,
)


@click.group(invoke_without_command=True)
@click.version_option(version=__version__, prog_name="entropic")
@click.option(
    "--config",
    "-c",
    type=click.Path(exists=True, path_type=Path),
    help="Path to configuration file",
)
@click.option(
    "--model",
    "-m",
    type=str,
    help="Model tier to use (e.g., lead, eng, arch)",
)
@click.option(
    "--log-level",
    "-l",
    type=click.Choice(["DEBUG", "INFO", "WARNING", "ERROR"]),
    help="Logging level",
)
@click.option(
    "--project",
    "-p",
    type=click.Path(exists=True, file_okay=False, path_type=Path),
    help="Project directory",
)
@click.pass_context
def main(
    ctx: click.Context,
    config: Path | None,  # noqa: ARG001
    model: str | None,
    log_level: str | None,
    project: Path | None,
) -> None:
    """Entropic - Local AI Inference Engine.

    \b
    Use 'entropic ask "prompt"' for single-turn inference.
    Install entropic-tui for the interactive terminal UI.

    @brief Root CLI group: load config, setup logging, store context for subcommands.
    @version 1
    """
    # Build CLI overrides
    cli_overrides: dict[str, Any] = {}

    if model:
        cli_overrides["routing"] = {"default": model}

    if log_level:
        cli_overrides["log_level"] = log_level

    # Load configuration
    app_config = reload_config(cli_overrides)

    # Determine project directory
    project_dir = project or Path.cwd()

    # Setup logging (writes to .entropic/session.log and session_model.log)
    # mcp-bridge is a relay subprocess — skip session log setup so it doesn't
    # truncate the main process's session.log with mode='w'.
    if ctx.invoked_subcommand != "mcp-bridge":
        logger = setup_logging(app_config, project_dir=project_dir)
        setup_model_logger(project_dir=project_dir)
        setup_display_logger(project_dir=project_dir)
    else:
        logger = logging.getLogger("entropic")

    # Store in context for subcommands
    ctx.ensure_object(dict)
    ctx.obj["config"] = app_config
    ctx.obj["logger"] = logger
    ctx.obj["project"] = project_dir

    # No subcommand → print help (engine is a library, not an app)
    if ctx.invoked_subcommand is None:
        click.echo(ctx.get_help())


@main.command()
@click.pass_context
def status(ctx: click.Context) -> None:
    """Show system and model status.

    @brief Print configured model tiers, router, routing state, and log level.
    @version 1
    """
    config = ctx.obj["config"]

    click.echo("Entropic Status")
    click.echo("=" * 40)

    # Models
    for name, tier_config in config.models.tiers.items():
        click.echo(f"  {name.capitalize()} Model: {tier_config.path}")

    if config.models.router:
        click.echo(f"  Router Model:   {config.models.router.path}")
    else:
        click.echo("  Router Model:   Not configured")

    click.echo()
    click.echo(f"  Routing:   {'enabled' if config.routing.enabled else 'disabled'}")
    click.echo(f"  Log Level: {config.log_level}")


@main.command()
@click.argument("message", required=False)
@click.option("--no-stream", is_flag=True, help="Disable streaming output")  # noqa: ARG001
@click.pass_context
def ask(ctx: click.Context, message: str | None, no_stream: bool) -> None:  # noqa: ARG001
    """Send a single message and get a response.

    If MESSAGE is not provided, reads from stdin.

    @brief Single-turn inference: read message from arg or stdin, run engine, print response.
    @version 1
    """
    if message is None:
        if sys.stdin.isatty():
            click.echo("Error: No message provided", err=True)
            sys.exit(1)
        message = sys.stdin.read().strip()

    assert message is not None  # guaranteed by stdin read above
    config = ctx.obj["config"]
    project_dir = ctx.obj["project"]
    asyncio.run(_run_ask(config, project_dir, message))


async def _run_ask(config: Any, project_dir: Path, message: str) -> None:
    """Run a single-turn ask through AgentEngine.

    @brief Initialize orchestrator + engine, stream response to stdout.
    @version 1
    """
    from entropic.core.engine import AgentEngine, EngineCallbacks, LoopConfig
    from entropic.inference.orchestrator import ModelOrchestrator
    from entropic.mcp.manager import ServerManager

    orchestrator = ModelOrchestrator(config)
    await orchestrator.initialize()

    if not orchestrator.get_available_models():
        click.echo("No models configured.", err=True)
        click.echo(
            "Configure models in ~/.entropic/config.yaml or .entropic/config.yaml",
            err=True,
        )
        await orchestrator.shutdown()
        return

    server_manager = ServerManager(
        config,
        project_dir=project_dir,
        tier_names=orchestrator.tier_names,
    )
    await server_manager.initialize()

    loop_config = LoopConfig(
        max_iterations=15,
        max_consecutive_errors=3,
        stream_output=True,
        auto_approve_tools=config.permissions.auto_approve,
    )
    engine = AgentEngine(
        orchestrator=orchestrator,
        server_manager=server_manager,
        config=config,
        loop_config=loop_config,
    )

    def on_chunk(chunk: str) -> None:
        """Handle streaming chunk.

        @brief Write a streamed text chunk to stdout immediately.
        @version 1
        """
        sys.stdout.write(chunk)
        sys.stdout.flush()

    def on_tool_start(tool_call: Any) -> None:
        """Handle tool execution start.

        @brief Print tool name and truncated arguments to stderr.
        @version 1
        """
        args_str = ", ".join(f"{k}={repr(v)[:30]}" for k, v in tool_call.arguments.items())
        if len(args_str) > 60:
            args_str = args_str[:57] + "..."
        click.echo(f"\n[Executing {tool_call.name}({args_str})...]", err=True)

    def on_tool_complete(tool_call: Any, result: str, duration_ms: float) -> None:
        """Handle tool execution completion.

        @brief Print tool name, duration, and result summary to stderr.
        @version 1
        """
        result_len = len(result)
        summary = f"{result_len} chars" if result_len > 100 else result[:50].replace("\n", " ")
        click.echo(f"[Done {tool_call.name} ({duration_ms:.0f}ms, {summary})]", err=True)

    engine.set_callbacks(
        EngineCallbacks(
            on_stream_chunk=on_chunk,
            on_tool_start=on_tool_start,
            on_tool_complete=on_tool_complete,
        )
    )

    try:
        async for _ in engine.run(message):
            pass
        sys.stdout.write("\n")
        sys.stdout.flush()
    finally:
        await server_manager.shutdown()
        await orchestrator.shutdown()


@main.command()
@click.pass_context
def init(ctx: click.Context) -> None:
    """Initialize Entropic in the current directory.

    @brief Create .entropic/ directory with default config.yaml and ENTROPIC.md.
    @version 1
    """
    project_dir = ctx.obj["project"]
    entropic_dir = project_dir / ".entropic"

    if entropic_dir.exists():
        click.echo(f"Entropic already initialized in {project_dir}")
        return

    # Create directories
    entropic_dir.mkdir(parents=True)

    # Create default config (LibraryConfig-shaped, no TUI fields)
    default_config = """# Entropic Project Configuration
# See ~/.entropic/config.yaml for global settings

permissions:
  allow:
    - "filesystem.*"
    - "git.*"
"""
    (entropic_dir / "config.yaml").write_text(default_config)

    # Create ENTROPIC.md in .entropic/
    entropic_md = """# Project Context

This file provides context to Entropic. Edit it to describe your project.

## Overview

<!-- Brief description of what this project does -->

## Tech Stack

<!-- Languages, frameworks, key dependencies -->

## Structure

<!-- Key directories and their purpose -->

## Conventions

<!-- Coding standards, naming conventions, patterns to follow -->
"""
    entropic_md_path = entropic_dir / "ENTROPIC.md"
    if not entropic_md_path.exists():
        entropic_md_path.write_text(entropic_md)

    click.echo(f"Initialized Entropic in {project_dir}")
    click.echo("Created:")
    click.echo(f"  - {entropic_dir}/config.yaml")
    click.echo(f"  - {entropic_dir}/ENTROPIC.md")


@main.command()
@click.argument("model", type=str)
@click.option(
    "--output-dir",
    "-o",
    type=click.Path(path_type=Path),
    default=Path("~/models/gguf"),
    help="Output directory for models",
)
@click.option("--force", "-f", is_flag=True, help="Overwrite existing files")
def download(model: str, output_dir: Path, force: bool) -> None:
    """Download Entropic models from HuggingFace.

    @brief Delegate to cli_download module to fetch model files.
    @version 1
    """
    from entropic.cli_download import download_models

    download_models(model, output_dir, force)


@main.command("setup-cuda")
@click.option("--force", "-f", is_flag=True, help="Remove cached clone and rebuild from scratch")
@click.option("--cpu", is_flag=True, help="Build CPU-only (skip CUDA)")
def setup_cuda(force: bool, cpu: bool) -> None:
    """Build llama-cpp-python with CUDA support for GPU acceleration.

    Clones llama-cpp-python (pinned to a known release), pins the nested
    llama.cpp to a commit with latest model architecture support, and
    installs into the current Python environment.

    \b
    Cache:         ~/.entropic/.build/
    Prerequisites: git, cmake, CUDA toolkit (unless --cpu)

    @brief Clone, pin, and build llama-cpp-python with CUDA or CPU backend.
    @version 1
    """
    from entropic.cli_setup_cuda import setup_cuda_command

    setup_cuda_command(force=force, cpu=cpu)


@main.command("mcp-bridge")
@click.option(
    "--socket",
    type=click.Path(path_type=Path),
    help="Path to Unix socket (default: ~/.entropic/socks/{hash(cwd)}.sock)",
)
@click.pass_context
def mcp_bridge(ctx: click.Context, socket: Path | None) -> None:  # noqa: ARG001
    """Run as MCP bridge for Claude Code integration.

    This bridges stdio (used by Claude Code) to Entropic's Unix socket.
    Entropic must already be running for the bridge to connect.

    \b
    Configure Claude Code's .mcp.json:
    {
      "mcpServers": {
        "entropic": {
          "type": "stdio",
          "command": "entropic",
          "args": ["mcp-bridge"]
        }
      }
    }

    @brief Bridge stdio to Entropic's Unix socket for Claude Code MCP integration.
    @version 1
    """
    from entropic.mcp.bridge import main as bridge_main

    socket_path = str(socket) if socket else None
    exit_code = bridge_main(socket_path)
    sys.exit(exit_code)


main.add_command(benchmark)


if __name__ == "__main__":
    main()
