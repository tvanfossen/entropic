# Entropi — Pre-Work Checklist

> Complete this checklist before starting development with Claude Code

**Purpose:** This document is for YOU (the human) to prepare the development environment before implementing Entropi.

---

## 1. Hardware Confirmation

Before proceeding, confirm your hardware matches expectations:

- [ ] **GPU:** NVIDIA RTX PRO 4000 Ada with 16GB VRAM
- [ ] **RAM:** 64GB system memory
- [ ] **CPU:** Intel Core i9
- [ ] **Storage:** At least 50GB free space
- [ ] **OS:** Ubuntu 24.04 LTS

**Verification:**
```bash
# Check GPU
nvidia-smi

# Expected output should show:
# - RTX PRO 4000
# - 16GB memory
# - Driver version 535+ or 545+
```

---

## 2. Model Downloads (~15 GB)

### Create Model Directory
```bash
mkdir -p ~/models/gguf
cd ~/models/gguf
```

### Install Hugging Face CLI
```bash
pip install huggingface_hub
```

### Download All Models

Run these commands (total download: ~15 GB, ~20 min on 100 Mbps):

```bash
# ═══════════════════════════════════════════════════════════════════════════════
# PRIMARY: Qwen2.5-Coder-14B-Instruct Q4_K_M (~9GB)
# Using bartowski for single-file download (no merge needed)
# ═══════════════════════════════════════════════════════════════════════════════
huggingface-cli download bartowski/Qwen2.5-Coder-14B-Instruct-GGUF \
  --include "Qwen2.5-Coder-14B-Instruct-Q4_K_M.gguf" \
  --local-dir ~/models/gguf

# ═══════════════════════════════════════════════════════════════════════════════
# WORKHORSE: Qwen2.5-Coder-7B-Instruct Q4_K_M (~4.7GB)
# ═══════════════════════════════════════════════════════════════════════════════
huggingface-cli download bartowski/Qwen2.5-Coder-7B-Instruct-GGUF \
  --include "Qwen2.5-Coder-7B-Instruct-Q4_K_M.gguf" \
  --local-dir ~/models/gguf

# ═══════════════════════════════════════════════════════════════════════════════
# FAST: Qwen2.5-Coder-1.5B-Instruct Q4_K_M (~1GB)
# ═══════════════════════════════════════════════════════════════════════════════
huggingface-cli download Qwen/Qwen2.5-Coder-1.5B-Instruct-GGUF \
  qwen2.5-coder-1.5b-instruct-q4_k_m.gguf \
  --local-dir ~/models/gguf

# ═══════════════════════════════════════════════════════════════════════════════
# MICRO: Qwen2.5-Coder-0.5B-Instruct Q8_0 (~0.5GB)
# Using Q8_0 for better routing quality (file is tiny anyway)
# ═══════════════════════════════════════════════════════════════════════════════
huggingface-cli download Qwen/Qwen2.5-Coder-0.5B-Instruct-GGUF \
  qwen2.5-coder-0.5b-instruct-q8_0.gguf \
  --local-dir ~/models/gguf
```

### Verify Downloads
```bash
ls -lh ~/models/gguf/

# Expected:
# Qwen2.5-Coder-14B-Instruct-Q4_K_M.gguf  ~9.0 GB
# Qwen2.5-Coder-7B-Instruct-Q4_K_M.gguf   ~4.7 GB
# qwen2.5-coder-1.5b-instruct-q4_k_m.gguf ~1.0 GB
# qwen2.5-coder-0.5b-instruct-q8_0.gguf   ~0.5 GB
```

**Checklist:**
- [ ] 14B model downloaded and verified
- [ ] 7B model downloaded and verified
- [ ] 1.5B model downloaded and verified
- [ ] 0.5B model downloaded and verified

---

## 3. System Packages

### Update System
```bash
sudo apt update && sudo apt upgrade -y
```

### Install Build Dependencies
```bash
sudo apt install -y \
  build-essential \
  cmake \
  git \
  curl \
  wget \
  pkg-config \
  libssl-dev \
  libffi-dev \
  python3-dev \
  python3-pip \
  python3-venv \
  libncurses-dev
```

### Install Docker
```bash
# Install Docker
sudo apt install -y docker.io docker-compose-v2

# Add user to docker group
sudo usermod -aG docker $USER

# IMPORTANT: Log out and back in for group change to take effect
# Or run: newgrp docker
```

