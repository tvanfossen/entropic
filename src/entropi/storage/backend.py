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

        messages = [MessageRecord.from_row(row).to_message() for row in msg_rows]

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
        total = await self._db.fetchone("SELECT COUNT(*) as count FROM conversations")
        messages = await self._db.fetchone("SELECT COUNT(*) as count FROM messages")
        tokens = await self._db.fetchone("SELECT SUM(token_count) as total FROM messages")

        return {
            "total_conversations": total["count"] if total else 0,
            "total_messages": messages["count"] if messages else 0,
            "total_tokens": tokens["total"] or 0 if tokens else 0,
        }
