# Commands

> Slash commands and keyboard shortcuts

## Slash Commands

### Session Management

| Command | Description |
|---------|-------------|
| `/sessions` | List all sessions |
| `/new [name]` | Create a new session |
| `/session <id>` | Switch to a session |
| `/rename <name>` | Rename current session |
| `/delete <id>` | Delete a session |
| `/export` | Export session as markdown |

### Conversation

| Command | Description |
|---------|-------------|
| `/clear` | Clear conversation history |
| `/exit` | Exit Entropic |
| `/help` | Show available commands |

### Model Control

| Command | Description |
|---------|-------------|
| `/think on` | Enable deep reasoning mode (14B model) |
| `/think off` | Disable deep reasoning mode (7B model) |
| `/think status` | Show current thinking mode |
| `/status` | Show model and VRAM status |

### Project

| Command | Description |
|---------|-------------|
| `/init` | Initialize Entropic in current directory |

## Keyboard Shortcuts

| Shortcut | Action |
|----------|--------|
| `Ctrl+C` | Cancel current operation / Exit |
| `Ctrl+D` | Exit (at empty prompt) |
| `Ctrl+T` | Toggle thinking mode |
| `Ctrl+L` | Clear screen |
| `Up/Down` | Navigate command history |

## CLI Commands

### entropic

Start interactive mode:

```bash
entropic
```

### entropic ask

Send a single message:

```bash
entropic ask "What is a linked list?"

# Pipe from stdin
cat question.txt | entropic ask

# Disable streaming
entropic ask --no-stream "Quick question"
```

### entropic status

Show system status:

```bash
entropic status
```

### entropic init

Initialize Entropic in a project:

```bash
cd /path/to/project
entropic init
```

Creates:
- `.entropic/config.yaml` - Project configuration
- `.entropic/commands/` - Custom commands directory
- `ENTROPIC.md` - Project context file

### entropic download

Download models:

```bash
entropic download --all
entropic download --model normal
```

### CLI Options

| Option | Description |
|--------|-------------|
| `--config, -c` | Path to configuration file |
| `--model, -m` | Model to use: thinking, normal, code, micro |
| `--log-level, -l` | Log level: DEBUG, INFO, WARNING, ERROR |
| `--project, -p` | Project directory |
| `--version` | Show version |
| `--help` | Show help |