**Checklist:**
- [ ] System updated
- [ ] Build dependencies installed
- [ ] Docker installed
- [ ] User added to docker group
- [ ] Logged out and back in (or ran `newgrp docker`)

---

## 4. CUDA Setup

### Check Current State
```bash
# Check if NVIDIA driver is installed
nvidia-smi

# Check if CUDA toolkit is installed
nvcc --version
```

### Install CUDA Toolkit 12.4 (if needed)
```bash
# Add NVIDIA repository
wget https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64/cuda-keyring_1.1-1_all.deb
sudo dpkg -i cuda-keyring_1.1-1_all.deb
sudo apt update

# Install CUDA Toolkit
sudo apt install -y cuda-toolkit-12-4

# Add to PATH
echo 'export PATH=/usr/local/cuda/bin:$PATH' >> ~/.bashrc
echo 'export LD_LIBRARY_PATH=/usr/local/cuda/lib64:$LD_LIBRARY_PATH' >> ~/.bashrc
source ~/.bashrc
```

### Verify CUDA
```bash
nvcc --version
# Should show: Cuda compilation tools, release 12.4

nvidia-smi
# Should show driver and CUDA version
```

**Checklist:**
- [ ] NVIDIA driver working (`nvidia-smi` shows GPU)
- [ ] CUDA Toolkit 12.4+ installed (`nvcc --version` works)
- [ ] PATH configured

---

## 5. Python Environment

### Verify Python Version
```bash
python3 --version
# Should be 3.11 or 3.12 (Ubuntu 24.04 has 3.12)
```

### Create Virtual Environment
```bash
python3 -m venv ~/.venvs/entropi
source ~/.venvs/entropi/bin/activate

# Upgrade pip
pip install --upgrade pip setuptools wheel
```

### Install llama-cpp-python with CUDA
This is the critical step — if this fails, debug before proceeding.

```bash
# Set CUDA architecture for RTX PRO 4000 Ada (sm_89)
CMAKE_ARGS="-DGGML_CUDA=on -DCMAKE_CUDA_ARCHITECTURES=89" \
  pip install llama-cpp-python --no-cache-dir --force-reinstall
```

**If the above fails**, try without architecture flag:
```bash
CMAKE_ARGS="-DGGML_CUDA=on" \
  pip install llama-cpp-python --no-cache-dir --force-reinstall
```

### Test Model Loading
```bash
python3 << 'EOF'
from llama_cpp import Llama
import os

model_path = os.path.expanduser("~/models/gguf/qwen2.5-coder-0.5b-instruct-q8_0.gguf")
if not os.path.exists(model_path):
    print(f"❌ Model not found: {model_path}")
    exit(1)

print("Loading model...")
llm = Llama(
    model_path=model_path,
    n_ctx=2048,
    n_gpu_layers=-1,
    verbose=False
)

print("Running inference...")
output = llm("def fibonacci(n):", max_tokens=50, stop=["\n\n"])
print(output["choices"][0]["text"])
print("\n✅ Model loading and inference successful!")
EOF
```

### Install Other Dependencies
```bash
pip install \
  mcp \
  rich \
  prompt-toolkit \
  click \
  pydantic \
  pydantic-settings \
  aiosqlite \
  httpx \
  pytest \
  pytest-asyncio \
  pytest-bdd \
  mypy \
  black \
  ruff \
  flake8 \
  pre-commit
```

**Checklist:**
- [ ] Python 3.11+ available
- [ ] Virtual environment created
- [ ] llama-cpp-python installed with CUDA
- [ ] Test inference successful
- [ ] Other dependencies installed

---

## 6. Project Setup

### Create Project Directory
```bash
mkdir -p ~/projects/entropi
cd ~/projects/entropi
git init
```

### Create Directory Structure
```bash
mkdir -p src/entropi/{config,core,inference,mcp,quality,storage,ui,prompts}
mkdir -p src/entropi/inference/adapters
mkdir -p src/entropi/mcp/servers
mkdir -p src/entropi/quality/analyzers
mkdir -p src/entropi/prompts/templates
mkdir -p tests/{features,step_defs,unit,integration}
mkdir -p docker
mkdir -p .entropi/commands
```

