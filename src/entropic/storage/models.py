"""
Data models for storage.

Provides typed models for database entities.
"""

import json
import uuid
from dataclasses import dataclass, field
from datetime import datetime
from typing import Any

from entropic.core.base import Message


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
class DelegationRecord:
    """Database record for a delegation."""

    id: str
    parent_conversation_id: str
    child_conversation_id: str
    delegating_tier: str
    target_tier: str
    task: str
    max_turns: int | None = None
    status: str = "pending"
    result_summary: str | None = None
    created_at: datetime = field(default_factory=datetime.utcnow)
    completed_at: datetime | None = None

    @classmethod
    def create(
        cls,
        parent_conversation_id: str,
        child_conversation_id: str,
        delegating_tier: str,
        target_tier: str,
        task: str,
    ) -> "DelegationRecord":
        """Create a new delegation record."""
        return cls(
            id=str(uuid.uuid4()),
            parent_conversation_id=parent_conversation_id,
            child_conversation_id=child_conversation_id,
            delegating_tier=delegating_tier,
            target_tier=target_tier,
            task=task,
        )

    @classmethod
    def from_row(cls, row: dict[str, Any]) -> "DelegationRecord":
        """Create from database row."""
        return cls(
            id=row["id"],
            parent_conversation_id=row["parent_conversation_id"],
            child_conversation_id=row["child_conversation_id"],
            delegating_tier=row["delegating_tier"],
            target_tier=row["target_tier"],
            task=row["task"],
            max_turns=row.get("max_turns"),
            status=row["status"],
            result_summary=row.get("result_summary"),
            created_at=datetime.fromisoformat(row["created_at"]),
            completed_at=(
                datetime.fromisoformat(row["completed_at"]) if row.get("completed_at") else None
            ),
        )

    def to_row(self) -> tuple[Any, ...]:
        """Convert to database row tuple for INSERT."""
        return (
            self.id,
            self.parent_conversation_id,
            self.child_conversation_id,
            self.delegating_tier,
            self.target_tier,
            self.task,
            self.max_turns,
            self.status,
            self.result_summary,
            self.created_at.isoformat(),
            self.completed_at.isoformat() if self.completed_at else None,
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
    identity_tier: str | None = None

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
            identity_tier=message.metadata.get("identity_tier"),
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
            identity_tier=row.get("identity_tier"),
        )

    def to_message(self) -> Message:
        """Convert to Message object."""
        metadata: dict[str, Any] = {"token_count": self.token_count}
        if self.identity_tier:
            metadata["identity_tier"] = self.identity_tier
        return Message(
            role=self.role,
            content=self.content,
            tool_calls=self.tool_calls,
            tool_results=self.tool_results,
            metadata=metadata,
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
            self.identity_tier,
        )
