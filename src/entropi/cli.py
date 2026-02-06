"""
CLI entry point for Entropi.

Handles command-line arguments and initializes the application.
"""

import asyncio
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

from entropi import __version__  # noqa: E402
from entropi.config.loader import reload_config  # noqa: E402
from entropi.core.logging import setup_logging  # noqa: E402


@click.group(invoke_without_command=True)
@click.version_option(version=__version__, prog_name="entropi")
@click.option(
    "--config",
    "-c",
    type=click.Path(exists=True, path_type=Path),
    help="Path to configuration file",
)
@click.option(
    "--model",
    "-m",
    type=click.Choice(["thinking", "normal", "code", "micro"]),
    help="Model to use",
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
@click.option(
    "--headless",
    is_flag=True,
    help="Run without TUI (for testing/automation)",
)
@click.pass_context
def main(
    ctx: click.Context,
    config: Path | None,
    model: str | None,
    log_level: str | None,
    project: Path | None,
    headless: bool,
) -> None:
    """
    Entropi - Local AI Coding Assistant

    Run without arguments to start interactive mode.
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

    # Setup logging (writes to .entropi/session.log)
    logger = setup_logging(app_config, project_dir=project_dir)

    # Store in context for subcommands
    ctx.ensure_object(dict)
    ctx.obj["config"] = app_config
    ctx.obj["logger"] = logger
    ctx.obj["project"] = project_dir

    # Store headless flag for subcommands
    ctx.obj["headless"] = headless

    # If no subcommand, start interactive mode
    if ctx.invoked_subcommand is None:
        from entropi.app import Application

        # Create presenter based on headless flag
        presenter = None
        if headless:
            from entropi.ui.headless import HeadlessPresenter

            presenter = HeadlessPresenter()

        app = Application(
            config=app_config,
            project_dir=ctx.obj["project"],
            presenter=presenter,
        )
        asyncio.run(app.run())


@main.command()
@click.pass_context
def status(ctx: click.Context) -> None:
    """Show system and model status."""
    from rich.console import Console
    from rich.table import Table

    console = Console()
    config = ctx.obj["config"]

    table = Table(title="Entropi Status")
    table.add_column("Component", style="cyan")
    table.add_column("Status", style="green")

    # Models
    if config.models.thinking:
        table.add_row("Thinking Model (Qwen3-14B)", str(config.models.thinking.path))
    else:
        table.add_row("Thinking Model", "[dim]Not configured[/dim]")

    if config.models.normal:
        table.add_row("Normal Model (Qwen3-8B)", str(config.models.normal.path))
    else:
        table.add_row("Normal Model", "[dim]Not configured[/dim]")

    if config.models.code:
        table.add_row("Code Model (Qwen2.5-Coder-7B)", str(config.models.code.path))
    else:
        table.add_row("Code Model", "[dim]Not configured[/dim]")

    if config.models.micro:
        table.add_row("Micro Model (Router)", str(config.models.micro.path))
    else:
        table.add_row("Micro Model", "[dim]Not configured[/dim]")

    # Thinking mode
    table.add_row("Thinking Mode Default", str(config.thinking.enabled))

    # Settings
    table.add_row("Routing Enabled", str(config.routing.enabled))
    table.add_row("Quality Enforcement", str(config.quality.enabled))
    table.add_row("Log Level", config.log_level)

    console.print(table)


@main.command()
@click.argument("message", required=False)
@click.option("--no-stream", is_flag=True, help="Disable streaming output")
@click.pass_context
def ask(ctx: click.Context, message: str | None, no_stream: bool) -> None:
    """
    Send a single message and get a response.

    If MESSAGE is not provided, reads from stdin.
    """
    if message is None:
        if sys.stdin.isatty():
            click.echo("Error: No message provided", err=True)
            sys.exit(1)
        message = sys.stdin.read().strip()

    from entropi.app import Application

    config = ctx.obj["config"]
    app = Application(config=config, project_dir=ctx.obj["project"])

    asyncio.run(app.single_turn(message, stream=not no_stream))


@main.command()
@click.pass_context
def init(ctx: click.Context) -> None:
    """Initialize Entropi in the current directory."""
    project_dir = ctx.obj["project"]
    entropi_dir = project_dir / ".entropi"

    if entropi_dir.exists():
        click.echo(f"Entropi already initialized in {project_dir}")
        return

    # Create directories
    entropi_dir.mkdir(parents=True)
    (entropi_dir / "commands").mkdir()

    # Create default config
    default_config = """# Entropi Project Configuration
# See ~/.entropi/config.yaml for global settings

quality:
  enabled: true
  rules:
    max_cognitive_complexity: 15
    require_type_hints: true

permissions:
  allow:
    - "filesystem.*"
    - "git.*"
"""
    (entropi_dir / "config.yaml").write_text(default_config)

    # Create ENTROPI.md in .entropi/
    entropi_md = """# Project Context

This file provides context to Entropi. Edit it to describe your project.

## Overview

<!-- Brief description of what this project does -->

## Tech Stack

<!-- Languages, frameworks, key dependencies -->

## Structure

<!-- Key directories and their purpose -->

## Conventions

<!-- Coding standards, naming conventions, patterns to follow -->
"""
    entropi_md_path = entropi_dir / "ENTROPI.md"
    if not entropi_md_path.exists():
        entropi_md_path.write_text(entropi_md)

    click.echo(f"Initialized Entropi in {project_dir}")
    click.echo("Created:")
    click.echo(f"  - {entropi_dir}/config.yaml")
    click.echo(f"  - {entropi_dir}/commands/")
    click.echo(f"  - {entropi_dir}/ENTROPI.md")


@main.command()
@click.argument("model", type=click.Choice(["thinking", "normal", "code", "micro", "all"]))
@click.option(
    "--output-dir",
    "-o",
    type=click.Path(path_type=Path),
    default=Path("~/models/gguf"),
    help="Output directory for models",
)
@click.option("--force", "-f", is_flag=True, help="Overwrite existing files")
def download(model: str, output_dir: Path, force: bool) -> None:
    """Download Entropi models from HuggingFace."""
    from entropi.cli_download import download_models

    download_models(model, output_dir, force)


@main.command("mcp-bridge")
@click.option(
    "--socket",
    type=click.Path(path_type=Path),
    help="Path to Unix socket (default: ~/.entropi/mcp.sock)",
)
@click.pass_context
def mcp_bridge(ctx: click.Context, socket: Path | None) -> None:
    """
    Run as MCP bridge for Claude Code integration.

    This bridges stdio (used by Claude Code) to Entropi's Unix socket.
    Entropi must already be running for the bridge to connect.

    Configure Claude Code's .mcp.json:

    \b
    {
      "mcpServers": {
        "entropi": {
          "type": "stdio",
          "command": "entropi",
          "args": ["mcp-bridge"]
        }
      }
    }
    """
    from entropi.mcp.bridge import main as bridge_main

    socket_path = str(socket) if socket else None
    exit_code = bridge_main(socket_path)
    sys.exit(exit_code)


if __name__ == "__main__":
    main()
