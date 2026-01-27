# Pre-Work Checklist

> Complete before Claude Code implementation begins

**Estimated Time:** 1-2 hours
**Prerequisites:** Hardware in hand, internet connection

---

## 1. Hardware Confirmation

- [x] Lenovo ThinkPad P16 received
- [x] Ubuntu 24.04 LTS installed
- [x] NVIDIA drivers installed and working (`nvidia-smi` shows GPU)
- [x] GPU confirmed: RTX PRO 4000 with 16GB VRAM

```bash
# Verify GPU
nvidia-smi

# Expected output should show:
# - Driver Version: 550.x or higher
# - CUDA Version: 12.x
# - GPU: NVIDIA RTX 4000 Ada
# - Memory: 16GB
```

---

## 2. Model Downloads

Download all 4 models (~18 GB total):

```bash
# Create models directory
mkdir -p ~/models/gguf
cd ~/models/gguf

# Install huggingface-cli if not present
pip install huggingface_hub

# ═══════════════════════════════════════════════════════════════
# THINKING MODEL: Qwen3-14B (~9 GB) - bartowski quantization
# Used when thinking mode is ON for deep reasoning
# ═══════════════════════════════════════════════════════════════
huggingface-cli download bartowski/Qwen_Qwen3-14B-GGUF \
  --include "Qwen_Qwen3-14B-Q4_K_M.gguf" \
  --local-dir ~/models/gguf

# ═══════════════════════════════════════════════════════════════
# NORMAL MODEL: Qwen3-8B (~5 GB) - bartowski quantization
# Used when thinking mode is OFF for fast reasoning
# ═══════════════════════════════════════════════════════════════
huggingface-cli download bartowski/Qwen_Qwen3-8B-GGUF \
  --include "Qwen_Qwen3-8B-Q4_K_M.gguf" \
  --local-dir ~/models/gguf

# ═══════════════════════════════════════════════════════════════
# CODE MODEL: Qwen2.5-Coder-7B (~4.7 GB) - bartowski quantization
# Used for ALL code generation (regardless of thinking mode)
# ═══════════════════════════════════════════════════════════════
huggingface-cli download bartowski/Qwen2.5-Coder-7B-Instruct-GGUF \
  --include "Qwen2.5-Coder-7B-Instruct-Q4_K_M.gguf" \
  --local-dir ~/models/gguf

# ═══════════════════════════════════════════════════════════════
# MICRO MODEL: Qwen2.5-Coder-0.5B (~0.5 GB) - Official Qwen
# Always loaded for routing decisions
# ═══════════════════════════════════════════════════════════════
huggingface-cli download Qwen/Qwen2.5-Coder-0.5B-Instruct-GGUF \
  --include "qwen2.5-coder-0.5b-instruct-q8_0.gguf" \
  --local-dir ~/models/gguf
```

### Verify Downloads

```bash
ls -lh ~/models/gguf/

# Expected:
# Qwen_Qwen3-14B-Q4_K_M.gguf               ~9.0 GB
# Qwen_Qwen3-8B-Q4_K_M.gguf                ~5.0 GB
# Qwen2.5-Coder-7B-Instruct-Q4_K_M.gguf    ~4.7 GB
# qwen2.5-coder-0.5b-instruct-q8_0.gguf    ~0.5 GB
```

---

## 3. System Packages

```bash
# Update system
sudo apt update && sudo apt upgrade -y

# Install build essentials
sudo apt install -y \
    build-essential \
    cmake \
    git \
    python3-dev \
    python3-pip \
    python3-venv

# Install Docker (for distribution)
curl -fsSL https://get.docker.com | sudo sh
sudo usermod -aG docker $USER
# Log out and back in for group change
```

---

## 4. CUDA Toolkit

> **Important:** The Ubuntu `nvidia-cuda-toolkit` package (12.0) is too old for Blackwell GPUs.
> You must install CUDA 12.8+ from NVIDIA's repository for Blackwell (compute_120) support.

```bash
# Add NVIDIA CUDA repository
wget https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64/cuda-keyring_1.1-1_all.deb
sudo dpkg -i cuda-keyring_1.1-1_all.deb
sudo apt update

# Install CUDA 12.8 toolkit
sudo apt install -y cuda-toolkit-12-8

# Remove old Ubuntu CUDA if installed (conflicts with nvcc)
sudo apt remove -y nvidia-cuda-toolkit nvidia-cuda-toolkit-doc 2>/dev/null || true

# Add to PATH (add to ~/.bashrc)
echo 'export PATH=/usr/local/cuda-12.8/bin:$PATH' >> ~/.bashrc
echo 'export LD_LIBRARY_PATH=/usr/local/cuda-12.8/lib64:$LD_LIBRARY_PATH' >> ~/.bashrc
echo 'export CUDACXX=/usr/local/cuda-12.8/bin/nvcc' >> ~/.bashrc
source ~/.bashrc

# Verify (should show 12.8.x)
nvcc --version
```

