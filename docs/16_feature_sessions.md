# Feature Proposal: Multi-Session & Per-Repository History

> Project-scoped sessions with persistent history per repository

**Status:** Implemented (Core)
**Priority:** High (core UX feature)
**Complexity:** Medium
**Dependencies:** Storage layer

## Implementation Status

| Component | Status | Location |
|-----------|--------|----------|
| Session dataclass | Done | `src/entropi/storage/session.py` |
| Project identification | Done | `src/entropi/storage/session.py` |
| SessionManager | Done | `src/entropi/storage/session.py` |
| Database migrations | Done | `src/entropi/storage/session.py` |
| /sessions command | Done | `src/entropi/core/commands.py` |
| /session command | Done | `src/entropi/core/commands.py` |
| /new command | Done | `src/entropi/core/commands.py` |
| /rename command | Done | `src/entropi/core/commands.py` |
| /delete command | Done | `src/entropi/core/commands.py` |
| /export command | Done | `src/entropi/core/commands.py` |
| App integration | Pending | - |
| UI session list | Pending | - |

---

## Problem Statement

Developers work on multiple projects. Each project needs:

1. **Separate conversation history** — What I discussed in project A shouldn't appear in project B
2. **Project-specific context** — ENTROPI.md, .entropi/ config, tool permissions
3. **Session switching** — Work on project A, switch to B, come back to A where I left off
4. **Session persistence** — Close terminal, reopen, continue conversation

Current design has single global history. Need project-scoped isolation.

---

## Solution: Project-Scoped Sessions

### Core Concepts

| Concept | Description |
|---------|-------------|
| **Project** | A directory with code, identified by path (usually git root) |
| **Session** | A single conversation within a project |
| **Instance** | A running Entropi process (one per terminal) |

### Data Model

```
~/.entropi/
├── config.yaml              # Global config
├── global.db               # Global data (settings, recent projects)
└── projects/
    ├── {project-hash-1}/
    │   ├── config.yaml     # Project config override
    │   ├── history.db      # Project conversations
    │   └── sessions/
    │       ├── session-1.json
    │       └── session-2.json
    └── {project-hash-2}/
        ├── config.yaml
        ├── history.db
        └── sessions/
```

### Project Identification

```python
def get_project_id(path: Path) -> str:
    """
    Get unique project identifier.

    Uses git root if in a git repo, otherwise the absolute path.
    Hashes to create filesystem-safe directory name.
    """
    # Try to find git root
    git_root = find_git_root(path)
    if git_root:
        project_path = git_root
    else:
        project_path = path.resolve()

    # Create stable hash
    return hashlib.sha256(str(project_path).encode()).hexdigest()[:16]


def find_git_root(path: Path) -> Path | None:
    """Find git repository root."""
    current = path.resolve()
    while current != current.parent:
        if (current / ".git").exists():
            return current
        current = current.parent
    return None
```

---

## User Experience

### Starting Entropi

```bash
# In project A
cd ~/projects/chess-game
entropi

# Output:
╭─ Entropi ─────────────────────────────────────────────────────╮
│ Project: chess-game                                           │
│ Session: Continue previous (3 messages) or /new              │
╰───────────────────────────────────────────────────────────────╯
```

```bash
# In project B (different terminal)
cd ~/projects/web-api
entropi

# Output:
╭─ Entropi ─────────────────────────────────────────────────────╮
│ Project: web-api                                              │
│ Session: New session (first time)                             │
╰───────────────────────────────────────────────────────────────╯
```

### Session Commands

```
/sessions           List all sessions for this project
/session <id>       Switch to a specific session
/new                Start a new session (keep history)
/new --clear        Start fresh (archive current session)
/rename <name>      Rename current session
/delete <id>        Delete a session
/export <id>        Export session to markdown
```

### Example Session List

```
> /sessions

Sessions for chess-game:
┌─────┬────────────────────────────┬──────────────┬──────────┐
│ ID  │ Name                       │ Messages     │ Updated  │
├─────┼────────────────────────────┼──────────────┼──────────┤
│ → 1 │ Board implementation       │ 47 messages  │ 2 min ago│
│   2 │ Move validation            │ 23 messages  │ yesterday│
│   3 │ Initial planning           │ 12 messages  │ 3 days   │
└─────┴────────────────────────────┴──────────────┴──────────┘

→ = current session
```

### Switching Sessions

```
> /session 2

Loaded session: "Move validation" (23 messages)
Last message: "The en passant logic should check..."
```

---

## Implementation

### Session Model

