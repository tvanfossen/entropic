"""
Session manager with SQLite persistence.

Manages conversation state with support for multiple message sources
(human, claude-code) in a shared context.
"""

from __future__ import annotations

import json
import sqlite3
import time
import uuid
from contextlib import contextmanager
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterator

from entropi.core.base import Message
from entropi.core.logging import get_logger
from entropi.core.queue import MessageSource

logger = get_logger("core.session")

# Database schema version for migrations
SCHEMA_VERSION = 1


@dataclass
class SessionMessage:
    """A message in a session with source tracking."""

    id: str
    session_id: str
    role: str
    content: str
    source: str  # MessageSource value
    timestamp: float
    tool_calls: list[dict[str, Any]] = field(default_factory=list)
    tool_results: list[dict[str, Any]] = field(default_factory=list)
    metadata: dict[str, Any] = field(default_factory=dict)

    def to_message(self) -> Message:
        """Convert to Message for engine consumption."""
        return Message(
            role=self.role,
            content=self.content,
            tool_calls=self.tool_calls,
            tool_results=self.tool_results,
            metadata={**self.metadata, "source": self.source},
        )

    @classmethod
    def from_message(
        cls,
        message: Message,
        session_id: str,
        source: str = MessageSource.HUMAN,
    ) -> SessionMessage:
        """Create from a Message."""
        return cls(
            id=str(uuid.uuid4()),
            session_id=session_id,
            role=message.role,
            content=message.content,
            source=source,
            timestamp=time.time(),
            tool_calls=message.tool_calls,
            tool_results=message.tool_results,
            metadata=message.metadata,
        )


@dataclass
class Session:
    """A conversation session."""

    id: str
    created_at: float
    updated_at: float
    title: str = ""
    metadata: dict[str, Any] = field(default_factory=dict)

    @classmethod
    def create(cls, title: str = "") -> Session:
        """Create a new session."""
        now = time.time()
        return cls(
            id=str(uuid.uuid4()),
            created_at=now,
            updated_at=now,
            title=title,
        )


