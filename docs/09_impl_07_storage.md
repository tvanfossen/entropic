# Implementation 07: Storage

> SQLite-based conversation persistence with async support

**Prerequisites:** Implementation 06 complete
**Estimated Time:** 2 hours with Claude Code
**Checkpoint:** Conversations persist across sessions

---

## Objectives

1. Implement SQLite storage backend with aiosqlite
2. Create data models for conversations and messages
3. Add full-text search capability
4. Implement conversation import/export
5. Handle migrations

---

## 1. Database Layer

### File: `src/entropi/storage/database.py`

```python
"""
SQLite database operations.

Provides async database access using aiosqlite.
"""
import asyncio
import json
from contextlib import asynccontextmanager
from pathlib import Path
from typing import Any, AsyncIterator

import aiosqlite

from entropi.core.logging import get_logger

logger = get_logger("storage.database")


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
        await conn.execute("""
            CREATE TABLE IF NOT EXISTS migrations (
                id INTEGER PRIMARY KEY,
                name TEXT UNIQUE,
                applied_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
            )
        """)

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
        params: tuple = (),
    ) -> aiosqlite.Cursor:
        """Execute SQL statement."""
        async with self._get_connection() as conn:
            cursor = await conn.execute(sql, params)
            await conn.commit()
            return cursor

    async def executemany(
        self,
        sql: str,
        params_list: list[tuple],
    ) -> None:
        """Execute SQL statement with multiple parameter sets."""
        async with self._get_connection() as conn:
            await conn.executemany(sql, params_list)
            await conn.commit()

    async def fetchone(
        self,
        sql: str,
        params: tuple = (),
    ) -> dict[str, Any] | None:
        """Fetch single row."""
        async with self._get_connection() as conn:
            cursor = await conn.execute(sql, params)
            row = await cursor.fetchone()
            return dict(row) if row else None

    async def fetchall(
        self,
        sql: str,
        params: tuple = (),
    ) -> list[dict[str, Any]]:
        """Fetch all rows."""
        async with self._get_connection() as conn:
            cursor = await conn.execute(sql, params)
            rows = await cursor.fetchall()
            return [dict(row) for row in rows]


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
```

---

## 2. Data Models

### File: `src/entropi/storage/models.py`

```python
"""
Data models for storage.

Provides typed models for database entities.
"""
import json
import uuid
from dataclasses import dataclass, field
from datetime import datetime
from typing import Any

from entropi.core.base import Message


@dataclass
class ConversationRecord:
    """Database record for a conversation."""

    id: str
    title: str
    created_at: datetime
    updated_at: datetime
    project_path: str | None = None
    model_id: str | None = None
    metadata: dict[str, Any] = field(default_factory=dict)

    @classmethod
    def create(
        cls,
        title: str = "New Conversation",
        project_path: str | None = None,
        model_id: str | None = None,
    ) -> "ConversationRecord":
        """Create a new conversation record."""
        now = datetime.utcnow()
        return cls(
            id=str(uuid.uuid4()),
            title=title,
            created_at=now,
            updated_at=now,
            project_path=project_path,
            model_id=model_id,
        )

    @classmethod
    def from_row(cls, row: dict[str, Any]) -> "ConversationRecord":
        """Create from database row."""
        return cls(
            id=row["id"],
            title=row["title"],
            created_at=datetime.fromisoformat(row["created_at"]),
            updated_at=datetime.fromisoformat(row["updated_at"]),
            project_path=row.get("project_path"),
            model_id=row.get("model_id"),
            metadata=json.loads(row.get("metadata", "{}")),
        )

    def to_row(self) -> tuple:
        """Convert to database row tuple."""
        return (
            self.id,
            self.title,
            self.created_at.isoformat(),
            self.updated_at.isoformat(),
            self.project_path,
            self.model_id,
            json.dumps(self.metadata),
        )


@dataclass
class MessageRecord:
    """Database record for a message."""

    id: str
    conversation_id: str
    role: str
    content: str
    tool_calls: list[dict[str, Any]] = field(default_factory=list)
    tool_results: list[dict[str, Any]] = field(default_factory=list)
    token_count: int = 0
    created_at: datetime = field(default_factory=datetime.utcnow)
    is_compacted: bool = False

    @classmethod
    def from_message(
        cls,
        message: Message,
        conversation_id: str,
    ) -> "MessageRecord":
        """Create from Message object."""
        return cls(
            id=str(uuid.uuid4()),
            conversation_id=conversation_id,
            role=message.role,
            content=message.content,
            tool_calls=message.tool_calls,
            tool_results=message.tool_results,
            token_count=message.metadata.get("token_count", 0),
        )

    @classmethod
    def from_row(cls, row: dict[str, Any]) -> "MessageRecord":
        """Create from database row."""
        return cls(
            id=row["id"],
            conversation_id=row["conversation_id"],
            role=row["role"],
            content=row["content"],
            tool_calls=json.loads(row.get("tool_calls", "[]")),
            tool_results=json.loads(row.get("tool_results", "[]")),
            token_count=row.get("token_count", 0),
            created_at=datetime.fromisoformat(row["created_at"]),
            is_compacted=bool(row.get("is_compacted", False)),
        )

    def to_message(self) -> Message:
        """Convert to Message object."""
        return Message(
            role=self.role,
            content=self.content,
            tool_calls=self.tool_calls,
            tool_results=self.tool_results,
            metadata={"token_count": self.token_count},
        )

    def to_row(self) -> tuple:
        """Convert to database row tuple."""
        return (
            self.id,
            self.conversation_id,
            self.role,
            self.content,
            json.dumps(self.tool_calls),
            json.dumps(self.tool_results),
            self.token_count,
            self.created_at.isoformat(),
            self.is_compacted,
        )
```

