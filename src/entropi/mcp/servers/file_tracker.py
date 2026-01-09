"""
File access tracking for read-before-write enforcement.

Tracks file reads to prevent blind modifications.
"""

from dataclasses import dataclass
from datetime import datetime, timedelta
from hashlib import sha256
from pathlib import Path

from entropi.core.logging import get_logger

logger = get_logger("mcp.file_tracker")


@dataclass
class FileAccessRecord:
    """Record of a file read operation."""

    path: Path
    content_hash: str
    read_at: datetime
    content_preview: str  # First 200 chars for debugging

    def is_stale(self, max_age: timedelta = timedelta(minutes=5)) -> bool:
        """Check if this read is too old to trust."""
        return datetime.utcnow() - self.read_at > max_age


class FileAccessTracker:
    """
    Tracks file reads to enforce read-before-write.

    Maintains a record of recently read files with their
    content hashes to detect external modifications.
    """

    def __init__(self, max_age_minutes: int = 5) -> None:
        """
        Initialize tracker.

        Args:
            max_age_minutes: How long a read record is valid
        """
        self.max_age = timedelta(minutes=max_age_minutes)
        self._records: dict[Path, FileAccessRecord] = {}

    def record_read(self, path: Path, content: str) -> None:
        """
        Record that a file was read.

        Args:
            path: File path
            content: File content that was read
        """
        resolved = path.resolve()
        self._records[resolved] = FileAccessRecord(
            path=resolved,
            content_hash=sha256(content.encode()).hexdigest(),
            read_at=datetime.utcnow(),
            content_preview=content[:200],
        )
        logger.debug(f"Recorded read: {resolved}")

    def get_record(self, path: Path) -> FileAccessRecord | None:
        """
        Get the read record for a file.

        Args:
            path: File path

        Returns:
            FileAccessRecord or None if not read recently
        """
        resolved = path.resolve()
        record = self._records.get(resolved)
        if record and record.is_stale(self.max_age):
            # Clean up stale record
            del self._records[resolved]
            logger.debug(f"Removed stale record: {resolved}")
            return None
        return record

    def was_read_recently(self, path: Path) -> bool:
        """
        Check if file was read recently.

        Args:
            path: File path

        Returns:
            True if file was read within max_age
        """
        return self.get_record(path) is not None

    def verify_unchanged(self, path: Path, current_content: str) -> bool:
        """
        Verify file content matches what was read.

        Args:
            path: File path
            current_content: Current file content to verify

        Returns:
            True if content hash matches the recorded read
        """
        record = self.get_record(path)
        if not record:
            return False
        current_hash = sha256(current_content.encode()).hexdigest()
        return record.content_hash == current_hash

    def clear(self, path: Path | None = None) -> None:
        """
        Clear records.

        Args:
            path: Specific path to clear, or None to clear all
        """
        if path:
            self._records.pop(path.resolve(), None)
        else:
            self._records.clear()