```python
@dataclass
class Session:
    """A conversation session within a project."""

    id: str
    project_id: str
    name: str
    messages: list[Message]
    created_at: datetime
    updated_at: datetime
    metadata: dict[str, Any] = field(default_factory=dict)

    # Compaction history
    compaction_count: int = 0
    original_message_count: int = 0  # Before any compaction


@dataclass
class Project:
    """A project (repository) with its sessions."""

    id: str
    path: Path
    name: str  # Directory name for display
    sessions: list[Session]
    active_session_id: str | None
    created_at: datetime
    last_accessed: datetime
```

### Database Schema

```sql
-- ~/.entropi/global.db

-- Track known projects
CREATE TABLE projects (
    id TEXT PRIMARY KEY,              -- Hash of path
    path TEXT NOT NULL UNIQUE,        -- Absolute path
    name TEXT NOT NULL,               -- Display name
    active_session_id TEXT,           -- Last used session
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    last_accessed TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX idx_projects_last_accessed ON projects(last_accessed DESC);


-- ~/.entropi/projects/{project_id}/history.db

-- Sessions within this project
CREATE TABLE sessions (
    id TEXT PRIMARY KEY,
    name TEXT NOT NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    message_count INTEGER DEFAULT 0,
    token_count INTEGER DEFAULT 0,
    compaction_count INTEGER DEFAULT 0,
    metadata JSON
);

-- Messages within sessions
CREATE TABLE messages (
    id TEXT PRIMARY KEY,
    session_id TEXT REFERENCES sessions(id) ON DELETE CASCADE,
    role TEXT NOT NULL,
    content TEXT NOT NULL,
    token_count INTEGER,
    model_used TEXT,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX idx_messages_session ON messages(session_id, created_at);

-- Tool executions (for audit/replay)
CREATE TABLE tool_executions (
    id TEXT PRIMARY KEY,
    message_id TEXT REFERENCES messages(id) ON DELETE CASCADE,
    tool_name TEXT NOT NULL,
    arguments JSON,
    result TEXT,
    success BOOLEAN,
    duration_ms INTEGER,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- Compaction snapshots (for history recovery)
CREATE TABLE compaction_snapshots (
    id TEXT PRIMARY KEY,
    session_id TEXT REFERENCES sessions(id) ON DELETE CASCADE,
    messages JSON NOT NULL,
    token_count INTEGER,
    summary TEXT,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);
```

### Session Manager