---

## 3. Storage Backend

### File: `src/entropi/storage/backend.py`

```python
"""
Storage backend implementation.

Implements the StorageBackend abstract class using SQLite.
"""
from datetime import datetime
from pathlib import Path
from typing import Any

from entropi.core.base import Message, StorageBackend
from entropi.core.logging import get_logger
from entropi.storage.database import Database
from entropi.storage.models import ConversationRecord, MessageRecord

logger = get_logger("storage.backend")


class SQLiteStorage(StorageBackend):
    """SQLite-based storage backend."""

    def __init__(self, db_path: Path) -> None:
        """
        Initialize storage.

        Args:
            db_path: Path to database file
        """
        self._db = Database(db_path)

    async def initialize(self) -> None:
        """Initialize storage."""
        await self._db.initialize()

    async def close(self) -> None:
        """Close storage."""
        await self._db.close()

    async def create_conversation(
        self,
        title: str = "New Conversation",
        project_path: str | None = None,
        model_id: str | None = None,
    ) -> str:
        """
        Create a new conversation.

        Returns:
            Conversation ID
        """
        record = ConversationRecord.create(
            title=title,
            project_path=project_path,
            model_id=model_id,
        )

        await self._db.execute(
            """
            INSERT INTO conversations
                (id, title, created_at, updated_at, project_path, model_id, metadata)
            VALUES (?, ?, ?, ?, ?, ?, ?)
            """,
            record.to_row(),
        )

        logger.info(f"Created conversation: {record.id}")
        return record.id

    async def save_conversation(
        self,
        conversation_id: str,
        messages: list[Message],
        metadata: dict[str, Any] | None = None,
    ) -> None:
        """Save messages to a conversation."""
        # Update conversation timestamp
        await self._db.execute(
            "UPDATE conversations SET updated_at = ? WHERE id = ?",
            (datetime.utcnow().isoformat(), conversation_id),
        )

        # Insert messages
        for message in messages:
            record = MessageRecord.from_message(message, conversation_id)
            await self._db.execute(
                """
                INSERT INTO messages
                    (id, conversation_id, role, content, tool_calls,
                     tool_results, token_count, created_at, is_compacted)
                VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                record.to_row(),
            )

    async def load_conversation(
        self,
        conversation_id: str,
    ) -> tuple[list[Message], dict[str, Any]]:
        """Load a conversation."""
        # Get conversation metadata
        conv_row = await self._db.fetchone(
            "SELECT * FROM conversations WHERE id = ?",
            (conversation_id,),
        )

        if not conv_row:
            return [], {}

        conv = ConversationRecord.from_row(conv_row)

        # Get messages
        msg_rows = await self._db.fetchall(
            """
            SELECT * FROM messages
            WHERE conversation_id = ?
            ORDER BY created_at ASC
            """,
            (conversation_id,),
        )

        messages = [
            MessageRecord.from_row(row).to_message()
            for row in msg_rows
        ]

        return messages, conv.metadata

    async def list_conversations(
        self,
        limit: int = 100,
        offset: int = 0,
    ) -> list[dict[str, Any]]:
        """List conversations."""
        rows = await self._db.fetchall(
            """
            SELECT c.*, COUNT(m.id) as message_count
            FROM conversations c
            LEFT JOIN messages m ON c.id = m.conversation_id
            GROUP BY c.id
            ORDER BY c.updated_at DESC
            LIMIT ? OFFSET ?
            """,
            (limit, offset),
        )

        return [
            {
                "id": row["id"],
                "title": row["title"],
                "updated_at": row["updated_at"],
                "message_count": row["message_count"],
                "project_path": row["project_path"],
            }
            for row in rows
        ]

    async def search_conversations(
        self,
        query: str,
        limit: int = 10,
    ) -> list[dict[str, Any]]:
        """Search conversations using full-text search."""
        rows = await self._db.fetchall(
            """
            SELECT DISTINCT c.id, c.title, c.updated_at,
                   snippet(messages_fts, 0, '>>>', '<<<', '...', 32) as snippet
            FROM messages_fts
            JOIN messages m ON messages_fts.rowid = m.rowid
            JOIN conversations c ON m.conversation_id = c.id
            WHERE messages_fts MATCH ?
            ORDER BY rank
            LIMIT ?
            """,
            (query, limit),
        )

        return [dict(row) for row in rows]

    async def delete_conversation(self, conversation_id: str) -> None:
        """Delete a conversation and its messages."""
        await self._db.execute(
            "DELETE FROM conversations WHERE id = ?",
            (conversation_id,),
        )
        logger.info(f"Deleted conversation: {conversation_id}")

    async def update_conversation_title(
        self,
        conversation_id: str,
        title: str,
    ) -> None:
        """Update conversation title."""
        await self._db.execute(
            "UPDATE conversations SET title = ? WHERE id = ?",
            (title, conversation_id),
        )

    async def get_conversation_stats(self) -> dict[str, Any]:
        """Get storage statistics."""
        total = await self._db.fetchone(
            "SELECT COUNT(*) as count FROM conversations"
        )
        messages = await self._db.fetchone(
            "SELECT COUNT(*) as count FROM messages"
        )
        tokens = await self._db.fetchone(
            "SELECT SUM(token_count) as total FROM messages"
        )

        return {
            "total_conversations": total["count"] if total else 0,
            "total_messages": messages["count"] if messages else 0,
            "total_tokens": tokens["total"] or 0 if tokens else 0,
        }
```

