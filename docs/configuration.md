# Configuration

> How to configure Entropi

## Configuration Hierarchy

Configuration is loaded from multiple sources, with later sources overriding earlier ones:

1. **Defaults** - Built into the application
2. **Global config** - `~/.entropi/config.yaml`
3. **Project config** - `.entropi/config.yaml`
4. **Local config** - `.entropi/config.local.yaml` (gitignored)
5. **Environment variables** - `ENTROPI_*`
6. **CLI arguments**

## Configuration File

### Full Example

```yaml
# ~/.entropi/config.yaml

models:
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
  code:
    path: ~/models/gguf/Falcon-H1R-7B-Q8_0.gguf
    adapter: falcon
    context_length: 32768
    gpu_layers: -1
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
  fallback_model: normal

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
| `models.thinking.path` | Path | - | Path to thinking model (14B) |
| `models.normal.path` | Path | - | Path to normal model (7B) |
| `models.code.path` | Path | - | Path to code model (7B) |
| `models.micro.path` | Path | - | Path to micro model (0.6B) |
| `models.*.adapter` | String | - | Adapter type: `qwen3`, `falcon`, `generic` |
| `models.*.context_length` | Integer | 16384 | Maximum context window |
| `models.*.gpu_layers` | Integer | -1 | GPU layers (-1 = all) |
| `models.default` | String | normal | Default model tier |

### Routing

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `routing.enabled` | Boolean | true | Enable automatic task routing |
| `routing.fallback_model` | String | normal | Fallback when routing fails |

### Generation

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `generation.max_tokens` | Integer | 4096 | Max tokens per response |
| `generation.default_temperature` | Float | 0.7 | Sampling temperature |
| `generation.default_top_p` | Float | 0.9 | Top-p sampling |

### Logging

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `log_level` | String | INFO | Log level: DEBUG, INFO, WARNING, ERROR |

## Environment Variables

All configuration options can be set via environment variables with the `ENTROPI_` prefix:

```bash
export ENTROPI_LOG_LEVEL=DEBUG
export ENTROPI_MODELS__NORMAL__CONTEXT_LENGTH=16384
```

Use double underscores (`__`) for nested options.

## Project-Specific Configuration

Create `.entropi/config.yaml` in your project root for project-specific settings:

```yaml
# .entropi/config.yaml

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

For settings you don't want to commit (like local paths), use `.entropi/config.local.yaml`:

```yaml
# .entropi/config.local.yaml (gitignored)

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
