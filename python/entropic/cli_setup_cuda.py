"""
Clone-at-install builder for llama-cpp-python with CUDA support.

Clones llama-cpp-python at a pinned tag, builds with CUDA (or CPU),
and installs into the current Python environment.

Used by `entropic setup-cuda` for PyPI users and internally by install.sh
for source installs.
"""

import logging
import shutil
import subprocess
import sys
from pathlib import Path

import click

# Pinned build versions — originally in src/entropic/build_config.py.
# JamePeng fork, actively maintained (upstream abandoned since Aug 2025).
LLAMA_CPP_PYTHON_REPO = "https://github.com/JamePeng/llama-cpp-python.git"
LLAMA_CPP_PYTHON_TAG = "v0.3.25-cu124-Basic-linux-20260215"

logger = logging.getLogger("entropic.setup_cuda")


def _default_build_dir() -> Path:
    """Return the default build directory (~/.entropic/.build/)."""
    return Path.home() / ".entropic" / ".build"


def _detect_gpu() -> str | None:
    """Detect NVIDIA GPU via nvidia-smi. Returns GPU name or None."""
    try:
        result = subprocess.run(
            ["nvidia-smi", "--query-gpu=name", "--format=csv,noheader"],
            capture_output=True,
            text=True,
            timeout=10,
        )
        if result.returncode == 0:
            gpu = result.stdout.strip().split("\n")[0]
            if gpu:
                return gpu
    except (FileNotFoundError, subprocess.TimeoutExpired):
        pass
    return None


def _run(cmd: list[str], cwd: Path | None = None, env: dict[str, str] | None = None) -> None:
    """Run a subprocess, raising on failure."""
    import os

    run_env = dict(os.environ)
    if env:
        run_env.update(env)
    result = subprocess.run(cmd, cwd=cwd, env=run_env)
    if result.returncode != 0:
        raise click.ClickException(f"Command failed (exit {result.returncode}): {' '.join(cmd)}")


def _clone(build_dir: Path) -> Path:
    """Clone llama-cpp-python at the pinned tag. Returns clone path."""
    clone_dir = build_dir / "llama-cpp-python"

    if clone_dir.exists():
        click.echo(f"Using cached clone: {clone_dir}")
    else:
        click.echo(f"Cloning llama-cpp-python {LLAMA_CPP_PYTHON_TAG}...")
        build_dir.mkdir(parents=True, exist_ok=True)
        _run(
            [
                "git",
                "clone",
                "--depth=1",
                f"--branch={LLAMA_CPP_PYTHON_TAG}",
                "--recurse-submodules",
                "--shallow-submodules",
                LLAMA_CPP_PYTHON_REPO,
                str(clone_dir),
            ]
        )
        click.echo("Clone complete.")

    return clone_dir


def _build_and_install(clone_dir: Path, *, cuda: bool) -> None:
    """Build llama-cpp-python and install into the current environment."""
    mode = "CUDA" if cuda else "CPU-only"
    click.echo(f"Building llama-cpp-python ({mode})...")

    pip_exe = str(Path(sys.executable).parent / "pip")
    env: dict[str, str] = {"PIP_CONFIG_FILE": "/dev/null"}
    if cuda:
        env["CMAKE_ARGS"] = "-DGGML_CUDA=on"

    _run(
        [pip_exe, "install", str(clone_dir)],
        env=env,
    )
    click.echo("Build and install complete.")


def _verify_gpu_offload() -> bool:
    """Verify the installed llama-cpp-python supports GPU offload."""
    try:
        result = subprocess.run(
            [
                sys.executable,
                "-c",
                "from llama_cpp import llama_supports_gpu_offload; "
                "print(llama_supports_gpu_offload())",
            ],
            capture_output=True,
            text=True,
            timeout=30,
        )
        return result.stdout.strip() == "True"
    except (subprocess.TimeoutExpired, FileNotFoundError):
        return False


def setup_cuda_command(force: bool = False, cpu: bool = False) -> None:
    """Clone, build, and install llama-cpp-python.

    Clones llama-cpp-python at a pinned tag that natively includes
    latest model architecture support, builds with CUDA (or CPU if
    --cpu), and installs into the current Python environment.

    Cache: ~/.entropic/.build/  (re-run is fast, use --force to rebuild)

    Prerequisites: git, cmake, CUDA toolkit (unless --cpu)
    """
    build_dir = _default_build_dir()

    # --force: wipe cached clone
    if force and (build_dir / "llama-cpp-python").exists():
        click.echo("Removing cached build...")
        shutil.rmtree(build_dir / "llama-cpp-python")

    # GPU detection (unless --cpu)
    cuda = False
    if not cpu:
        gpu = _detect_gpu()
        if gpu:
            click.echo(f"Found GPU: {gpu}")
            cuda = True
        else:
            click.echo("No NVIDIA GPU detected. Building CPU-only.")
    else:
        click.echo("CPU-only build requested.")

    # Clone
    clone_dir = _clone(build_dir)

    # Build + install
    _build_and_install(clone_dir, cuda=cuda)

    # Verify
    if cuda:
        if _verify_gpu_offload():
            click.echo("GPU offload: verified working")
        else:
            click.echo(
                "WARNING: GPU offload verification failed. "
                "The build may not have CUDA support. "
                "Check that cmake and CUDA toolkit are installed."
            )

    click.echo("Done.")
