# Getting Started

> From zero to running inference in under 10 minutes

## Prerequisites

- Linux (tested on Ubuntu 24.04)
- cmake 3.21+, C++20 compiler (gcc 10+ or clang 14+)
- NVIDIA GPU with 16GB+ VRAM (or CPU-only for smaller models)
- Python 3.10+ (for build tools and wrapper)
- ~15 GB disk space for the primary model

## Step 1: Clone and Build

```bash
git clone --recurse-submodules https://github.com/tvanfossen/entropic.git
cd entropic

python3 -m venv .venv
.venv/bin/pip install -e .
.venv/bin/pip install invoke pre-commit
.venv/bin/pre-commit install

# CUDA build (default, recommended)
inv build --clean

# Or CPU-only (no GPU required, slower inference)
inv build --cpu
```

## Step 2: Download a Model

```bash
# Recommended (13.1 GB, ~35 tok/s on 16GB GPU)
entropic download primary

# Smaller alternatives:
entropic download mid          # 9.5 GB, 12GB+ VRAM
entropic download lightweight  # 4.5 GB, 8GB+ VRAM
```

Models download to `~/models/gguf/`. The engine resolves `path: primary` in
config to the full path automatically.

## Step 3: Run the Hello World Example

```bash
inv example -n hello-world
```

This builds the C example, configures the engine from
`examples/hello-world/default_config.yaml`, loads the model, and starts an
interactive streaming chat. Type a message and press Enter. Type `quit` to exit.

Session logs appear in `examples/hello-world/.hello-world/`:
- `session.log` — engine operations
- `session_model.log` — full user/assistant exchanges

## Step 4: Run the Chess Example

```bash
inv example -n pychess
```

This demonstrates the multi-tier pipeline:
1. **Thinker tier** analyzes the position (grammar-constrained output)
2. Thinker delegates to **executor tier** via `entropic.delegate`
3. Executor calls `chess.make_move` on the external chess MCP server
4. Executor calls `entropic.complete` to signal it's done
5. Move is applied to the board

You play as White (UCI notation: `e2e4`). The AI plays as Black.

## Step 5: Run Tests

```bash
# CPU unit + regression tests (673 tests, ~2 seconds)
inv test --cpu --no-build

# Model tests (GPU required, ~3 minutes)
inv test --model --no-build
```

## What's Next?

- **Build your own app**: See [Library Consumer Guide](library-consumer-guide.md)
- **Understand the architecture**: See [Architecture](architecture-cpp.md)
- **Contribute**: See [CONTRIBUTING.md](../CONTRIBUTING.md)

## Troubleshooting

### "Model file not found"

The engine validates model paths at configure time. If the model isn't
downloaded, you'll see:

```
Model file not found for tier 'lead': /home/user/models/gguf/Qwen3.5-...
Download with: entropic download primary
```

### "Unknown server" for tool calls

Ensure the MCP server is enabled in your config:

```yaml
mcp:
  enable_entropic: true    # Required for delegation/completion tools
```

### Build fails with CUDA errors

Try CPU-only build first to verify the non-CUDA parts work:

```bash
inv build --cpu
inv test --cpu --no-build
```

### Session logs are empty

Ensure you're using `entropic_configure_dir()` (not `entropic_configure_from_file()`).
Only `configure_dir` sets up file logging automatically.
