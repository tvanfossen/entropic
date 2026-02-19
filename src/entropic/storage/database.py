"""
SQLite database operations.

Provides async database access using aiosqlite.
"""

import asyncio
from collections.abc import AsyncIterator
from contextlib import asynccontextmanager
from pathlib import Path
from typing import Any

import aiosqlite

from entropic.core.logging import get_logger

logger = get_logger("storage.database")


# Database migrations
MIGRATIONS = {
    "001_initial": """
        -- Conversations table
        CREATE TABLE IF NOT EXISTS conversations (
            id TEXT PRIMARY KEY,
            title TEXT,
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            project_path TEXT,
            model_id TEXT,
            metadata TEXT DEFAULT '{}'
        );

        -- Messages table
        CREATE TABLE IF NOT EXISTS messages (
            id TEXT PRIMARY KEY,
            conversation_id TEXT NOT NULL,
            role TEXT NOT NULL CHECK(role IN ('user', 'assistant', 'system', 'tool')),
            content TEXT NOT NULL,
            tool_calls TEXT DEFAULT '[]',
            tool_results TEXT DEFAULT '[]',
            token_count INTEGER DEFAULT 0,
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            is_compacted BOOLEAN DEFAULT FALSE,
            FOREIGN KEY (conversation_id) REFERENCES conversations(id) ON DELETE CASCADE
        );

        -- Tool executions table
        CREATE TABLE IF NOT EXISTS tool_executions (
            id TEXT PRIMARY KEY,
            message_id TEXT,
            server_name TEXT NOT NULL,
            tool_name TEXT NOT NULL,
            arguments TEXT DEFAULT '{}',
            result TEXT,
            duration_ms INTEGER DEFAULT 0,
            status TEXT CHECK(status IN ('success', 'error', 'timeout')),
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            FOREIGN KEY (message_id) REFERENCES messages(id) ON DELETE CASCADE
        );

        -- Indexes
        CREATE INDEX IF NOT EXISTS idx_messages_conversation
            ON messages(conversation_id);
        CREATE INDEX IF NOT EXISTS idx_messages_created
            ON messages(created_at);
        CREATE INDEX IF NOT EXISTS idx_conversations_updated
            ON conversations(updated_at);
    """,
    "002_fts": """
        -- Full-text search virtual table
        CREATE VIRTUAL TABLE IF NOT EXISTS messages_fts USING fts5(
            content,
            content='messages',
            content_rowid='rowid'
        );

        -- Triggers to keep FTS in sync
        CREATE TRIGGER IF NOT EXISTS messages_ai AFTER INSERT ON messages BEGIN
            INSERT INTO messages_fts(rowid, content) VALUES (NEW.rowid, NEW.content);
        END;

        CREATE TRIGGER IF NOT EXISTS messages_ad AFTER DELETE ON messages BEGIN
            INSERT INTO messages_fts(messages_fts, rowid, content)
                VALUES('delete', OLD.rowid, OLD.content);
        END;

        CREATE TRIGGER IF NOT EXISTS messages_au AFTER UPDATE ON messages BEGIN
            INSERT INTO messages_fts(messages_fts, rowid, content)
                VALUES('delete', OLD.rowid, OLD.content);
            INSERT INTO messages_fts(rowid, content) VALUES (NEW.rowid, NEW.content);
        END;
    """,
}


class Database:
    """
    Async SQLite database wrapper.

    Handles connection pooling, migrations, and common operations.
    """

    def __init__(self, db_path: Path) -> None:
        """
        Initialize database.

        Args:
            db_path: Path to SQLite database file
        """
        self.db_path = db_path
        self._connection: aiosqlite.Connection | None = None
        self._lock = asyncio.Lock()

    async def initialize(self) -> None:
        """Initialize database and run migrations."""
        # Ensure directory exists
        self.db_path.parent.mkdir(parents=True, exist_ok=True)

        async with self._get_connection() as conn:
            await self._run_migrations(conn)

        logger.info(f"Database initialized: {self.db_path}")

    async def close(self) -> None:
        """Close database connection."""
        if self._connection:
            await self._connection.close()
            self._connection = None

    @asynccontextmanager
    async def _get_connection(self) -> AsyncIterator[aiosqlite.Connection]:
        """Get database connection."""
        async with self._lock:
            if self._connection is None:
                self._connection = await aiosqlite.connect(self.db_path)
                self._connection.row_factory = aiosqlite.Row

            yield self._connection

    async def _run_migrations(self, conn: aiosqlite.Connection) -> None:
        """Run database migrations."""
        # Create migrations table
        await conn.execute(
            """
            CREATE TABLE IF NOT EXISTS migrations (
                id INTEGER PRIMARY KEY,
                name TEXT UNIQUE,
                applied_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
            )
        """
        )

        # Get applied migrations
        cursor = await conn.execute("SELECT name FROM migrations")
        applied = {row[0] for row in await cursor.fetchall()}

        # Run pending migrations
        for name, sql in MIGRATIONS.items():
            if name not in applied:
                logger.info(f"Running migration: {name}")
                await conn.executescript(sql)
                await conn.execute(
                    "INSERT INTO migrations (name) VALUES (?)",
                    (name,),
                )

        await conn.commit()

    async def execute(
        self,
        sql: str,
        params: tuple[Any, ...] = (),
    ) -> aiosqlite.Cursor:
        """Execute SQL statement."""
        async with self._get_connection() as conn:
            cursor = await conn.execute(sql, params)
            await conn.commit()
            return cursor

    async def executemany(
        self,
        sql: str,
        params_list: list[tuple[Any, ...]],
    ) -> None:
        """Execute SQL statement with multiple parameter sets."""
        async with self._get_connection() as conn:
            await conn.executemany(sql, params_list)
            await conn.commit()

    async def fetchone(
        self,
        sql: str,
        params: tuple[Any, ...] = (),
    ) -> dict[str, Any] | None:
        """Fetch single row."""
        async with self._get_connection() as conn:
            cursor = await conn.execute(sql, params)
            row = await cursor.fetchone()
            return dict(row) if row else None

    async def fetchall(
        self,
        sql: str,
        params: tuple[Any, ...] = (),
    ) -> list[dict[str, Any]]:
        """Fetch all rows."""
        async with self._get_connection() as conn:
            cursor = await conn.execute(sql, params)
            rows = await cursor.fetchall()
            return [dict(row) for row in rows]