```python
class SessionManager:
    """Manages sessions for a project."""

    def __init__(self, project_path: Path) -> None:
        self.project_path = project_path.resolve()
        self.project_id = get_project_id(project_path)
        self.project_name = project_path.name

        # Storage paths
        self.global_db_path = Path.home() / ".entropi" / "global.db"
        self.project_dir = Path.home() / ".entropi" / "projects" / self.project_id
        self.project_db_path = self.project_dir / "history.db"

        self._current_session: Session | None = None
        self._global_db: Database | None = None
        self._project_db: Database | None = None

    async def initialize(self) -> None:
        """Initialize storage and load/create session."""
        # Ensure directories exist
        self.project_dir.mkdir(parents=True, exist_ok=True)

        # Initialize databases
        self._global_db = await Database.connect(self.global_db_path)
        self._project_db = await Database.connect(self.project_db_path)

        # Ensure schema
        await self._ensure_schema()

        # Register/update project in global db
        await self._register_project()

        # Load or create session
        await self._load_or_create_session()

    async def _register_project(self) -> None:
        """Register this project in global database."""
        await self._global_db.execute("""
            INSERT INTO projects (id, path, name, last_accessed)
            VALUES (?, ?, ?, CURRENT_TIMESTAMP)
            ON CONFLICT(id) DO UPDATE SET last_accessed = CURRENT_TIMESTAMP
        """, (self.project_id, str(self.project_path), self.project_name))

    async def _load_or_create_session(self) -> None:
        """Load most recent session or create new one."""
        # Check for active session
        active_id = await self._get_active_session_id()

        if active_id:
            self._current_session = await self._load_session(active_id)
        else:
            self._current_session = await self.create_session("Initial session")

    async def _get_active_session_id(self) -> str | None:
        """Get the active session ID for this project."""
        row = await self._global_db.fetchone(
            "SELECT active_session_id FROM projects WHERE id = ?",
            (self.project_id,)
        )
        return row["active_session_id"] if row else None

    async def _load_session(self, session_id: str) -> Session:
        """Load a session with its messages."""
        row = await self._project_db.fetchone(
            "SELECT * FROM sessions WHERE id = ?", (session_id,)
        )

        messages = await self._project_db.fetchall(
            "SELECT * FROM messages WHERE session_id = ? ORDER BY created_at",
            (session_id,)
        )

        return Session(
            id=row["id"],
            project_id=self.project_id,
            name=row["name"],
            messages=[Message(**m) for m in messages],
            created_at=row["created_at"],
            updated_at=row["updated_at"],
            metadata=json.loads(row["metadata"] or "{}"),
            compaction_count=row["compaction_count"],
        )

    @property
    def current_session(self) -> Session:
        """Get current session."""
        if not self._current_session:
            raise RuntimeError("Session not initialized")
        return self._current_session

    async def create_session(self, name: str) -> Session:
        """Create a new session."""
        session_id = str(uuid.uuid4())

        await self._project_db.execute("""
            INSERT INTO sessions (id, name) VALUES (?, ?)
        """, (session_id, name))

        session = Session(
            id=session_id,
            project_id=self.project_id,
            name=name,
            messages=[],
            created_at=datetime.utcnow(),
            updated_at=datetime.utcnow(),
        )

        # Set as active
        await self._set_active_session(session_id)
        self._current_session = session

        return session

    async def switch_session(self, session_id: str) -> Session:
        """Switch to a different session."""
        session = await self._load_session(session_id)
        await self._set_active_session(session_id)
        self._current_session = session
        return session

    async def _set_active_session(self, session_id: str) -> None:
        """Set the active session for this project."""
        await self._global_db.execute(
            "UPDATE projects SET active_session_id = ? WHERE id = ?",
            (session_id, self.project_id)
        )

    async def add_message(self, message: Message) -> None:
        """Add a message to the current session."""
        await self._project_db.execute("""
            INSERT INTO messages (id, session_id, role, content, token_count, model_used)
            VALUES (?, ?, ?, ?, ?, ?)
        """, (message.id, self.current_session.id, message.role, message.content,
              message.token_count, message.model_used))

        self.current_session.messages.append(message)

        # Update session timestamp
        await self._project_db.execute(
            "UPDATE sessions SET updated_at = CURRENT_TIMESTAMP, message_count = message_count + 1 WHERE id = ?",
            (self.current_session.id,)
        )

    async def list_sessions(self) -> list[dict]:
        """List all sessions for this project."""
        rows = await self._project_db.fetchall("""
            SELECT id, name, message_count, updated_at
            FROM sessions
            ORDER BY updated_at DESC
        """)
        return [dict(r) for r in rows]

    async def rename_session(self, session_id: str, new_name: str) -> None:
        """Rename a session."""
        await self._project_db.execute(
            "UPDATE sessions SET name = ? WHERE id = ?",
            (new_name, session_id)
        )
        if self.current_session.id == session_id:
            self.current_session.name = new_name

    async def delete_session(self, session_id: str) -> None:
        """Delete a session."""
        if session_id == self.current_session.id:
            raise ValueError("Cannot delete current session")

        await self._project_db.execute(
            "DELETE FROM sessions WHERE id = ?", (session_id,)
        )

    async def export_session(self, session_id: str) -> str:
        """Export session to markdown."""
        session = await self._load_session(session_id)

        lines = [
            f"# {session.name}",
            f"",
            f"**Project:** {self.project_name}",
            f"**Created:** {session.created_at}",
            f"**Messages:** {len(session.messages)}",
            f"",
            "---",
            "",
        ]

        for msg in session.messages:
            role = "**User:**" if msg.role == "user" else "**Assistant:**"
            lines.append(role)
            lines.append("")
            lines.append(msg.content)
            lines.append("")
            lines.append("---")
            lines.append("")

        return "\n".join(lines)
```

### Recent Projects

```python
class RecentProjects:
    """Track recently accessed projects."""

    def __init__(self, global_db: Database) -> None:
        self._db = global_db

    async def get_recent(self, limit: int = 10) -> list[dict]:
        """Get recently accessed projects."""
        rows = await self._db.fetchall("""
            SELECT id, path, name, last_accessed
            FROM projects
            ORDER BY last_accessed DESC
            LIMIT ?
        """, (limit,))
        return [dict(r) for r in rows]

    async def get_project_info(self, project_id: str) -> dict | None:
        """Get info for a specific project."""
        row = await self._db.fetchone(
            "SELECT * FROM projects WHERE id = ?", (project_id,)
        )
        return dict(row) if row else None
```

---

## Commands Implementation

### /sessions Command

```python
class SessionsCommand(Command):
    """List sessions for current project."""

    @property
    def name(self) -> str:
        return "sessions"

    @property
    def description(self) -> str:
        return "List all sessions for this project"

    async def execute(self, args: str, context: CommandContext) -> CommandResult:
        sessions = await context.session_manager.list_sessions()

        if not sessions:
            return CommandResult(
                success=True,
                message="No sessions found. Current session is the first.",
            )

        return CommandResult(
            success=True,
            data={
                "action": "show_sessions",
                "sessions": sessions,
                "current_id": context.session_manager.current_session.id,
            },
        )
```

### /session Command