---

## 4. Tests

### File: `tests/unit/test_storage.py`

```python
"""Tests for storage system."""
import pytest
import tempfile
from pathlib import Path

from entropi.core.base import Message
from entropi.storage.backend import SQLiteStorage
from entropi.storage.models import ConversationRecord, MessageRecord


class TestConversationRecord:
    """Tests for ConversationRecord."""

    def test_create(self) -> None:
        """Test creating a new record."""
        record = ConversationRecord.create(title="Test")
        assert record.title == "Test"
        assert record.id is not None

    def test_to_row_and_back(self) -> None:
        """Test round-trip conversion."""
        original = ConversationRecord.create(title="Test")
        row = {
            "id": original.id,
            "title": original.title,
            "created_at": original.created_at.isoformat(),
            "updated_at": original.updated_at.isoformat(),
            "project_path": None,
            "model_id": None,
            "metadata": "{}",
        }
        restored = ConversationRecord.from_row(row)
        assert restored.id == original.id
        assert restored.title == original.title


class TestMessageRecord:
    """Tests for MessageRecord."""

    def test_from_message(self) -> None:
        """Test creating from Message."""
        message = Message(role="user", content="Hello")
        record = MessageRecord.from_message(message, "conv-123")
        assert record.role == "user"
        assert record.content == "Hello"
        assert record.conversation_id == "conv-123"

    def test_to_message(self) -> None:
        """Test converting to Message."""
        record = MessageRecord(
            id="msg-1",
            conversation_id="conv-1",
            role="assistant",
            content="Hi there",
        )
        message = record.to_message()
        assert message.role == "assistant"
        assert message.content == "Hi there"


@pytest.mark.asyncio
class TestSQLiteStorage:
    """Tests for SQLite storage backend."""

    @pytest.fixture
    async def storage(self):
        """Create temporary storage."""
        with tempfile.TemporaryDirectory() as tmpdir:
            db_path = Path(tmpdir) / "test.db"
            storage = SQLiteStorage(db_path)
            await storage.initialize()
            yield storage
            await storage.close()

    async def test_create_conversation(self, storage: SQLiteStorage) -> None:
        """Test creating a conversation."""
        conv_id = await storage.create_conversation(title="Test")
        assert conv_id is not None

        conversations = await storage.list_conversations()
        assert len(conversations) == 1
        assert conversations[0]["title"] == "Test"

    async def test_save_and_load(self, storage: SQLiteStorage) -> None:
        """Test saving and loading messages."""
        conv_id = await storage.create_conversation()

        messages = [
            Message(role="user", content="Hello"),
            Message(role="assistant", content="Hi!"),
        ]

        await storage.save_conversation(conv_id, messages)

        loaded, metadata = await storage.load_conversation(conv_id)
        assert len(loaded) == 2
        assert loaded[0].content == "Hello"
        assert loaded[1].content == "Hi!"

    async def test_search(self, storage: SQLiteStorage) -> None:
        """Test full-text search."""
        conv_id = await storage.create_conversation(title="Python Help")

        messages = [
            Message(role="user", content="How do I write a for loop in Python?"),
            Message(role="assistant", content="Here is how to write a for loop..."),
        ]

        await storage.save_conversation(conv_id, messages)

        results = await storage.search_conversations("for loop")
        assert len(results) >= 1
```

---

## Checkpoint: Verification

```bash
# Run tests
pytest tests/unit/test_storage.py -v

# Test persistence manually
entropi
> Hello, how are you?
# Exit and restart
entropi
> /load  # Should show previous conversation
```

**Success Criteria:**
- [ ] Storage tests pass
- [ ] Conversations persist after restart
- [ ] Search works
- [ ] Database file created correctly

---

## Next Phase

Proceed to **Implementation 08: Commands & Context** to implement slash commands.
