"""
Pinned build versions for llama-cpp-python.

Single source of truth used by both install.sh and `entropic setup-cuda`.

We use the JamePeng fork which actively tracks llama.cpp weekly.
Upstream abetlen/llama-cpp-python has been abandoned since Aug 2025
and its Python bindings are incompatible with any llama.cpp that
includes post-Aug-2025 features (qwen3.5, new KV cache API, etc.).
Same package name and Python API — drop-in replacement.
"""

# llama-cpp-python repo (JamePeng fork, actively maintained)
LLAMA_CPP_PYTHON_REPO = "https://github.com/JamePeng/llama-cpp-python.git"

# Release tag — v0.3.25 natively includes qwen3.5 series support
# (llama.cpp pinned at 079feab9, 2026-02-14)
# Fork uses platform-suffixed tags; all point to the same source commit.
LLAMA_CPP_PYTHON_TAG = "v0.3.25-cu124-Basic-linux-20260215"