```python
class SessionCommand(Command):
    """Switch to a specific session."""

    @property
    def name(self) -> str:
        return "session"

    @property
    def usage(self) -> str:
        return "/session <id>"

    async def execute(self, args: str, context: CommandContext) -> CommandResult:
        if not args:
            return CommandResult(
                success=False,
                message="Usage: /session <id>",
            )

        session_id = args.strip()

        try:
            session = await context.session_manager.switch_session(session_id)
            return CommandResult(
                success=True,
                message=f"Switched to session: {session.name}",
                data={"action": "session_switched", "session": session},
            )
        except Exception as e:
            return CommandResult(
                success=False,
                message=f"Failed to switch session: {e}",
            )
```

### /new Command

```python
class NewCommand(Command):
    """Start a new session."""

    @property
    def name(self) -> str:
        return "new"

    @property
    def usage(self) -> str:
        return "/new [name] [--clear]"

    async def execute(self, args: str, context: CommandContext) -> CommandResult:
        # Parse args
        clear = "--clear" in args
        name = args.replace("--clear", "").strip() or "New session"

        session = await context.session_manager.create_session(name)

        return CommandResult(
            success=True,
            message=f"Created new session: {session.name}",
            data={"action": "new_session", "session": session},
        )
```

---

## Configuration

```yaml
# ~/.entropi/config.yaml
sessions:
  auto_save: true                    # Save messages immediately
  auto_resume: true                  # Resume last session on start
  max_sessions_per_project: 50       # Limit sessions per project
  archive_after_days: 30             # Auto-archive old sessions

  # Session naming
  auto_name: true                    # Auto-generate name from first message
  name_max_length: 50
```

---

## UI Integration

### Startup Banner

```
╭─ Entropi ─────────────────────────────────────────────────────╮
│ Project: chess-game                                           │
│ Path: ~/projects/chess-game                                   │
├───────────────────────────────────────────────────────────────┤
│ Session: "Board implementation" (47 messages)                 │
│ Last: "The FEN notation parser should..."                     │
├───────────────────────────────────────────────────────────────┤
│ Commands: /sessions /new /help                                │
╰───────────────────────────────────────────────────────────────╯
```

### Status Bar Project Indicator

```
╭─ Entropi ──────────────────────────────────────────────────────────────╮
│ ⚡ Normal │ chess-game │ Session: Board impl │ VRAM: 11.5/16 GB       │
╰────────────────────────────────────────────────────────────────────────╯
```

### Session Switcher Dialog (Ctrl+A)

```
╭─ Switch Session ───────────────────────────────────────────────╮
│                                                                │
│   → Board implementation          47 msgs    2 min ago        │
│     Move validation               23 msgs    yesterday        │
│     Initial planning              12 msgs    3 days           │
│                                                                │
│   [Enter] Select  [n] New  [d] Delete  [Esc] Cancel           │
╰────────────────────────────────────────────────────────────────╯
```

---

## Multiple Instances

Running multiple Entropi instances is safe:

```
Terminal 1:                    Terminal 2:
cd ~/chess-game               cd ~/web-api
entropi                       entropi

Project: chess-game           Project: web-api
Session: Board impl           Session: API design
```

Each instance:
- Has its own project context
- Writes to its own project database
- Maintains separate message history
- Can run simultaneously without conflicts

SQLite handles concurrent access with WAL mode.

---

## Cross-Project Features (Future)

### Project Switcher

```
> /project ~/web-api

Switching to project: web-api
Loading session: "API design"
```

### Global Search

```
> /search --all "authentication"

Results across all projects:

chess-game (2 matches):
  - Session "Initial planning": "...user authentication..."

web-api (5 matches):
  - Session "API design": "...JWT authentication..."
  - Session "Security review": "...OAuth authentication..."
```

### Project Dashboard (Future)

```bash
entropi dashboard

Recent Projects:
┌──────────────────┬────────────┬─────────────┬──────────────┐
│ Project          │ Sessions   │ Last Used   │ Path         │
├──────────────────┼────────────┼─────────────┼──────────────┤
│ chess-game       │ 3 sessions │ 2 min ago   │ ~/chess-game │
│ web-api          │ 5 sessions │ yesterday   │ ~/web-api    │
│ ml-pipeline      │ 2 sessions │ 1 week ago  │ ~/ml-project │
└──────────────────┴────────────┴─────────────┴──────────────┘
```

---

## Benefits Summary

| Feature | Benefit |
|---------|---------|
| **Project isolation** | No cross-contamination of context |
| **Session persistence** | Resume exactly where you left off |
| **Multiple sessions** | Different tasks within same project |
| **Multi-instance** | Work on multiple projects simultaneously |
| **History** | Full searchable history per project |
| **Export** | Share/archive sessions as markdown |
