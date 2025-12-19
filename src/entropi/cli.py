"""
CLI entry point for Entropi.

Handles command-line arguments and initializes the application.
"""

import asyncio
import sys
from pathlib import Path
from typing import Any

import click

from entropi import __version__
from entropi.config.loader import reload_config
from entropi.core.logging import setup_logging


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
    type=click.Choice(["primary", "workhorse", "fast", "micro"]),
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
@click.pass_context
def main(
    ctx: click.Context,
    config: Path | None,
    model: str | None,
    log_level: str | None,
    project: Path | None,
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

    # Setup logging
    logger = setup_logging(app_config)

    # Store in context for subcommands
    ctx.ensure_object(dict)
    ctx.obj["config"] = app_config
    ctx.obj["logger"] = logger
    ctx.obj["project"] = project or Path.cwd()

    # If no subcommand, start interactive mode
    if ctx.invoked_subcommand is None:
        from entropi.app import Application

        app = Application(config=app_config, project_dir=ctx.obj["project"])
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
    if config.models.primary:
        table.add_row("Primary Model", str(config.models.primary.path))
    else:
        table.add_row("Primary Model", "[dim]Not configured[/dim]")

    if config.models.workhorse:
        table.add_row("Workhorse Model", str(config.models.workhorse.path))

    if config.models.fast:
        table.add_row("Fast Model", str(config.models.fast.path))
    else:
        table.add_row("Fast Model", "[dim]Not configured[/dim]")

    if config.models.micro:
        table.add_row("Micro Model", str(config.models.micro.path))
    else:
        table.add_row("Micro Model", "[dim]Not configured[/dim]")

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

    # Create ENTROPI.md template
    entropi_md = """# Project Context

## About
Describe your project here.

## Structure
- `src/` - Source code
- `tests/` - Test files

## Commands
```bash
# Add common commands here
```

## Standards
- Add coding standards here
"""
    if not (project_dir / "ENTROPI.md").exists():
        (project_dir / "ENTROPI.md").write_text(entropi_md)

    click.echo(f"Initialized Entropi in {project_dir}")
    click.echo("Created:")
    click.echo(f"  - {entropi_dir}/config.yaml")
    click.echo(f"  - {entropi_dir}/commands/")
    click.echo(f"  - {project_dir}/ENTROPI.md")


@main.command()
@click.argument("model", type=click.Choice(["primary", "workhorse", "fast", "micro", "all"]))
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


if __name__ == "__main__":
    main()
