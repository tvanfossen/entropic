"""
Session management for project-scoped conversations.

Provides project isolation with separate histories per repository.
"""

import hashlib
import json
import uuid
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Any

from entropi.core.base import Message
from entropi.core.logging import get_logger
from entropi.storage.database import Database

logger = get_logger("storage.session")


def get_project_id(path: Path) -> str:
    """
    Get unique project identifier.

    Uses git root if in a git repo, otherwise the absolute path.
    Hashes to create filesystem-safe directory name.

    Args:
        path: Project path

    Returns:
        16-character hex hash of project path
    """
    git_root = find_git_root(path)
    project_path = git_root if git_root else path.resolve()
    return hashlib.sha256(str(project_path).encode()).hexdigest()[:16]


def find_git_root(path: Path) -> Path | None:
    """
    Find git repository root.

    Args:
        path: Starting path

    Returns:
        Git root path or None
    """
    current = path.resolve()
    while current != current.parent:
        if (current / ".git").exists():
            return current
        current = current.parent
    return None


@dataclass
class Session:
    """A conversation session within a project."""

    id: str
    project_id: str
    name: str
    messages: list[Message] = field(default_factory=list)
    created_at: datetime = field(default_factory=datetime.utcnow)
    updated_at: datetime = field(default_factory=datetime.utcnow)
    metadata: dict[str, Any] = field(default_factory=dict)
    compaction_count: int = 0
    message_count: int = 0


@dataclass
class Project:
    """A project (repository) with its sessions."""

    id: str
    path: Path
    name: str
    active_session_id: str | None = None
    created_at: datetime = field(default_factory=datetime.utcnow)
    last_accessed: datetime = field(default_factory=datetime.utcnow)


# Session-specific migrations
SESSION_MIGRATIONS = {
    "001_sessions": """
        -- Sessions table (replaces conversations for project-scoped storage)
        CREATE TABLE IF NOT EXISTS sessions (
            id TEXT PRIMARY KEY,
            name TEXT NOT NULL,
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            message_count INTEGER DEFAULT 0,
            token_count INTEGER DEFAULT 0,
            compaction_count INTEGER DEFAULT 0,
            metadata TEXT DEFAULT '{}'
        );

        -- Messages within sessions
        CREATE TABLE IF NOT EXISTS session_messages (
            id TEXT PRIMARY KEY,
            session_id TEXT NOT NULL,
            role TEXT NOT NULL CHECK(role IN ('user', 'assistant', 'system', 'tool')),
            content TEXT NOT NULL,
            tool_calls TEXT DEFAULT '[]',
            tool_results TEXT DEFAULT '[]',
            token_count INTEGER DEFAULT 0,
            model_used TEXT,
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            FOREIGN KEY (session_id) REFERENCES sessions(id) ON DELETE CASCADE
        );

        CREATE INDEX IF NOT EXISTS idx_session_messages_session
            ON session_messages(session_id, created_at);
    """,
}

# Global project registry migrations
PROJECT_MIGRATIONS = {
    "001_projects": """
        CREATE TABLE IF NOT EXISTS projects (
            id TEXT PRIMARY KEY,
            path TEXT NOT NULL UNIQUE,
            name TEXT NOT NULL,
            active_session_id TEXT,
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            last_accessed TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        );

        CREATE INDEX IF NOT EXISTS idx_projects_last_accessed
            ON projects(last_accessed DESC);
    """,
}


