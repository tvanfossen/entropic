"""
Model download helper.

Loads the model registry from data/models.yaml and provides
download + verification for bundled models.
"""

import hashlib
import sys
from collections.abc import Sequence
from pathlib import Path
from typing import Any

import click
import httpx
import yaml

_MODELS_FILE = Path(__file__).parent / "data" / "bundled_models.yaml"


def _load_models() -> dict[str, dict[str, Any]]:
    """Load model registry from bundled YAML."""
    return yaml.safe_load(_MODELS_FILE.read_text())


def download_models(
    model: str,
    output_dir: Path,
    force: bool = False,
) -> None:
    """Download Entropic models.

    @brief Download one or all bundled models to a local directory.
    @version 2

    Args:
        model: Model key (from models.yaml, or 'all')
        output_dir: Output directory for models
        force: Overwrite existing files
    """
    models = _load_models()
    output_dir = output_dir.expanduser()
    output_dir.mkdir(parents=True, exist_ok=True)

    keys: Sequence[str] = list(models.keys()) if model == "all" else [model]

    for model_key in keys:
        model_info = models[model_key]
        filename = f"{model_info['name']}.gguf"
        output_path = output_dir / filename

        if output_path.exists() and not force:
            click.echo(f"Skipping {model_key} (already exists)")
            continue

        click.echo(f"\nDownloading {model_key} ({model_info['size_gb']} GB)")

        try:
            download_file(str(model_info["url"]), output_path)
            click.echo(f"Downloaded to {output_path}")
        except Exception as e:
            click.echo(f"Failed to download {model_key}: {e}", err=True)


def download_file(url: str, output_path: Path) -> None:
    """Download file with plain-text progress.

    @brief Stream a URL to disk with percentage updates on stderr.
    @version 2
    """
    with httpx.stream("GET", url, follow_redirects=True) as response:
        response.raise_for_status()
        total = int(response.headers.get("content-length", 0))
        downloaded = 0
        last_pct = -1

        with open(output_path, "wb") as f:
            for chunk in response.iter_bytes(chunk_size=8192):
                f.write(chunk)
                downloaded += len(chunk)
                if total > 0:
                    pct = downloaded * 100 // total
                    if pct != last_pct:
                        last_pct = pct
                        sys.stderr.write(f"\r  {pct}%")
                        sys.stderr.flush()

        if total > 0:
            sys.stderr.write("\n")
            sys.stderr.flush()


def verify_file(file_path: Path, expected_hash: str | None) -> bool:
    """Verify file integrity using SHA256.

    @brief Compare file SHA-256 against expected digest.
    @version 1
    """
    if expected_hash is None:
        return True

    sha256_hash = hashlib.sha256()
    with open(file_path, "rb") as f:
        for chunk in iter(lambda: f.read(8192), b""):
            sha256_hash.update(chunk)

    return sha256_hash.hexdigest() == expected_hash


@click.command()
@click.argument("model", type=click.STRING)
@click.option(
    "--output-dir",
    "-o",
    type=click.Path(path_type=Path),
    default=Path("~/models/gguf").expanduser(),
    help="Output directory for models",
)
@click.option("--force", "-f", is_flag=True, help="Overwrite existing files")
def download(model: str, output_dir: Path, force: bool) -> None:
    """Download Entropic models.

    MODEL can be a key from models.yaml (primary, mid, lightweight, router) or 'all'.
    """
    models = _load_models()
    valid_keys = list(models.keys()) + ["all"]
    if model not in valid_keys:
        raise click.BadParameter(f"Unknown model '{model}'. Valid: {', '.join(valid_keys)}")
    download_models(model, output_dir, force)


if __name__ == "__main__":
    download()