### Create ENTROPI.md (for Entropi to read about itself)
```bash
cat > ENTROPI.md << 'EOF'
# Entropi

Local AI coding assistant powered by Qwen models.

## Project Overview
Entropi is a terminal-based coding assistant that runs fully locally using quantized Qwen models and llama-cpp-python with CUDA acceleration.

## Tech Stack
- **Language:** Python 3.12
- **Inference:** llama-cpp-python (CUDA)
- **Tools:** MCP (Model Context Protocol)
- **UI:** Rich + Prompt Toolkit
- **Storage:** SQLite (aiosqlite)
- **Config:** Pydantic

## Models (in ~/models/gguf/)
- **Primary (14B):** Qwen2.5-Coder-14B-Instruct-Q4_K_M.gguf
- **Workhorse (7B):** Qwen2.5-Coder-7B-Instruct-Q4_K_M.gguf
- **Fast (1.5B):** qwen2.5-coder-1.5b-instruct-q4_k_m.gguf
- **Micro (0.5B):** qwen2.5-coder-0.5b-instruct-q8_0.gguf

## Commands
```bash
# Activate environment
source ~/.venvs/entropi/bin/activate

# Run in dev mode
python -m entropi

# Run tests
pytest tests/ -v

# Type check
mypy src/

# Format
black src/ tests/
ruff check src/ tests/
```

## Architecture Principles
1. **KISS** — Keep it simple
2. **DRY** — Don't repeat yourself
3. **Modular** — Highly encapsulated components
4. **Configurable** — Everything is configurable
5. **MCP-First** — All tools via MCP protocol
6. **Quality Enforced** — Code quality checked at generation time

## Key Design Decisions
- Multi-model routing (14B/7B/1.5B/0.5B)
- Docker-only distribution
- Pre-commit enforcement matches generation-time enforcement
- BDD tests with pytest-bdd
EOF
```

### Create Initial pyproject.toml
```bash
cat > pyproject.toml << 'EOF'
[project]
name = "entropi"
version = "0.1.0"
description = "Local AI coding assistant powered by Qwen models"
readme = "README.md"
license = {text = "Apache-2.0"}
requires-python = ">=3.11"
authors = [
    {name = "Your Name", email = "you@example.com"}
]
keywords = ["ai", "coding", "assistant", "llm", "local"]
classifiers = [
    "Development Status :: 3 - Alpha",
    "Environment :: Console",
    "Intended Audience :: Developers",
    "License :: OSI Approved :: Apache Software License",
    "Programming Language :: Python :: 3.11",
    "Programming Language :: Python :: 3.12",
    "Topic :: Software Development",
]

dependencies = [
    "llama-cpp-python>=0.2.0",
    "mcp>=0.1.0",
    "rich>=13.0.0",
    "prompt-toolkit>=3.0.0",
    "click>=8.0.0",
    "pydantic>=2.0.0",
    "pydantic-settings>=2.0.0",
    "aiosqlite>=0.19.0",
    "httpx>=0.25.0",
]

[project.optional-dependencies]
dev = [
    "pytest>=7.0.0",
    "pytest-asyncio>=0.21.0",
    "pytest-bdd>=7.0.0",
    "pytest-cov>=4.0.0",
    "mypy>=1.0.0",
    "black>=23.0.0",
    "ruff>=0.1.0",
    "flake8>=6.0.0",
    "flake8-cognitive-complexity>=0.1.0",
    "pre-commit>=3.0.0",
]

[project.scripts]
entropi = "entropi.cli:main"

[build-system]
requires = ["setuptools>=61.0", "wheel"]
build-backend = "setuptools.build_meta"

[tool.setuptools.packages.find]
where = ["src"]

[tool.black]
line-length = 100
target-version = ["py311", "py312"]

[tool.ruff]
line-length = 100
select = ["E", "F", "W", "I", "N", "UP", "B", "C4"]
ignore = ["E501"]

[tool.mypy]
python_version = "3.12"
strict = true
ignore_missing_imports = true

[tool.pytest.ini_options]
asyncio_mode = "auto"
testpaths = ["tests"]
python_files = ["test_*.py"]
python_functions = ["test_*"]
EOF
```

### Create Pre-commit Configuration
```bash
cat > .pre-commit-config.yaml << 'EOF'
repos:
  - repo: https://github.com/pre-commit/pre-commit-hooks
    rev: v4.5.0
    hooks:
      - id: trailing-whitespace
      - id: end-of-file-fixer
      - id: check-yaml
      - id: check-json
      - id: check-toml
      - id: check-merge-conflict
      - id: check-added-large-files
        args: ['--maxkb=1000']
      - id: debug-statements

  - repo: https://github.com/psf/black
    rev: 24.3.0
    hooks:
      - id: black
        args: ['--line-length=100']

  - repo: https://github.com/astral-sh/ruff-pre-commit
    rev: v0.3.4
    hooks:
      - id: ruff
        args: ['--fix']

  - repo: https://github.com/pre-commit/mirrors-mypy
    rev: v1.9.0
    hooks:
      - id: mypy
        additional_dependencies:
          - pydantic>=2.0.0
          - types-aiofiles

  - repo: https://github.com/pycqa/flake8
    rev: 7.0.0
    hooks:
      - id: flake8
        additional_dependencies:
          - flake8-cognitive-complexity
          - flake8-functions
        args:
          - '--max-cognitive-complexity=15'
          - '--max-returns-amount=3'
          - '--max-line-length=100'
          - '--ignore=E501,W503'
EOF
```