---

## 5. Python Environment

```bash
# Create project directory
mkdir -p ~/projects/entropi
cd ~/projects/entropi

# Create virtual environment
python3 -m venv .venv
source .venv/bin/activate

# Upgrade pip
pip install --upgrade pip setuptools wheel

# Install llama-cpp-python with CUDA support
# Build from source for Blackwell GPU support (compute_120)
# CUDACXX must point to the 12.8 nvcc for Blackwell architecture
export CUDACXX=/usr/local/cuda-12.8/bin/nvcc
CMAKE_ARGS="-DGGML_CUDA=on" pip install llama-cpp-python --no-cache-dir

# Verify CUDA support
python3 -c "from llama_cpp import Llama; print('llama-cpp-python installed successfully')"
```

---

## 6. Test Inference

Create a quick test script to verify model loading:

```bash
cat > test_inference.py << 'EOF'
"""Quick inference test for all models."""
import time
from pathlib import Path
from llama_cpp import Llama

MODELS_DIR = Path.home() / "models" / "gguf"

MODELS = {
    "micro": "qwen2.5-coder-0.5b-instruct-q8_0.gguf",
    "code": "Qwen2.5-Coder-7B-Instruct-Q4_K_M.gguf",
    "normal": "Qwen_Qwen3-8B-Q4_K_M.gguf",
    "thinking": "Qwen_Qwen3-14B-Q4_K_M.gguf",
}

def test_model(name: str, filename: str) -> None:
    """Test a single model."""
    path = MODELS_DIR / filename
    if not path.exists():
        print(f"❌ {name}: File not found: {path}")
        return

    print(f"Testing {name} ({filename})...")
    start = time.time()

    try:
        llm = Llama(
            model_path=str(path),
            n_ctx=2048,
            n_gpu_layers=-1,  # Offload all to GPU
            verbose=False,
        )

        load_time = time.time() - start
        print(f"  ✓ Loaded in {load_time:.1f}s")

        # Quick generation test
        start = time.time()
        output = llm.create_chat_completion(
            messages=[{"role": "user", "content": "Say hello in 5 words."}],
            max_tokens=20,
        )
        gen_time = time.time() - start

        response = output["choices"][0]["message"]["content"]
        print(f"  ✓ Generated in {gen_time:.1f}s: {response[:50]}...")

        # Cleanup
        del llm

    except Exception as e:
        print(f"  ❌ Error: {e}")

if __name__ == "__main__":
    print("=" * 60)
    print("Entropi Model Verification")
    print("=" * 60)

    for name, filename in MODELS.items():
        test_model(name, filename)
        print()

    print("=" * 60)
    print("Verification complete!")
EOF

python3 test_inference.py
```

Expected output:
```
============================================================
Entropi Model Verification
============================================================
Testing micro (qwen2.5-coder-0.5b-instruct-q8_0.gguf)...
  ✓ Loaded in 0.5s
  ✓ Generated in 0.1s: Hello there, nice to meet!...

Testing code (Qwen2.5-Coder-7B-Instruct-Q4_K_M.gguf)...
  ✓ Loaded in 2.1s
  ✓ Generated in 0.3s: Hello, how are you today?...

Testing normal (Qwen_Qwen3-8B-Q4_K_M.gguf)...
  ✓ Loaded in 2.5s
  ✓ Generated in 0.4s: Hello! Nice to meet you!...

Testing thinking (Qwen_Qwen3-14B-Q4_K_M.gguf)...
  ✓ Loaded in 4.2s
  ✓ Generated in 0.5s: Hello, it's great meeting you!...

============================================================
Verification complete!
```

---

## 7. Project Structure

```bash
cd ~/projects/entropi

# Create directory structure
mkdir -p src/entropi/{config,core,inference,mcp,quality,storage,ui}
mkdir -p src/entropi/inference/adapters
mkdir -p src/entropi/mcp/servers
mkdir -p src/entropi/quality/analyzers
mkdir -p src/entropi/prompts/templates
mkdir -p tests/{unit,integration,bdd}
mkdir -p docker

# Create initial files
touch src/entropi/__init__.py
touch src/entropi/__main__.py
```

---

## 8. Initial Files

### pyproject.toml

