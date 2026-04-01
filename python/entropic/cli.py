"""CLI entry point for Entropic.

Routes all engine operations through the C API wrapper.
No Python engine fallback — if librentropic.so is not available,
commands that need the engine fail with a clear error.

@brief CLI using EntropicEngine (C wrapper). No Python engine fallback.
@version 1
"""

import sys
from pathlib import Path

import click

from entropic import EntropicEngine


def _resolve_config_path() -> str | None:
    """Find the first available config file.

    @brief Check standard config locations and return first match.
    @version 1
    """
    for candidate in [
        Path(".entropic/config.local.yaml"),
        Path(".entropic/config.yaml"),
        Path.home() / ".entropic" / "config.yaml",
    ]:
        if candidate.is_file():
            return str(candidate.resolve())
    return None


def _create_engine(config_override: str | None = None) -> EntropicEngine:
    """Create an EntropicEngine with config resolution.

    @brief Resolve config path and instantiate C engine.
    @version 1
    """
    config_path = config_override or _resolve_config_path()
    return EntropicEngine(config_path=config_path)


@click.group(invoke_without_command=True)
@click.version_option(prog_name="entropic")
@click.option(
    "--config",
    "-c",
    type=click.Path(exists=True, path_type=Path),
    help="Path to configuration file",
)
@click.pass_context
def main(ctx: click.Context, config: Path | None) -> None:
    """Entropic - Local AI Inference Engine.

    \\b
    Use 'entropic ask "prompt"' for single-turn inference.
    Install entropic-tui for the interactive terminal UI.

    @brief Root CLI group: store config path for subcommands.
    @version 1
    """
    ctx.ensure_object(dict)
    ctx.obj["config_path"] = str(config.resolve()) if config else None

    if ctx.invoked_subcommand is None:
        click.echo(ctx.get_help())


@main.command()
@click.argument("message", required=False)
@click.option("--no-stream", is_flag=True, help="Disable streaming output")
@click.pass_context
def ask(ctx: click.Context, message: str | None, no_stream: bool) -> None:
    """Send a single message and get a response.

    If MESSAGE is not provided, reads from stdin.

    @brief Single-turn inference via EntropicEngine.
    @version 1
    """
    if message is None:
        if sys.stdin.isatty():
            click.echo("Error: No message provided", err=True)
            sys.exit(1)
        message = sys.stdin.read().strip()

    assert message is not None  # guaranteed by stdin read above

    with _create_engine(ctx.obj.get("config_path")) as engine:
        if no_stream:
            result = engine.run(message)
            click.echo(result.response)
        else:
            engine.run_streaming(
                message,
                on_token=lambda tok: sys.stdout.write(tok) or sys.stdout.flush(),
            )
            sys.stdout.write("\n")
            sys.stdout.flush()


@main.command()
@click.pass_context
def status(ctx: click.Context) -> None:
    """Show system and model status.

    @brief Print engine version and model state.
    @version 1
    """
    with _create_engine(ctx.obj.get("config_path")) as engine:
        click.echo("Entropic Status")
        click.echo("=" * 40)
        click.echo("  Backend:   C engine (librentropic)")
        click.echo(f"  Version:   {engine.version()}")
        click.echo(f"  API:       v{engine.api_version()}")


@main.command()
def init() -> None:
    """Initialize Entropic in the current directory.

    @brief Create .entropic/ directory with default config and context file.
    @version 1
    """
    project_dir = Path.cwd()
    entropic_dir = project_dir / ".entropic"

    if entropic_dir.exists():
        click.echo(f"Entropic already initialized in {project_dir}")
        return

    entropic_dir.mkdir(parents=True)

    default_config = """\
# Entropic Project Configuration
# See ~/.entropic/config.yaml for global settings

permissions:
  allow:
    - "filesystem.*"
    - "git.*"
"""
    (entropic_dir / "config.yaml").write_text(default_config)

    entropic_md = """\
# Project Context

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


@main.command()
@click.argument("subcommand", type=click.Choice(["run"]), default="run")
@click.pass_context
def benchmark(ctx: click.Context, subcommand: str) -> None:  # noqa: ARG001  # Click injects
    """Run inference benchmarks.

    @brief Route benchmark commands through EntropicEngine.
    @version 1
    """
    with _create_engine(ctx.obj.get("config_path")) as engine:
        result = engine.run("__benchmark__")
        click.echo(result.response)


if __name__ == "__main__":
    main()