### Initialize Pre-commit
```bash
cd ~/projects/entropi
source ~/.venvs/entropi/bin/activate
pre-commit install
```

### Create Initial .gitignore
```bash
cat > .gitignore << 'EOF'
# Python
__pycache__/
*.py[cod]
*$py.class
*.so
.Python
build/
develop-eggs/
dist/
downloads/
eggs/
.eggs/
lib/
lib64/
parts/
sdist/
var/
wheels/
*.egg-info/
.installed.cfg
*.egg

# Virtual environments
.venv/
venv/
ENV/

# IDEs
.idea/
.vscode/
*.swp
*.swo
*~

# Testing
.pytest_cache/
.coverage
htmlcov/
.tox/
.nox/

# Type checking
.mypy_cache/

# Local config (don't commit)
.entropi/config.local.yaml
.entropi/*.local.*

# Models (too large)
*.gguf

# Database
*.db
*.sqlite

# Logs
*.log
logs/

# OS
.DS_Store
Thumbs.db
EOF
```

### Create Empty __init__.py Files
```bash
touch src/entropi/__init__.py
touch src/entropi/config/__init__.py
touch src/entropi/core/__init__.py
touch src/entropi/inference/__init__.py
touch src/entropi/inference/adapters/__init__.py
touch src/entropi/mcp/__init__.py
touch src/entropi/mcp/servers/__init__.py
touch src/entropi/quality/__init__.py
touch src/entropi/quality/analyzers/__init__.py
touch src/entropi/storage/__init__.py
touch src/entropi/ui/__init__.py
touch src/entropi/prompts/__init__.py
touch tests/__init__.py
```

### Initial Commit
```bash
git add .
git commit -m "Initial project structure"
```

**Checklist:**
- [ ] Project directory created
- [ ] Directory structure created
- [ ] ENTROPI.md created
- [ ] pyproject.toml created
- [ ] .pre-commit-config.yaml created
- [ ] Pre-commit installed
- [ ] .gitignore created
- [ ] Initial commit made

---

## 7. Install Claude Code

```bash
# Via npm
npm install -g @anthropic-ai/claude-code

# Or via Anthropic's installer
curl -fsSL https://claude.ai/install-claude-code.sh | bash

# Verify
claude --version
```

**Checklist:**
- [ ] Claude Code installed
- [ ] `claude --version` works

---

## 8. Verification Script

Run this script to verify everything is ready:

