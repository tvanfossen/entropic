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

    def to_row(self) -> tuple[Any, ...]:
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

    def to_row(self) -> tuple[Any, ...]:
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
