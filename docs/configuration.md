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

### Bundled Default

The bundled default config ships at
[`src/entropic/data/default_config.yaml`](../src/entropic/data/default_config.yaml)
and is loaded automatically. It defines all 10 tiers pointing to the same model
file with 131072 context length.

To customize, create an override file — only set what differs from the default:

### Local Override Example

```yaml
# .entropic/config.local.yaml — only override model paths
models:
  lead:
    path: ~/models/gguf/Qwen3.5-35B-A3B-Q4_K_M.gguf
```

### Project Permission Example

```yaml
# .entropic/config.yaml — project-specific permissions
permissions:
  allow:
    - "filesystem.*"
    - "git.*"
  deny:
    - "bash.execute:rm -rf *"
    - "bash.execute:sudo *"
```

**Note:** Multiple tiers can share the same model file. Identity swaps are
free (prompt changes only). Model swaps (different GGUF files) cost VRAM
load/unload time.

## Configuration Options

### Models — Hardware Config (`ModelConfig`)

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `models.tiers.*.path` | Path | required | Path to GGUF model file |
| `models.tiers.*.adapter` | str | `qwen2` | Adapter: qwen35, qwen3, qwen2, falcon, smollm3, router, generic |
| `models.tiers.*.context_length` | int | 16384 | Maximum context window (512–131072) |
| `models.tiers.*.gpu_layers` | int | -1 | GPU layers (-1 = all) |
| `models.tiers.*.keep_warm` | bool | false | Pre-warm at startup, deactivate (not unload) on swap |
| `models.tiers.*.use_mlock` | bool | true | Lock model pages in RAM (prevents swap, faster activation) |
| `models.tiers.*.logits_all` | bool | false | Compute logits for all positions (needed for logprobs) |
| `models.tiers.*.allowed_tools` | list/null | null | Tool visibility filter (`server.tool` format). null = all |
| `models.default` | str | `lead` | Default model tier |
| `models.router` | object/null | null | Router model config (optional, not a tier) |

### Models — Tier Overrides (`TierConfig`)

These override identity frontmatter defaults when set in config:

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `models.tiers.*.identity` | path/false/null | null | Custom identity file. null = bundled, false = disabled |
| `models.tiers.*.grammar` | path/null | null | Path to `.gbnf` grammar file for output constraints |
| `models.tiers.*.auto_chain` | bool/null | null | Override identity `auto_chain`. null = defer to frontmatter |
| `models.tiers.*.routable` | bool/null | null | Override identity `routable` flag |

### Identity Frontmatter — Inference Config

Inference parameters live in identity frontmatter (`data/prompts/identity_*.md`),
not in config. This keeps config focused on hardware and identity on behavior.

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `temperature` | float | 0.7 | Sampling temperature |
| `max_output_tokens` | int | 1024 | Max tokens per generation |
| `repeat_penalty` | float | 1.1 | Repetition penalty |
| `enable_thinking` | bool | false | Allow `<think>` blocks |
| `bash_commands` | list/null | null | Bash command allowlist (null = all, [] = none) |
| `explicit_completion` | bool | false | Require `entropic.complete` to terminate (vs heuristic) |
| `phases` | dict/null | null | Named phase configurations with per-phase overrides |

See [Models](models.md) for the full frontmatter schema.

### Routing

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `routing.enabled` | bool | false | Enable automatic task routing (requires router model) |
| `routing.fallback_tier` | str | `lead` | Fallback when routing fails |

### Generation

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `generation.max_tokens` | int | 4096 | Max tokens per response |
| `generation.default_temperature` | float | 0.7 | Sampling temperature |
| `generation.default_top_p` | float | 0.9 | Top-p sampling |

### VRAM

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `library.vram_reserve_mb` | int | 512 | VRAM headroom to keep free (MB) |

### MCP

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `mcp.external.socket_path` | path/null | null | Unix socket for external MCP. null = auto |

External MCP servers (`.mcp.json` or `mcp.external_servers`) connect at `initialize()` time.

### Logging

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `log_level` | str | INFO | Log level: DEBUG, INFO, WARNING, ERROR |

## Environment Variables

All options can be set via environment variables with the `ENTROPIC_` prefix:

```bash
export ENTROPIC_LOG_LEVEL=DEBUG
export ENTROPIC_MODELS__TIERS__LEAD__CONTEXT_LENGTH=16384
```

Use double underscores (`__`) for nested options.

## Project-Specific Configuration

Create `.entropic/config.yaml` in your project root:

```yaml
# .entropic/config.yaml
permissions:
  allow:
    - "filesystem.*"
    - "git.*"
    - "bash.execute:pytest *"
    - "bash.execute:make *"
```

## Local Overrides

For settings you don't want to commit, use `.entropic/config.local.yaml`:

```yaml
# .entropic/config.local.yaml (gitignored)
models:
  tiers:
    lead:
      path: /custom/path/to/model.gguf
```

## Adapters

| Adapter | Models | Format |
|---------|--------|--------|
| `qwen35` | Qwen3.5-35B-A3B MoE | ChatML with `<tool_call>` tags, `<think>` blocks |
| `qwen3` | Qwen3-* | ChatML with `<tool_call>` tags, `<think>` blocks |
| `qwen2` | Qwen2.5-* | ChatML with `<tool_call>` tags |
| `falcon` | Falcon-H1R-* | ChatML with `<tool_call>` tags, `<think>` blocks |
| `smollm3` | SmolLM3-* | ChatML with `<tool_call>` tags |
| `router` | Classification | Raw text continuation |
| `generic` | Any | Basic ChatML |
