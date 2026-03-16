"""Tests for storage system."""

import tempfile
from pathlib import Path

import pytest
from entropic.core.base import Message
from entropic.storage.backend import SQLiteStorage
from entropic.storage.models import ConversationRecord, DelegationRecord, MessageRecord


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

    def test_create_with_project_path(self) -> None:
        """Test creating with project path."""
        record = ConversationRecord.create(
            title="Test",
            project_path="/path/to/project",
        )
        assert record.project_path == "/path/to/project"


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

    def test_from_message_with_tool_calls(self) -> None:
        """Test creating from Message with tool calls."""
        message = Message(
            role="assistant",
            content="Using tool",
            tool_calls=[{"id": "1", "name": "test"}],
        )
        record = MessageRecord.from_message(message, "conv-123")
        assert len(record.tool_calls) == 1


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

    async def test_list_empty(self, storage: SQLiteStorage) -> None:
        """Test listing when no conversations exist."""
        conversations = await storage.list_conversations()
        assert conversations == []

    async def test_delete_conversation(self, storage: SQLiteStorage) -> None:
        """Test deleting a conversation."""
        conv_id = await storage.create_conversation()
        await storage.delete_conversation(conv_id)

        conversations = await storage.list_conversations()
        assert len(conversations) == 0

    async def test_load_nonexistent(self, storage: SQLiteStorage) -> None:
        """Test loading nonexistent conversation."""
        messages, metadata = await storage.load_conversation("nonexistent")
        assert messages == []
        assert metadata == {}

    async def test_update_title(self, storage: SQLiteStorage) -> None:
        """Test updating conversation title."""
        conv_id = await storage.create_conversation(title="Original")
        await storage.update_conversation_title(conv_id, "Updated")

        conversations = await storage.list_conversations()
        assert conversations[0]["title"] == "Updated"

    async def test_get_stats(self, storage: SQLiteStorage) -> None:
        """Test getting storage stats."""
        stats = await storage.get_conversation_stats()
        assert "total_conversations" in stats
        assert "total_messages" in stats
        assert "total_tokens" in stats


class TestDelegationRecord:
    """Tests for DelegationRecord."""

    def test_create(self) -> None:
        record = DelegationRecord.create(
            parent_conversation_id="parent-1",
            child_conversation_id="child-1",
            delegating_tier="lead",
            target_tier="eng",
            task="implement feature",
        )
        record.max_turns = 10
        assert record.delegating_tier == "lead"
        assert record.target_tier == "eng"
        assert record.status == "pending"
        assert record.max_turns == 10

    def test_to_row_roundtrip(self) -> None:
        record = DelegationRecord.create(
            parent_conversation_id="p1",
            child_conversation_id="c1",
            delegating_tier="lead",
            target_tier="eng",
            task="do stuff",
        )
        row_dict = {
            "id": record.id,
            "parent_conversation_id": record.parent_conversation_id,
            "child_conversation_id": record.child_conversation_id,
            "delegating_tier": record.delegating_tier,
            "target_tier": record.target_tier,
            "task": record.task,
            "max_turns": record.max_turns,
            "status": record.status,
            "result_summary": record.result_summary,
            "created_at": record.created_at.isoformat(),
            "completed_at": None,
        }
        restored = DelegationRecord.from_row(row_dict)
        assert restored.id == record.id
        assert restored.target_tier == "eng"
        assert restored.completed_at is None


class TestMessageRecordIdentityTier:
    """Tests for identity_tier on MessageRecord."""

    def test_from_message_with_identity_tier(self) -> None:
        msg = Message(
            role="assistant",
            content="done",
            metadata={"identity_tier": "eng"},
        )
        record = MessageRecord.from_message(msg, "conv-1")
        assert record.identity_tier == "eng"

    def test_to_message_includes_identity_tier(self) -> None:
        record = MessageRecord(
            id="m1",
            conversation_id="c1",
            role="assistant",
            content="done",
            identity_tier="qa",
        )
        msg = record.to_message()
        assert msg.metadata["identity_tier"] == "qa"

    def test_to_message_no_identity_tier(self) -> None:
        record = MessageRecord(
            id="m1",
            conversation_id="c1",
            role="user",
            content="hello",
        )
        msg = record.to_message()
        assert "identity_tier" not in msg.metadata


@pytest.mark.asyncio
class TestDelegationStorage:
    """Tests for delegation storage in SQLiteStorage."""

    @pytest.fixture
    async def storage(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            db_path = Path(tmpdir) / "test.db"
            storage = SQLiteStorage(db_path)
            await storage.initialize()
            yield storage
            await storage.close()

    async def test_create_delegation(self, storage: SQLiteStorage) -> None:
        parent_id = await storage.create_conversation(title="Parent")
        delegation_id, child_id = await storage.create_delegation(
            parent_conversation_id=parent_id,
            delegating_tier="lead",
            target_tier="eng",
            task="implement login",
        )
        assert delegation_id is not None
        assert child_id is not None

        delegations = await storage.get_delegations(parent_id)
        assert len(delegations) == 1
        assert delegations[0]["target_tier"] == "eng"
        assert delegations[0]["status"] == "running"

    async def test_complete_delegation(self, storage: SQLiteStorage) -> None:
        parent_id = await storage.create_conversation(title="Parent")
        delegation_id, child_id = await storage.create_delegation(
            parent_conversation_id=parent_id,
            delegating_tier="lead",
            target_tier="qa",
            task="review code",
            max_turns=5,
        )
        await storage.complete_delegation(delegation_id, "completed", "All tests pass.")

        delegations = await storage.get_delegations(parent_id)
        assert delegations[0]["status"] == "completed"
        assert delegations[0]["result_summary"] == "All tests pass."
        assert delegations[0]["completed_at"] is not None

    async def test_failed_delegation(self, storage: SQLiteStorage) -> None:
        parent_id = await storage.create_conversation(title="Parent")
        delegation_id, _ = await storage.create_delegation(
            parent_conversation_id=parent_id,
            delegating_tier="lead",
            target_tier="eng",
            task="break things",
        )
        await storage.complete_delegation(delegation_id, "failed", "Child loop error")

        delegations = await storage.get_delegations(parent_id)
        assert delegations[0]["status"] == "failed"

    async def test_child_conversation_has_messages(self, storage: SQLiteStorage) -> None:
        parent_id = await storage.create_conversation(title="Parent")
        _, child_id = await storage.create_delegation(
            parent_conversation_id=parent_id,
            delegating_tier="lead",
            target_tier="eng",
            task="write code",
        )
        child_messages = [
            Message(role="system", content="You are eng."),
            Message(role="user", content="Write code."),
            Message(role="assistant", content="Done."),
        ]
        await storage.save_conversation(child_id, child_messages)

        loaded, _ = await storage.load_conversation(child_id)
        assert len(loaded) == 3
        assert loaded[2].content == "Done."

    async def test_no_delegations_for_new_conversation(self, storage: SQLiteStorage) -> None:
        conv_id = await storage.create_conversation(title="Fresh")
        delegations = await storage.get_delegations(conv_id)
        assert delegations == []
