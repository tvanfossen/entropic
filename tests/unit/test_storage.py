"""Tests for storage system."""

import tempfile
from pathlib import Path

import pytest
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