```bash
cat > pyproject.toml << 'EOF'
[build-system]
requires = ["setuptools>=61.0", "wheel"]
build-backend = "setuptools.build_meta"

[project]
name = "entropi"
version = "0.1.0"
description = "Local AI Coding Assistant"
readme = "README.md"
requires-python = ">=3.11"
license = {text = "Apache-2.0"}
authors = [
    {name = "Your Name", email = "you@example.com"}
]
dependencies = [
    "llama-cpp-python>=0.2.0",
    "click>=8.0",
    "rich>=13.0",
    "prompt-toolkit>=3.0",
    "pydantic>=2.0",
    "pydantic-settings>=2.0",
    "aiosqlite>=0.19.0",
    "mcp>=1.0.0",
    "pyyaml>=6.0",
    "httpx>=0.25.0",
]

[project.optional-dependencies]
dev = [
    "pytest>=7.0",
    "pytest-asyncio>=0.21",
    "pytest-cov>=4.0",
    "pytest-bdd>=6.0",
    "black>=23.0",
    "ruff>=0.1.0",
    "mypy>=1.0",
    "pre-commit>=3.0",
]

[project.scripts]
entropi = "entropi.cli:main"

[tool.setuptools.packages.find]
where = ["src"]

[tool.black]
line-length = 100
target-version = ["py311"]

[tool.ruff]
line-length = 100
target-version = "py311"
select = ["E", "F", "I", "N", "W", "C90"]

[tool.ruff.mccabe]
max-complexity = 15

[tool.mypy]
python_version = "3.11"
strict = true
warn_return_any = true
warn_unused_ignores = true

[tool.pytest.ini_options]
asyncio_mode = "auto"
testpaths = ["tests"]
EOF
```

### .pre-commit-config.yaml

```bash
cat > .pre-commit-config.yaml << 'EOF'
repos:
  - repo: https://github.com/pre-commit/pre-commit-hooks
    rev: v4.5.0
    hooks:
      - id: trailing-whitespace
      - id: end-of-file-fixer
      - id: check-yaml
      - id: check-added-large-files
        args: ['--maxkb=1000']
      - id: check-merge-conflict

  - repo: https://github.com/psf/black
    rev: 24.1.1
    hooks:
      - id: black

  - repo: https://github.com/astral-sh/ruff-pre-commit
    rev: v0.1.14
    hooks:
      - id: ruff
        args: [--fix, --exit-non-zero-on-fix]

  - repo: https://github.com/pre-commit/mirrors-mypy
    rev: v1.8.0
    hooks:
      - id: mypy
        additional_dependencies:
          - pydantic>=2.0
          - types-PyYAML

  - repo: https://github.com/PyCQA/flake8
    rev: 7.0.0
    hooks:
      - id: flake8
        additional_dependencies:
          - flake8-cognitive-complexity
          - flake8-functions
        args:
          - --max-cognitive-complexity=15
          - --max-returns-amount=4
          - --max-line-length=100
EOF
```

### .gitignore

```bash
cat > .gitignore << 'EOF'
# Python
__pycache__/
*.py[cod]
*$py.class
*.so
.Python
.venv/
venv/
ENV/
env/
*.egg-info/
dist/
build/
*.egg

# IDE
.idea/
.vscode/
*.swp
*.swo

# Project
.entropi/config.local.yaml
*.db
*.sqlite

# Models (don't commit)
models/

# Testing
.coverage
htmlcov/
.pytest_cache/

# Docker
.docker/

# OS
.DS_Store
Thumbs.db
EOF
```

---

## 9. Pre-commit Setup

```bash
cd ~/projects/entropi
source .venv/bin/activate

# Install dev dependencies
pip install -e ".[dev]"

# Install pre-commit hooks
pre-commit install

# Run on all files (initial check)
pre-commit run --all-files
```

---

## 10. ENTROPI.md Template

Create a template for project context:

```bash
cat > ENTROPI.md << 'EOF'
# Project: Entropi

## Overview
Local AI Coding Assistant powered by Qwen models.

## Architecture
- Task-specialized routing: Qwen3 for reasoning, Qwen2.5-Coder for code
- Thinking mode toggle for deep vs fast reasoning
- MCP-based tool integration

## Conventions
- Python 3.11+
- Type hints required on all public APIs
- Google-style docstrings
- Max cognitive complexity: 15
- Max returns per function: 4

## Key Files
- `src/entropi/cli.py` - CLI entry point
- `src/entropi/app.py` - Application orchestrator
- `src/entropi/inference/router.py` - Task-aware routing

## Commands
- `/think on|off` - Toggle thinking mode
- `/clear` - Clear conversation
- `/status` - Show model status
EOF
```

---

## 11. Verification Script