class SessionManager:
    """Manages sessions for a project."""

    def __init__(
        self,
        project_path: Path,
        entropi_dir: Path | None = None,
    ) -> None:
        """
        Initialize session manager.

        Args:
            project_path: Project directory path
            entropi_dir: Entropi config directory (default: ~/.entropi)
        """
        self.project_path = project_path.resolve()
        self.project_id = get_project_id(project_path)
        self.project_name = self.project_path.name

        # Storage paths
        self.entropi_dir = entropi_dir or (Path.home() / ".entropi")
        self.global_db_path = self.entropi_dir / "global.db"
        self.project_dir = self.entropi_dir / "projects" / self.project_id
        self.project_db_path = self.project_dir / "history.db"

        self._current_session: Session | None = None
        self._global_db: Database | None = None
        self._project_db: Database | None = None

    @property
    def current_session(self) -> Session:
        """Get current session."""
        if not self._current_session:
            raise RuntimeError("Session not initialized. Call initialize() first.")
        return self._current_session

    @property
    def messages(self) -> list[Message]:
        """Get current session messages."""
        return self.current_session.messages

    async def initialize(self) -> None:
        """Initialize storage and load/create session."""
        # Ensure directories exist
        self.project_dir.mkdir(parents=True, exist_ok=True)

        # Initialize databases
        self._global_db = Database(self.global_db_path)
        await self._global_db.initialize()
        await self._run_project_migrations()

        self._project_db = Database(self.project_db_path)
        await self._project_db.initialize()
        await self._run_session_migrations()

        # Register/update project in global db
        await self._register_project()

        # Load or create session
        await self._load_or_create_session()

        logger.info(f"Session manager initialized for project: {self.project_name}")

    async def _run_project_migrations(self) -> None:
        """Run project registry migrations."""
        assert self._global_db is not None

        # Create migrations table if needed
        await self._global_db.execute(
            """
            CREATE TABLE IF NOT EXISTS migrations (
                id INTEGER PRIMARY KEY,
                name TEXT UNIQUE,
                applied_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
            )
        """
        )

        # Get applied migrations
        rows = await self._global_db.fetchall("SELECT name FROM migrations")
        applied = {row["name"] for row in rows}

        # Run pending migrations
        for name, sql in PROJECT_MIGRATIONS.items():
            if name not in applied:
                logger.debug(f"Running global migration: {name}")
                # Execute via connection directly for multi-statement
                async with self._global_db._get_connection() as conn:
                    await conn.executescript(sql)
                    await conn.execute("INSERT INTO migrations (name) VALUES (?)", (name,))
                    await conn.commit()

    async def _run_session_migrations(self) -> None:
        """Run session-specific migrations."""
        assert self._project_db is not None

        # Create migrations table if needed
        await self._project_db.execute(
            """
            CREATE TABLE IF NOT EXISTS migrations (
                id INTEGER PRIMARY KEY,
                name TEXT UNIQUE,
                applied_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
            )
        """
        )

        # Get applied migrations
        rows = await self._project_db.fetchall("SELECT name FROM migrations")
        applied = {row["name"] for row in rows}

        # Run pending migrations
        for name, sql in SESSION_MIGRATIONS.items():
            if name not in applied:
                logger.debug(f"Running session migration: {name}")
                async with self._project_db._get_connection() as conn:
                    await conn.executescript(sql)
                    await conn.execute("INSERT INTO migrations (name) VALUES (?)", (name,))
                    await conn.commit()

    async def _register_project(self) -> None:
        """Register this project in global database."""
        assert self._global_db is not None

        await self._global_db.execute(
            """
            INSERT INTO projects (id, path, name, last_accessed)
            VALUES (?, ?, ?, CURRENT_TIMESTAMP)
            ON CONFLICT(id) DO UPDATE SET last_accessed = CURRENT_TIMESTAMP
            """,
            (self.project_id, str(self.project_path), self.project_name),
        )

    async def _load_or_create_session(self) -> None:
        """Load most recent session or create new one."""
        # Check for active session
        active_id = await self._get_active_session_id()

        if active_id:
            try:
                self._current_session = await self._load_session(active_id)
                logger.debug(f"Loaded session: {self._current_session.name}")
                return
            except Exception as e:
                logger.warning(f"Failed to load active session: {e}")

        # Create new session
        self._current_session = await self.create_session("Initial session")

    async def _get_active_session_id(self) -> str | None:
        """Get the active session ID for this project."""
        assert self._global_db is not None

        row = await self._global_db.fetchone(
            "SELECT active_session_id FROM projects WHERE id = ?",
            (self.project_id,),
        )
        return row["active_session_id"] if row else None

    async def _load_session(self, session_id: str) -> Session:
        """Load a session with its messages."""
        assert self._project_db is not None

        row = await self._project_db.fetchone(
            "SELECT * FROM sessions WHERE id = ?",
            (session_id,),
        )

        if not row:
            raise ValueError(f"Session not found: {session_id}")

        msg_rows = await self._project_db.fetchall(
            "SELECT * FROM session_messages WHERE session_id = ? ORDER BY created_at",
            (session_id,),
        )

        messages = []
        for m in msg_rows:
            messages.append(
                Message(
                    role=m["role"],
                    content=m["content"],
                    tool_calls=json.loads(m["tool_calls"]) if m["tool_calls"] else None,
                    tool_results=json.loads(m["tool_results"]) if m["tool_results"] else None,
                )
            )

        return Session(
            id=row["id"],
            project_id=self.project_id,
            name=row["name"],
            messages=messages,
            created_at=(
                datetime.fromisoformat(row["created_at"])
                if isinstance(row["created_at"], str)
                else row["created_at"]
            ),
            updated_at=(
                datetime.fromisoformat(row["updated_at"])
                if isinstance(row["updated_at"], str)
                else row["updated_at"]
            ),
            metadata=json.loads(row["metadata"]) if row["metadata"] else {},
            compaction_count=row["compaction_count"] or 0,
            message_count=row["message_count"] or 0,
        )

    async def create_session(self, name: str) -> Session:
        """
        Create a new session.

        Args:
            name: Session name

        Returns:
            New session
        """
        assert self._project_db is not None

        session_id = str(uuid.uuid4())
        now = datetime.utcnow()

        await self._project_db.execute(
            """
            INSERT INTO sessions (id, name, created_at, updated_at)
            VALUES (?, ?, ?, ?)
            """,
            (session_id, name, now.isoformat(), now.isoformat()),
        )

        session = Session(
            id=session_id,
            project_id=self.project_id,
            name=name,
            messages=[],
            created_at=now,
            updated_at=now,
        )

        # Set as active
        await self._set_active_session(session_id)
        self._current_session = session

        logger.info(f"Created session: {name}")
        return session

    async def switch_session(self, session_id: str) -> Session:
        """
        Switch to a different session.

        Args:
            session_id: Session ID to switch to

        Returns:
            Loaded session
        """
        session = await self._load_session(session_id)
        await self._set_active_session(session_id)
        self._current_session = session
        logger.info(f"Switched to session: {session.name}")
        return session

    async def _set_active_session(self, session_id: str) -> None:
        """Set the active session for this project."""
        assert self._global_db is not None

        await self._global_db.execute(
            "UPDATE projects SET active_session_id = ? WHERE id = ?",
            (session_id, self.project_id),
        )

    async def add_message(self, message: Message) -> None:
        """
        Add a message to the current session.

        Args:
            message: Message to add
        """
        assert self._project_db is not None

        message_id = str(uuid.uuid4())

        await self._project_db.execute(
            """
            INSERT INTO session_messages
                (id, session_id, role, content, tool_calls, tool_results, token_count)
            VALUES (?, ?, ?, ?, ?, ?, ?)
            """,
            (
                message_id,
                self.current_session.id,
                message.role,
                message.content,
                json.dumps(message.tool_calls) if message.tool_calls else "[]",
                json.dumps(message.tool_results) if message.tool_results else "[]",
                len(message.content) // 4,  # Rough token estimate
            ),
        )

        self.current_session.messages.append(message)

        # Update session timestamp and count
        await self._project_db.execute(
            """
            UPDATE sessions
            SET updated_at = CURRENT_TIMESTAMP, message_count = message_count + 1
            WHERE id = ?
            """,
            (self.current_session.id,),
        )

    async def add_messages(self, messages: list[Message]) -> None:
        """
        Add multiple messages to the current session.

        Args:
            messages: Messages to add
        """
        for message in messages:
            await self.add_message(message)

    async def list_sessions(self) -> list[dict[str, Any]]:
        """
        List all sessions for this project.

        Returns:
            List of session info dicts
        """
        assert self._project_db is not None

        rows = await self._project_db.fetchall(
            """
            SELECT id, name, message_count, updated_at
            FROM sessions
            ORDER BY updated_at DESC
            """
        )

        return [
            {
                "id": r["id"],
                "name": r["name"],
                "message_count": r["message_count"] or 0,
                "updated_at": r["updated_at"],
                "is_current": r["id"] == self.current_session.id,
            }
            for r in rows
        ]

    async def rename_session(self, session_id: str, new_name: str) -> None:
        """
        Rename a session.

        Args:
            session_id: Session ID
            new_name: New name
        """
        assert self._project_db is not None

        await self._project_db.execute(
            "UPDATE sessions SET name = ? WHERE id = ?",
            (new_name, session_id),
        )

        if self.current_session.id == session_id:
            self.current_session.name = new_name

        logger.info(f"Renamed session to: {new_name}")

    async def delete_session(self, session_id: str) -> None:
        """
        Delete a session.

        Args:
            session_id: Session ID to delete

        Raises:
            ValueError: If trying to delete current session
        """
        if session_id == self.current_session.id:
            raise ValueError("Cannot delete current session. Switch to another session first.")

        assert self._project_db is not None

        await self._project_db.execute(
            "DELETE FROM sessions WHERE id = ?",
            (session_id,),
        )

        logger.info(f"Deleted session: {session_id}")

    async def export_session(self, session_id: str) -> str:
        """
        Export session to markdown.

        Args:
            session_id: Session ID to export

        Returns:
            Markdown content
        """
        session = await self._load_session(session_id)

        lines = [
            f"# {session.name}",
            "",
            f"**Project:** {self.project_name}",
            f"**Created:** {session.created_at}",
            f"**Messages:** {len(session.messages)}",
            "",
            "---",
            "",
        ]

        for msg in session.messages:
            role = "**User:**" if msg.role == "user" else "**Assistant:**"
            if msg.role == "tool":
                role = "**Tool:**"
            elif msg.role == "system":
                role = "**System:**"

            lines.append(role)
            lines.append("")
            lines.append(msg.content)
            lines.append("")
            lines.append("---")
            lines.append("")

        return "\n".join(lines)

    async def clear_messages(self) -> None:
        """Clear all messages from current session."""
        assert self._project_db is not None

        await self._project_db.execute(
            "DELETE FROM session_messages WHERE session_id = ?",
            (self.current_session.id,),
        )

        await self._project_db.execute(
            "UPDATE sessions SET message_count = 0 WHERE id = ?",
            (self.current_session.id,),
        )

        self.current_session.messages = []
        logger.info("Cleared session messages")

    async def update_messages(self, messages: list[Message]) -> None:
        """
        Replace current session messages (used after compaction).

        Args:
            messages: New message list
        """
        await self.clear_messages()
        await self.add_messages(messages)

    async def close(self) -> None:
        """Close database connections."""
        if self._project_db:
            await self._project_db.close()
        if self._global_db:
            await self._global_db.close()

    def get_session_summary(self) -> dict[str, Any]:
        """
        Get a summary of the current session for display.

        Returns:
            Session summary dict
        """
        session = self.current_session
        last_message = session.messages[-1].content[:100] if session.messages else None

        return {
            "project_name": self.project_name,
            "project_path": str(self.project_path),
            "session_name": session.name,
            "session_id": session.id,
            "message_count": len(session.messages),
            "last_message_preview": last_message,
        }


class RecentProjects:
    """Track recently accessed projects."""

    def __init__(self, global_db: Database) -> None:
        """
        Initialize recent projects tracker.

        Args:
            global_db: Global database instance
        """
        self._db = global_db

    async def get_recent(self, limit: int = 10) -> list[dict[str, Any]]:
        """
        Get recently accessed projects.

        Args:
            limit: Maximum number to return

        Returns:
            List of project info dicts
        """
        rows = await self._db.fetchall(
            """
            SELECT id, path, name, last_accessed
            FROM projects
            ORDER BY last_accessed DESC
            LIMIT ?
            """,
            (limit,),
        )
        return [dict(r) for r in rows]

    async def get_project_info(self, project_id: str) -> dict[str, Any] | None:
        """
        Get info for a specific project.

        Args:
            project_id: Project ID

        Returns:
            Project info dict or None
        """
        row = await self._db.fetchone(
            "SELECT * FROM projects WHERE id = ?",
            (project_id,),
        )
        return dict(row) if row else None
