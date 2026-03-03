"""GPU utilities for benchmark: VRAM querying and model cleanup."""

import gc
import subprocess
import time
from typing import Any

_VRAM_IDLE_THRESHOLD_MB = 1500  # baseline VRAM when no model is loaded
_VRAM_POLL_INTERVAL_S = 0.25
_VRAM_POLL_MAX_ATTEMPTS = 40  # 10s total


def get_vram_mb() -> tuple[int, int]:
    """Query GPU VRAM via nvidia-smi.

    Returns:
        (used_mb, total_mb). (0, 0) if nvidia-smi unavailable or fails.
    """
    result = subprocess.run(
        ["nvidia-smi", "--query-gpu=memory.used,memory.total", "--format=csv,noheader,nounits"],
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        return 0, 0
    try:
        parts = result.stdout.strip().split(",")
        return int(parts[0].strip()), int(parts[1].strip())
    except (ValueError, IndexError):
        return 0, 0


def get_gpu_info() -> dict[str, Any]:
    """Query GPU name, total VRAM, and CUDA version via nvidia-smi.

    Returns:
        Dict with keys: gpu_name, vram_total_mb, cuda_version.
        Empty dict if nvidia-smi unavailable.
    """
    result = subprocess.run(
        [
            "nvidia-smi",
            "--query-gpu=name,memory.total,driver_version",
            "--format=csv,noheader,nounits",
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        return {}
    try:
        parts = result.stdout.strip().split(",")
        if len(parts) < 3:
            raise ValueError("fewer than 3 fields")
        return {
            "gpu_name": parts[0].strip(),
            "vram_total_mb": int(parts[1].strip()),
            "driver_version": parts[2].strip(),
        }
    except (ValueError, IndexError):
        return {}


def free_model(model: Any) -> None:
    """Delete model object, force GC, and wait for VRAM to return to idle.

    llama.cpp CUDA cleanup is asynchronous — VRAM is not immediately released
    after deletion. This polls until VRAM drops below the idle threshold.

    Args:
        model: Any object holding GPU memory (Llama instance or similar).
    """
    del model
    gc.collect()

    _, _ = get_vram_mb()  # warm up the smi call
    for _ in range(_VRAM_POLL_MAX_ATTEMPTS):
        time.sleep(_VRAM_POLL_INTERVAL_S)
        vram_now, _ = get_vram_mb()
        if vram_now < _VRAM_IDLE_THRESHOLD_MB:
            return


def get_block_count(model_path: str) -> int:
    """Get total transformer block count from model metadata.

    Used to determine the maximum sensible n_gpu_layers for a GPU sweep.

    Args:
        model_path: Absolute path to GGUF file.

    Returns:
        Block count, or 40 as fallback if metadata unavailable.
    """
    try:
        import io
        import sys

        from llama_cpp import Llama

        old_stderr = sys.stderr
        sys.stderr = io.StringIO()
        try:
            model = Llama(model_path=model_path, n_gpu_layers=0, n_ctx=512, verbose=False)
        finally:
            sys.stderr = old_stderr

        for key, value in model.metadata.items():
            if "block_count" in key:
                free_model(model)
                return int(value)
        free_model(model)
    except Exception:
        pass
    return 40  # fallback