```bash
#!/bin/bash
set -e

echo "═══════════════════════════════════════════════════════════════"
echo "                ENTROPI PRE-WORK VERIFICATION                   "
echo "═══════════════════════════════════════════════════════════════"

# Check GPU
echo -n "Checking GPU... "
if nvidia-smi > /dev/null 2>&1; then
    echo "✅ NVIDIA GPU detected"
else
    echo "❌ NVIDIA GPU not found"
    exit 1
fi

# Check CUDA
echo -n "Checking CUDA... "
if nvcc --version > /dev/null 2>&1; then
    echo "✅ CUDA toolkit installed"
else
    echo "❌ CUDA toolkit not found"
    exit 1
fi

# Check Docker
echo -n "Checking Docker... "
if docker --version > /dev/null 2>&1; then
    echo "✅ Docker installed"
else
    echo "❌ Docker not found"
    exit 1
fi

# Check models
echo -n "Checking models... "
MODEL_DIR="$HOME/models/gguf"
MISSING=0
for model in \
    "Qwen2.5-Coder-14B-Instruct-Q4_K_M.gguf" \
    "Qwen2.5-Coder-7B-Instruct-Q4_K_M.gguf" \
    "qwen2.5-coder-1.5b-instruct-q4_k_m.gguf" \
    "qwen2.5-coder-0.5b-instruct-q8_0.gguf"; do
    if [ ! -f "$MODEL_DIR/$model" ]; then
        echo ""
        echo "  ❌ Missing: $model"
        MISSING=1
    fi
done
if [ $MISSING -eq 0 ]; then
    echo "✅ All models present"
else
    exit 1
fi

# Check Python environment
echo -n "Checking Python environment... "
if [ -d "$HOME/.venvs/entropi" ]; then
    echo "✅ Virtual environment exists"
else
    echo "❌ Virtual environment not found"
    exit 1
fi

# Check llama-cpp-python
echo -n "Checking llama-cpp-python... "
source ~/.venvs/entropi/bin/activate
if python -c "from llama_cpp import Llama" 2>/dev/null; then
    echo "✅ llama-cpp-python installed"
else
    echo "❌ llama-cpp-python not working"
    exit 1
fi

# Check project structure
echo -n "Checking project structure... "
if [ -d "$HOME/projects/entropi/src/entropi" ]; then
    echo "✅ Project structure exists"
else
    echo "❌ Project structure not found"
    exit 1
fi

# Check Claude Code
echo -n "Checking Claude Code... "
if command -v claude > /dev/null 2>&1; then
    echo "✅ Claude Code installed"
else
    echo "❌ Claude Code not found"
    exit 1
fi

echo ""
echo "═══════════════════════════════════════════════════════════════"
echo "                    ALL CHECKS PASSED ✅                        "
echo "═══════════════════════════════════════════════════════════════"
echo ""
echo "Ready to start development!"
echo ""
echo "Next steps:"
echo "  cd ~/projects/entropi"
echo "  source ~/.venvs/entropi/bin/activate"
echo "  claude"
echo ""
```

Save this as `~/verify_entropi_prework.sh` and run:
```bash
chmod +x ~/verify_entropi_prework.sh
~/verify_entropi_prework.sh
```

---

## 9. Summary Checklist

### Hardware & OS
- [ ] Ubuntu 24.04 LTS installed
- [ ] NVIDIA RTX PRO 4000 (16GB VRAM) working
- [ ] 64GB RAM available
- [ ] 50GB+ free disk space

### Models (~15 GB)
- [ ] 14B model downloaded
- [ ] 7B model downloaded
- [ ] 1.5B model downloaded
- [ ] 0.5B model downloaded

### System
- [ ] CUDA Toolkit 12.4+ installed
- [ ] Docker installed and configured
- [ ] Build dependencies installed

### Python
- [ ] Python 3.11+ available
- [ ] Virtual environment created
- [ ] llama-cpp-python with CUDA working
- [ ] Test inference successful
- [ ] All dependencies installed

### Project
- [ ] Project directory structure created
- [ ] ENTROPI.md created
- [ ] pyproject.toml created
- [ ] Pre-commit configured
- [ ] Git initialized with initial commit

### Tools
- [ ] Claude Code installed

### Verification
- [ ] Verification script passes all checks

---

## 10. Troubleshooting

### llama-cpp-python CUDA build fails

**Symptoms:** `CMAKE_ARGS="-DGGML_CUDA=on" pip install ...` fails

**Solutions:**
1. Ensure CUDA toolkit version matches driver:
   ```bash
   nvidia-smi  # Shows driver CUDA version
   nvcc --version  # Shows toolkit version
   ```
2. Try without architecture flag:
   ```bash
   CMAKE_ARGS="-DGGML_CUDA=on" pip install llama-cpp-python
   ```
3. Install from source:
   ```bash
   pip install llama-cpp-python --no-binary llama-cpp-python
   ```

### Model loading OOM

**Symptoms:** Out of memory when loading model

**Solutions:**
1. Ensure no other GPU processes running: `nvidia-smi`
2. Start with smaller model (0.5B) for testing
3. Reduce context size in test

### Docker permission denied

**Symptoms:** `docker: permission denied`

**Solutions:**
1. Ensure user is in docker group: `groups`
2. Log out and back in after adding to group
3. Or run: `newgrp docker`

### huggingface-cli download fails

**Symptoms:** Authentication error or rate limit

**Solutions:**
1. Login to Hugging Face (free account):
   ```bash
   huggingface-cli login
   ```
2. Use `--resume-download` for interrupted downloads

---

## Ready to Start?

Once all checkboxes are complete and the verification script passes:

```bash
cd ~/projects/entropi
source ~/.venvs/entropi/bin/activate
claude
```

Then tell Claude Code:
> "Read ENTROPI.md and the implementation documents in ~/entropi_docs/. Let's start with implementation document 01: Foundation."