```bash
cat > verify_setup.py << 'EOF'
#!/usr/bin/env python3
"""Verify all prerequisites are met."""
import shutil
import subprocess
import sys
from pathlib import Path

def check(name: str, condition: bool, fix: str = "") -> bool:
    """Check a condition and print result."""
    if condition:
        print(f"✓ {name}")
        return True
    else:
        print(f"✗ {name}")
        if fix:
            print(f"  Fix: {fix}")
        return False

def main() -> int:
    """Run all checks."""
    print("=" * 60)
    print("Entropi Setup Verification")
    print("=" * 60)

    all_passed = True

    # Check Python version
    py_version = sys.version_info
    all_passed &= check(
        f"Python {py_version.major}.{py_version.minor}",
        py_version >= (3, 11),
        "Install Python 3.11+"
    )

    # Check nvidia-smi
    all_passed &= check(
        "nvidia-smi",
        shutil.which("nvidia-smi") is not None,
        "Install NVIDIA drivers"
    )

    # Check CUDA
    all_passed &= check(
        "nvcc (CUDA compiler)",
        shutil.which("nvcc") is not None,
        "Install CUDA toolkit"
    )

    # Check Docker
    all_passed &= check(
        "Docker",
        shutil.which("docker") is not None,
        "Install Docker"
    )

    # Check models directory
    models_dir = Path.home() / "models" / "gguf"
    all_passed &= check(
        f"Models directory: {models_dir}",
        models_dir.exists(),
        f"mkdir -p {models_dir}"
    )

    # Check each model
    models = {
        "Qwen_Qwen3-14B-Q4_K_M.gguf": "Thinking model",
        "Qwen_Qwen3-8B-Q4_K_M.gguf": "Normal model",
        "Qwen2.5-Coder-7B-Instruct-Q4_K_M.gguf": "Code model",
        "qwen2.5-coder-0.5b-instruct-q8_0.gguf": "Micro model",
    }

    for filename, description in models.items():
        path = models_dir / filename
        all_passed &= check(
            f"{description}: {filename}",
            path.exists(),
            f"Download from HuggingFace"
        )

    # Check llama-cpp-python
    try:
        import llama_cpp
        all_passed &= check("llama-cpp-python", True)
    except ImportError:
        all_passed &= check(
            "llama-cpp-python",
            False,
            "pip install llama-cpp-python with CUDA"
        )

    # Check project structure
    project_dir = Path.home() / "projects" / "entropi"
    all_passed &= check(
        f"Project directory: {project_dir}",
        project_dir.exists(),
        f"mkdir -p {project_dir}"
    )

    print("=" * 60)
    if all_passed:
        print("All checks passed! Ready for implementation.")
        return 0
    else:
        print("Some checks failed. Please fix before proceeding.")
        return 1

if __name__ == "__main__":
    sys.exit(main())
EOF

chmod +x verify_setup.py
python3 verify_setup.py
```

---

## Checklist Summary

- [x] Hardware confirmed (GPU with 16GB VRAM)
- [x] Ubuntu 24.04 installed
- [x] NVIDIA drivers working
- [x] CUDA toolkit installed (12.8 for Blackwell)
- [ ] Docker installed
- [x] Models downloaded (4 models, ~19 GB)
- [x] Python environment created
- [x] llama-cpp-python with CUDA verified
- [x] Project structure created
- [ ] Pre-commit hooks installed
- [x] Entropi CLI working (`entropi ask`, `entropi status`)

---

## Troubleshooting

### CUDA not found
```bash
# Check CUDA 12.8 installation
ls /usr/local/cuda-12.8/
nvcc --version  # Should show 12.8.x

# Ensure PATH is set correctly
echo $PATH | grep cuda-12.8
echo $CUDACXX  # Should be /usr/local/cuda-12.8/bin/nvcc
```

### llama-cpp-python fails to build
```bash
# Ensure cmake is installed
sudo apt install cmake

# For Blackwell GPUs: ensure CUDACXX points to 12.8 nvcc
export CUDACXX=/usr/local/cuda-12.8/bin/nvcc
CMAKE_ARGS="-DGGML_CUDA=on" pip install llama-cpp-python --no-cache-dir --force-reinstall

# If you see "Unsupported gpu architecture 'compute_120'" error:
# - Your CUDA toolkit is too old (need 12.8+ for Blackwell)
# - Make sure nvidia-cuda-toolkit (Ubuntu package) is removed
# - Verify: nvcc --version shows 12.8.x
```

### Model loading fails with VRAM error
```bash
# Check available VRAM
nvidia-smi

# Try loading with fewer GPU layers
# In test script, change n_gpu_layers from -1 to a specific number
```

### Pre-commit hooks fail
```bash
# Run specific hook to debug
pre-commit run black --all-files
pre-commit run ruff --all-files
```

---

## Next Steps

Once this checklist is complete, Claude Code can begin implementation starting with:

1. **Implementation 01: Foundation** — Config, CLI, base classes
2. Continue through all 10 implementation phases

Each phase has clear checkpoints for A/B testing before proceeding.
