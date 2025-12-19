# Entropi

Local AI coding assistant powered by Qwen models.

## Project Overview

Entropi is a terminal-based coding assistant that runs fully locally using quantized Qwen models and llama-cpp-python with CUDA acceleration. It is modeled after Claude Code but runs entirely on local hardware with no API costs and no data leaving your machine.

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

## Project Structure

```
entropi/
├── src/entropi/
│   ├── config/         # Configuration schema and loading
│   ├── core/           # Base classes, logging, agentic loop
│   ├── inference/      # Model backends and adapters
│   ├── mcp/            # MCP client and servers
│   ├── quality/        # Code quality enforcement
│   ├── storage/        # SQLite persistence
│   ├── ui/             # Terminal UI
│   └── prompts/        # Prompt templates
├── tests/
│   ├── unit/
│   ├── integration/
│   └── features/       # BDD tests
└── docs/               # Implementation documents
```

## Commands

```bash
# Activate environment
source ~/.venvs/entropi/bin/activate

# Install in development mode
pip install -e ".[dev]"

# Run in dev mode
python -m entropi

# Run tests
pytest tests/ -v

# Type check
mypy src/

# Format
black src/ tests/
ruff check src/ tests/

# Pre-commit
pre-commit run --all-files
```

## Architecture Principles

1. **KISS** — Keep it simple
2. **DRY** — Don't repeat yourself
3. **Modular** — Highly encapsulated components
4. **Configurable** — Everything is configurable
5. **MCP-First** — All tools via MCP protocol
6. **Quality Enforced** — Code quality checked at generation time

## Key Design Decisions

- Multi-model routing (14B/7B/1.5B/0.5B based on task complexity)
- Docker-only distribution for production
- Pre-commit enforcement matches generation-time enforcement
- BDD tests with pytest-bdd
- Agentic loop with plan→act→observe→repeat cycle

## Quality Rules

- Max cognitive complexity: 15
- Max cyclomatic complexity: 10
- Max function lines: 50
- Max parameters: 5
- Max returns per function: 3
- Required: type hints, docstrings, return types
- Docstring style: Google