class SessionManager:
    """
    Manages conversation sessions with SQLite persistence.

    Features:
    - Save/load conversation state
    - Message source tracking (human vs claude-code)
    - History retrieval for MCP get_history
    - Session listing and search
    """

    def __init__(self, db_path: Path | str) -> None:
        """
        Initialize session manager.

        Args:
            db_path: Path to SQLite database
        """
        self._db_path = Path(db_path)
        self._db_path.parent.mkdir(parents=True, exist_ok=True)
        self._init_db()

    def _init_db(self) -> None:
        """Initialize database schema."""
        with self._connection() as conn:
            conn.executescript("""
                CREATE TABLE IF NOT EXISTS schema_version (
                    version INTEGER PRIMARY KEY
                );

                CREATE TABLE IF NOT EXISTS sessions (
                    id TEXT PRIMARY KEY,
                    created_at REAL NOT NULL,
                    updated_at REAL NOT NULL,
                    title TEXT DEFAULT '',
                    metadata TEXT DEFAULT '{}'
                );

                CREATE TABLE IF NOT EXISTS messages (
                    id TEXT PRIMARY KEY,
                    session_id TEXT NOT NULL,
                    role TEXT NOT NULL,
                    content TEXT NOT NULL,
                    source TEXT NOT NULL,
                    timestamp REAL NOT NULL,
                    tool_calls TEXT DEFAULT '[]',
                    tool_results TEXT DEFAULT '[]',
                    metadata TEXT DEFAULT '{}',
                    FOREIGN KEY (session_id) REFERENCES sessions(id) ON DELETE CASCADE
                );

                CREATE INDEX IF NOT EXISTS idx_messages_session
                ON messages(session_id, timestamp);

                CREATE INDEX IF NOT EXISTS idx_messages_source
                ON messages(source);

                CREATE INDEX IF NOT EXISTS idx_sessions_updated
                ON sessions(updated_at DESC);
            """)

            # Check/set schema version
            cursor = conn.execute("SELECT version FROM schema_version LIMIT 1")
            row = cursor.fetchone()
            if row is None:
                conn.execute(
                    "INSERT INTO schema_version (version) VALUES (?)",
                    (SCHEMA_VERSION,)
                )

    @contextmanager
    def _connection(self) -> Iterator[sqlite3.Connection]:
        """Get a database connection."""
        conn = sqlite3.connect(str(self._db_path))
        conn.row_factory = sqlite3.Row
        conn.execute("PRAGMA foreign_keys = ON")
        try:
            yield conn
            conn.commit()
        except Exception:
            conn.rollback()
            raise
        finally:
            conn.close()

    def create_session(self, title: str = "") -> Session:
        """
        Create a new session.

        Args:
            title: Optional session title

        Returns:
            Created session
        """
        session = Session.create(title)

        with self._connection() as conn:
            conn.execute(
                """
                INSERT INTO sessions (id, created_at, updated_at, title, metadata)
                VALUES (?, ?, ?, ?, ?)
                """,
                (
                    session.id,
                    session.created_at,
                    session.updated_at,
                    session.title,
                    json.dumps(session.metadata),
                ),
            )

        logger.info(f"Created session {session.id[:8]}")
        return session

    def get_session(self, session_id: str) -> Session | None:
        """Get a session by ID."""
        with self._connection() as conn:
            cursor = conn.execute(
                "SELECT * FROM sessions WHERE id = ?",
                (session_id,),
            )
            row = cursor.fetchone()
            if row:
                return Session(
                    id=row["id"],
                    created_at=row["created_at"],
                    updated_at=row["updated_at"],
                    title=row["title"],
                    metadata=json.loads(row["metadata"]),
                )
        return None

    def get_or_create_session(self, session_id: str | None = None) -> Session:
        """
        Get an existing session or create a new one.

        Args:
            session_id: Optional session ID

        Returns:
            Session (existing or new)
        """
        if session_id:
            session = self.get_session(session_id)
            if session:
                return session

        return self.create_session()

    def update_session(
        self,
        session_id: str,
        title: str | None = None,
        metadata: dict[str, Any] | None = None,
    ) -> bool:
        """
        Update session metadata.

        Args:
            session_id: Session ID
            title: New title
            metadata: Metadata to merge

        Returns:
            True if session was updated
        """
        with self._connection() as conn:
            updates = ["updated_at = ?"]
            params: list[Any] = [time.time()]

            if title is not None:
                updates.append("title = ?")
                params.append(title)

            if metadata is not None:
                # Merge with existing metadata
                cursor = conn.execute(
                    "SELECT metadata FROM sessions WHERE id = ?",
                    (session_id,),
                )
                row = cursor.fetchone()
                if row:
                    existing = json.loads(row["metadata"])
                    existing.update(metadata)
                    updates.append("metadata = ?")
                    params.append(json.dumps(existing))

            params.append(session_id)
            cursor = conn.execute(
                f"UPDATE sessions SET {', '.join(updates)} WHERE id = ?",
                params,
            )
            return cursor.rowcount > 0

    def delete_session(self, session_id: str) -> bool:
        """Delete a session and all its messages."""
        with self._connection() as conn:
            cursor = conn.execute(
                "DELETE FROM sessions WHERE id = ?",
                (session_id,),
            )
            return cursor.rowcount > 0

    def add_message(
        self,
        session_id: str,
        message: Message,
        source: str = MessageSource.HUMAN,
    ) -> SessionMessage:
        """
        Add a message to a session.

        Args:
            session_id: Session ID
            message: Message to add
            source: Message source

        Returns:
            Created SessionMessage
        """
        session_message = SessionMessage.from_message(message, session_id, source)

        with self._connection() as conn:
            conn.execute(
                """
                INSERT INTO messages
                (id, session_id, role, content, source, timestamp, tool_calls, tool_results, metadata)
                VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    session_message.id,
                    session_message.session_id,
                    session_message.role,
                    session_message.content,
                    session_message.source,
                    session_message.timestamp,
                    json.dumps(session_message.tool_calls),
                    json.dumps(session_message.tool_results),
                    json.dumps(session_message.metadata),
                ),
            )

            # Update session timestamp
            conn.execute(
                "UPDATE sessions SET updated_at = ? WHERE id = ?",
                (time.time(), session_id),
            )

        logger.debug(
            f"Added {message.role} message from {source} to session {session_id[:8]}"
        )
        return session_message

    def get_messages(
        self,
        session_id: str,
        limit: int = 100,
        offset: int = 0,
        include_tool_results: bool = True,
    ) -> list[SessionMessage]:
        """
        Get messages from a session.

        Args:
            session_id: Session ID
            limit: Maximum messages to return
            offset: Number of messages to skip (from newest)
            include_tool_results: Whether to include tool result messages

        Returns:
            List of SessionMessage (oldest first)
        """
        with self._connection() as conn:
            query = """
                SELECT * FROM messages
                WHERE session_id = ?
            """
            params: list[Any] = [session_id]

            if not include_tool_results:
                query += " AND role != 'tool'"

            query += " ORDER BY timestamp ASC LIMIT ? OFFSET ?"
            params.extend([limit, offset])

            cursor = conn.execute(query, params)
            return [self._row_to_message(row) for row in cursor.fetchall()]

    def get_history_for_mcp(
        self,
        session_id: str,
        limit: int = 20,
        include_tool_results: bool = True,
    ) -> list[dict[str, Any]]:
        """
        Get conversation history formatted for MCP get_history response.

        Args:
            session_id: Session ID
            limit: Maximum messages to return
            include_tool_results: Whether to include tool results

        Returns:
            List of message dicts for MCP
        """
        messages = self.get_messages(
            session_id,
            limit=limit,
            include_tool_results=include_tool_results,
        )

        return [
            {
                "id": msg.id,
                "role": msg.role,
                "content": msg.content,
                "source": msg.source,
                "timestamp": msg.timestamp,
                "has_tool_calls": len(msg.tool_calls) > 0,
            }
            for msg in messages
        ]

    def get_messages_as_base(self, session_id: str) -> list[Message]:
        """
        Get messages converted to base Message type.

        Args:
            session_id: Session ID

        Returns:
            List of Message for engine consumption
        """
        session_messages = self.get_messages(session_id)
        return [sm.to_message() for sm in session_messages]

    def list_sessions(
        self,
        limit: int = 50,
        offset: int = 0,
    ) -> list[Session]:
        """
        List sessions ordered by most recently updated.

        Args:
            limit: Maximum sessions to return
            offset: Number to skip

        Returns:
            List of sessions
        """
        with self._connection() as conn:
            cursor = conn.execute(
                """
                SELECT * FROM sessions
                ORDER BY updated_at DESC
                LIMIT ? OFFSET ?
                """,
                (limit, offset),
            )
            return [
                Session(
                    id=row["id"],
                    created_at=row["created_at"],
                    updated_at=row["updated_at"],
                    title=row["title"],
                    metadata=json.loads(row["metadata"]),
                )
                for row in cursor.fetchall()
            ]

    def search_sessions(self, query: str, limit: int = 20) -> list[Session]:
        """
        Search sessions by content.

        Args:
            query: Search query
            limit: Maximum results

        Returns:
            Matching sessions
        """
        with self._connection() as conn:
            # Search in message content and session title
            cursor = conn.execute(
                """
                SELECT DISTINCT s.* FROM sessions s
                LEFT JOIN messages m ON m.session_id = s.id
                WHERE s.title LIKE ? OR m.content LIKE ?
                ORDER BY s.updated_at DESC
                LIMIT ?
                """,
                (f"%{query}%", f"%{query}%", limit),
            )
            return [
                Session(
                    id=row["id"],
                    created_at=row["created_at"],
                    updated_at=row["updated_at"],
                    title=row["title"],
                    metadata=json.loads(row["metadata"]),
                )
                for row in cursor.fetchall()
            ]

    def get_latest_session(self) -> Session | None:
        """Get the most recently updated session."""
        sessions = self.list_sessions(limit=1)
        return sessions[0] if sessions else None

    def _row_to_message(self, row: sqlite3.Row) -> SessionMessage:
        """Convert a database row to SessionMessage."""
        return SessionMessage(
            id=row["id"],
            session_id=row["session_id"],
            role=row["role"],
            content=row["content"],
            source=row["source"],
            timestamp=row["timestamp"],
            tool_calls=json.loads(row["tool_calls"]),
            tool_results=json.loads(row["tool_results"]),
            metadata=json.loads(row["metadata"]),
        )
