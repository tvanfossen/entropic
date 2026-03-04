# Configuration

> How to configure Entropic

## Configuration Hierarchy

Configuration is loaded from multiple sources, with later sources overriding earlier ones:

1. **Defaults** - Built into the application
2. **Global config** - `~/.entropic/config.yaml`
3. **Project config** - `.entropic/config.yaml`
4. **Local config** - `.entropic/config.local.yaml` (gitignored)
5. **Environment variables** - `ENTROPIC_*`
6. **CLI arguments**

## Configuration File

### Full Example

```yaml
# ~/.entropic/config.yaml

models:
  tiers:
    thinking:
      path: ~/models/gguf/Qwen_Qwen3-14B-Q4_K_M.gguf
      adapter: qwen3
      context_length: 16384
      gpu_layers: -1
    normal:
      path: ~/models/gguf/Falcon-H1R-7B-Q8_0.gguf
      adapter: falcon
      context_length: 32768
      gpu_layers: -1
    analyzer:
      path: ~/models/gguf/Qwen3-8B-Q4_K_M.gguf
      adapter: qwen3
      identity: prompts/identity_analyzer.md    # Per-tier system prompt
      grammar: data/grammars/analysis.gbnf      # GBNF output constraint
      auto_chain: true                          # Chain to next tier on completion
      enable_thinking: false                    # Suppress think blocks
      max_output_tokens: 256
      allowed_tools:
        - entropic.todo_write
    micro:
      path: ~/models/gguf/Qwen3-0.6B-Q8_0.gguf
      adapter: qwen3
      context_length: 4096
      gpu_layers: -1
  default: normal

thinking:
  enabled: false  # Start in normal mode

routing:
  enabled: true
  fallback_tier: normal

generation:
  max_tokens: 4096
  default_temperature: 0.7
  default_top_p: 0.9

permissions:
  allow:
    - "filesystem.*"
    - "git.*"
  deny:
    - "bash.execute:rm -rf *"
    - "bash.execute:sudo *"

log_level: INFO
```

## Configuration Options

### Models

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `models.tiers.*.path` | Path | - | Path to GGUF model file |
| `models.tiers.*.adapter` | String | `qwen2` | Adapter type: `qwen3`, `qwen2`, `falcon`, `generic` |
| `models.tiers.*.context_length` | Integer | 16384 | Maximum context window |
| `models.tiers.*.max_output_tokens` | Integer | 4096 | Max tokens per generation |
| `models.tiers.*.gpu_layers` | Integer | -1 | GPU layers (-1 = all) |
| `models.tiers.*.warm_on_startup` | Boolean | false | Pre-load model to CPU RAM at startup (WARM state) |
| `models.tiers.*.use_mlock` | Boolean | true | Lock model pages in RAM (prevents swap, faster GPU promotion) |
| `models.tiers.*.identity` | Path/False/None | None | Per-tier system prompt file. None=bundled, False=disabled |
| `models.tiers.*.grammar` | Path/None | None | Path to `.gbnf` grammar file for output constraints |
| `models.tiers.*.auto_chain` | Boolean | false | Chain to next tier on completion |
| `models.tiers.*.enable_thinking` | Boolean | true | Allow think blocks. False adds `/no-think` (Qwen3) |
| `models.tiers.*.allowed_tools` | List/None | None | Tool visibility filter (`server.tool` format). None=all |
| `models.tiers.*.temperature` | Float | 0.7 | Sampling temperature |
| `models.tiers.*.top_p` | Float | 0.9 | Top-p sampling |
| `models.default` | String | normal | Default model tier |
| `models.router` | Object | None | Router model config (not a tier) |

### Routing

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `routing.enabled` | Boolean | true | Enable automatic task routing |
| `routing.fallback_tier` | String | normal | Fallback when routing fails |

### Generation

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `generation.max_tokens` | Integer | 4096 | Max tokens per response |
| `generation.default_temperature` | Float | 0.7 | Sampling temperature |
| `generation.default_top_p` | Float | 0.9 | Top-p sampling |

### VRAM

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `library.vram_reserve_mb` | Integer | 512 | VRAM headroom to keep free (MB). Informs load decisions. |

### MCP

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `mcp.external.socket_path` | Path/None | None | Unix socket path for the external MCP server. None = auto-derived from project directory (`~/.entropic/socks/{hash(cwd)}.sock`). |

External MCP servers (from `.mcp.json` or `mcp.external_servers`) are connected at `initialize()` time. See [Runtime MCP Registration](#runtime-mcp-registration) in the library consumer guide for the `connect_server()` API.

### Logging

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `log_level` | String | INFO | Log level: DEBUG, INFO, WARNING, ERROR |

## Environment Variables

All configuration options can be set via environment variables with the `ENTROPIC_` prefix:

```bash
export ENTROPIC_LOG_LEVEL=DEBUG
export ENTROPIC_MODELS__NORMAL__CONTEXT_LENGTH=16384
```

Use double underscores (`__`) for nested options.

## Project-Specific Configuration

Create `.entropic/config.yaml` in your project root for project-specific settings:

```yaml
# .entropic/config.yaml

# Override context length for this project
models:
  normal:
    context_length: 8192

# Project-specific permissions
permissions:
  allow:
    - "filesystem.*"
    - "git.*"
    - "bash.execute:pytest *"
    - "bash.execute:make *"
```

## Local Overrides

For settings you don't want to commit (like local paths), use `.entropic/config.local.yaml`:

```yaml
# .entropic/config.local.yaml (gitignored)

models:
  normal:
    path: /custom/path/to/model.gguf
```

## Adapters

Adapters handle model-specific chat formats and tool calling:

| Adapter | Models | Format |
|---------|--------|--------|
| `qwen3` | Qwen3-*, Qwen2.5-* | ChatML with `<tool_call>` tags |
| `falcon` | Falcon-H1R-* | ChatML with `<tool_call>` tags |
| `generic` | Any | Basic ChatML |
