"""
Model download helper.

Provides convenient model downloading and verification.
"""

import hashlib
from collections.abc import Sequence
from pathlib import Path
from typing import Any

import click
import httpx
from rich.console import Console
from rich.progress import BarColumn, Progress, SpinnerColumn, TextColumn

console = Console()

# Model registry - Task-specialized models (bartowski quantizations)
MODELS: dict[str, dict[str, Any]] = {
    "thinking": {
        "name": "Qwen_Qwen3-14B-Q4_K_M",
        "url": "https://huggingface.co/bartowski/Qwen_Qwen3-14B-GGUF/resolve/main/Qwen_Qwen3-14B-Q4_K_M.gguf",
        "size_gb": 9.0,
        "sha256": None,  # Add hash for verification
        "description": "Deep reasoning model (thinking mode)",
    },
    "normal": {
        "name": "Qwen_Qwen3-8B-Q4_K_M",
        "url": "https://huggingface.co/bartowski/Qwen_Qwen3-8B-GGUF/resolve/main/Qwen_Qwen3-8B-Q4_K_M.gguf",
        "size_gb": 5.0,
        "sha256": None,
        "description": "General reasoning model (fast mode)",
    },
    "code": {
        "name": "Qwen2.5-Coder-7B-Instruct-Q4_K_M",
        "url": "https://huggingface.co/bartowski/Qwen2.5-Coder-7B-Instruct-GGUF/resolve/main/Qwen2.5-Coder-7B-Instruct-Q4_K_M.gguf",
        "size_gb": 4.7,
        "sha256": None,
        "description": "Code generation model",
    },
    "micro": {
        "name": "qwen2.5-coder-0.5b-instruct-q8_0",
        "url": "https://huggingface.co/Qwen/Qwen2.5-Coder-0.5B-Instruct-GGUF/resolve/main/qwen2.5-coder-0.5b-instruct-q8_0.gguf",
        "size_gb": 0.5,
        "sha256": None,
        "description": "Routing classifier model",
    },
}


def download_models(
    model: str,
    output_dir: Path,
    force: bool = False,
) -> None:
    """
    Download Entropi models.

    Args:
        model: Model key ('thinking', 'normal', 'code', 'micro', 'all')
        output_dir: Output directory for models
        force: Overwrite existing files
    """
    output_dir = output_dir.expanduser()
    output_dir.mkdir(parents=True, exist_ok=True)

    models_to_download: Sequence[str] = list(MODELS.keys()) if model == "all" else [model]

    for model_key in models_to_download:
        model_info = MODELS[model_key]
        filename = f"{model_info['name']}.gguf"
        output_path = output_dir / filename

        if output_path.exists() and not force:
            console.print(f"[yellow]Skipping {model_key} (already exists)[/yellow]")
            continue

        console.print(f"\n[bold]Downloading {model_key}[/bold] ({model_info['size_gb']} GB)")

        try:
            download_file(str(model_info["url"]), output_path)
            console.print(f"[green]✓ Downloaded to {output_path}[/green]")
        except Exception as e:
            console.print(f"[red]✗ Failed to download {model_key}: {e}[/red]")


def download_file(url: str, output_path: Path) -> None:
    """Download file with progress bar."""
    with httpx.stream("GET", url, follow_redirects=True) as response:
        response.raise_for_status()
        total = int(response.headers.get("content-length", 0))

        with Progress(
            SpinnerColumn(),
            TextColumn("[progress.description]{task.description}"),
            BarColumn(),
            TextColumn("[progress.percentage]{task.percentage:>3.0f}%"),
        ) as progress:
            task = progress.add_task("Downloading...", total=total)

            with open(output_path, "wb") as f:
                for chunk in response.iter_bytes(chunk_size=8192):
                    f.write(chunk)
                    progress.update(task, advance=len(chunk))


def verify_file(file_path: Path, expected_hash: str | None) -> bool:
    """Verify file integrity using SHA256."""
    if expected_hash is None:
        return True

    sha256_hash = hashlib.sha256()
    with open(file_path, "rb") as f:
        for chunk in iter(lambda: f.read(8192), b""):
            sha256_hash.update(chunk)

    return sha256_hash.hexdigest() == expected_hash


@click.command()
@click.argument("model", type=click.Choice(list(MODELS.keys()) + ["all"]))
@click.option(
    "--output-dir",
    "-o",
    type=click.Path(path_type=Path),
    default=Path("~/models/gguf").expanduser(),
    help="Output directory for models",
)
@click.option("--force", "-f", is_flag=True, help="Overwrite existing files")
def download(model: str, output_dir: Path, force: bool) -> None:
    """Download Entropi models."""
    download_models(model, output_dir, force)


if __name__ == "__main__":
    download()
