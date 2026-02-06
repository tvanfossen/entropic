"""Tests for headless presenter."""

import asyncio

import pytest
from entropi.core.engine import ToolApproval
from entropi.ui.headless import HeadlessPresenter
from entropi.ui.presenter import Presenter


class TestHeadlessPresenter:
    """Tests for HeadlessPresenter implementation."""

    def test_implements_presenter_interface(self) -> None:
        """Test that HeadlessPresenter implements Presenter ABC."""
        presenter = HeadlessPresenter()
        assert isinstance(presenter, Presenter)

    def test_initial_state(self) -> None:
        """Test initial presenter state."""
        presenter = HeadlessPresenter()
        assert not presenter._running
        assert not presenter._generating
        assert presenter._auto_approve is True
        assert presenter.get_stream_content() == ""
        assert presenter.get_messages() == []

    def test_auto_approve_default(self) -> None:
        """Test default auto-approve behavior."""
        presenter = HeadlessPresenter()
        assert presenter._auto_approve is True

    def test_auto_approve_disabled(self) -> None:
        """Test auto-approve can be disabled."""
        presenter = HeadlessPresenter(auto_approve=False)
        assert presenter._auto_approve is False

    # === Generation Lifecycle ===

    def test_generation_lifecycle(self) -> None:
        """Test start/end generation state changes."""
        presenter = HeadlessPresenter()
        assert not presenter._generating

        presenter.start_generation()
        assert presenter._generating
        assert presenter._stream_buffer == []

        presenter.end_generation()
        assert not presenter._generating

    def test_stream_chunk_captured(self) -> None:
        """Test streaming chunks are captured."""
        presenter = HeadlessPresenter()
        presenter.start_generation()

        presenter.on_stream_chunk("Hello ")
        presenter.on_stream_chunk("world!")

        assert presenter.get_stream_content() == "Hello world!"

    def test_stream_buffer_clears_on_new_generation(self) -> None:
        """Test stream buffer is cleared when starting new generation."""
        presenter = HeadlessPresenter()
        presenter.start_generation()
        presenter.on_stream_chunk("first response")
        presenter.end_generation()

        presenter.start_generation()
        assert presenter.get_stream_content() == ""

    # === Messages ===

    def test_add_message_captured(self) -> None:
        """Test messages are captured."""
        presenter = HeadlessPresenter()

        presenter.add_message("user", "Hello")
        presenter.add_message("assistant", "Hi there!")

        messages = presenter.get_messages()
        assert len(messages) == 2
        assert messages[0] == ("user", "Hello")
        assert messages[1] == ("assistant", "Hi there!")

    def test_print_info_captured(self) -> None:
        """Test info messages are captured."""
        presenter = HeadlessPresenter()

        presenter.print_info("Info message")

        assert "Info message" in presenter.get_info_messages()

    def test_print_warning_captured(self) -> None:
        """Test warning messages are captured."""
        presenter = HeadlessPresenter()

        presenter.print_warning("Warning message")

        assert "Warning message" in presenter.get_warning_messages()

    def test_print_error_captured(self) -> None:
        """Test error messages are captured."""
        presenter = HeadlessPresenter()

        presenter.print_error("Error message")

        assert "Error message" in presenter.get_error_messages()

    # === Tool Handling ===

    def test_tool_calls_captured(self) -> None:
        """Test tool calls are captured."""
        presenter = HeadlessPresenter()

        presenter.print_tool_start("test_tool", {"arg1": "value1"})

        tool_calls = presenter.get_tool_calls()
        assert len(tool_calls) == 1
        assert tool_calls[0]["name"] == "test_tool"
        assert tool_calls[0]["arguments"] == {"arg1": "value1"}
        assert tool_calls[0]["status"] == "running"

    def test_tool_complete_updates_status(self) -> None:
        """Test tool completion updates status."""
        presenter = HeadlessPresenter()

        presenter.print_tool_start("test_tool", {})
        presenter.print_tool_complete("test_tool", "result", 100.0)

        tool_calls = presenter.get_tool_calls()
        assert tool_calls[0]["status"] == "complete"
        assert tool_calls[0]["result"] == "result"
        assert tool_calls[0]["duration_ms"] == 100.0

    @pytest.mark.asyncio
    async def test_tool_approval_auto_approve(self) -> None:
        """Test tool approval with auto-approve enabled."""
        presenter = HeadlessPresenter(auto_approve=True)

        result = await presenter.prompt_tool_approval("test", {}, is_sensitive=True)

        assert result == ToolApproval.ALLOW

    @pytest.mark.asyncio
    async def test_tool_approval_auto_deny(self) -> None:
        """Test tool approval with auto-approve disabled."""
        presenter = HeadlessPresenter(auto_approve=False)

        result = await presenter.prompt_tool_approval("test", {}, is_sensitive=True)

        assert result == ToolApproval.DENY

    def test_is_sensitive_tool_bash(self) -> None:
        """Test bash.execute is marked as sensitive."""
        presenter = HeadlessPresenter()
        assert presenter.is_sensitive_tool("bash.execute", {})

    def test_is_sensitive_tool_write_file(self) -> None:
        """Test filesystem.write_file is marked as sensitive."""
        presenter = HeadlessPresenter()
        assert presenter.is_sensitive_tool("filesystem.write_file", {})

    def test_is_sensitive_tool_dangerous_pattern(self) -> None:
        """Test dangerous patterns in arguments are sensitive."""
        presenter = HeadlessPresenter()
        assert presenter.is_sensitive_tool("some_tool", {"cmd": "sudo rm -rf /"})
        assert presenter.is_sensitive_tool("some_tool", {"script": "chmod 777 file"})

    def test_is_not_sensitive_tool_safe(self) -> None:
        """Test safe tools are not marked as sensitive."""
        presenter = HeadlessPresenter()
        assert not presenter.is_sensitive_tool("filesystem.read_file", {"path": "/tmp/test"})

    # === Clear Captured ===

    def test_clear_captured(self) -> None:
        """Test clearing all captured output."""
        presenter = HeadlessPresenter()

        presenter.start_generation()
        presenter.on_stream_chunk("test")
        presenter.add_message("user", "msg")
        presenter.print_info("info")
        presenter.print_warning("warn")
        presenter.print_error("error")
        presenter.print_tool_start("tool", {})

        presenter.clear_captured()

        assert presenter.get_stream_content() == ""
        assert presenter.get_messages() == []
        assert presenter.get_info_messages() == []
        assert presenter.get_warning_messages() == []
        assert presenter.get_error_messages() == []
        assert presenter.get_tool_calls() == []

    # === Event Loop ===

    @pytest.mark.asyncio
    async def test_exit_stops_run_loop(self) -> None:
        """Test exit() stops the run_async loop."""
        presenter = HeadlessPresenter()

        async def stop_after_delay() -> None:
            await asyncio.sleep(0.05)
            presenter.exit()

        # Start both tasks
        asyncio.create_task(stop_after_delay())
        await asyncio.wait_for(presenter.run_async(), timeout=1.0)

        assert not presenter._running

    @pytest.mark.asyncio
    async def test_send_input_processes_callback(self) -> None:
        """Test send_input triggers the input callback."""
        presenter = HeadlessPresenter()
        received_input: list[str] = []

        async def input_callback(text: str) -> None:
            received_input.append(text)
            presenter.exit()

        presenter.set_input_callback(input_callback)

        # Start run_async and send input
        async def send_after_delay() -> None:
            await asyncio.sleep(0.05)
            await presenter.send_input("test message")

        asyncio.create_task(send_after_delay())
        await asyncio.wait_for(presenter.run_async(), timeout=1.0)

        assert received_input == ["test message"]

    # === Callbacks ===

    def test_interrupt_callback(self) -> None:
        """Test interrupt callback is called."""
        presenter = HeadlessPresenter()
        interrupted = []

        def on_interrupt() -> None:
            interrupted.append(True)

        presenter.set_interrupt_callback(on_interrupt)
        presenter.interrupt()

        assert interrupted == [True]

    def test_pause_callback(self) -> None:
        """Test pause callback is called."""
        presenter = HeadlessPresenter()
        paused = []

        def on_pause() -> None:
            paused.append(True)

        presenter.set_pause_callback(on_pause)
        presenter.pause()

        assert paused == [True]

    # === Injection ===

    @pytest.mark.asyncio
    async def test_prompt_injection_returns_none(self) -> None:
        """Test prompt_injection returns None in headless mode."""
        presenter = HeadlessPresenter()

        result = await presenter.prompt_injection("partial content")

        assert result is None

    # === Context Feedback (no-op but shouldn't error) ===

    def test_context_methods_dont_error(self) -> None:
        """Test context feedback methods don't raise errors."""
        from entropi.core.compaction import CompactionResult
        from entropi.core.engine import AgentState
        from entropi.core.todos import TodoList
        from entropi.ui.presenter import StatusInfo

        presenter = HeadlessPresenter()

        # These should all complete without error
        presenter.update_state(AgentState.IDLE)
        presenter.print_context_usage(1000, 8000)
        presenter.print_compaction_notice(
            CompactionResult(
                compacted=True,
                old_token_count=5000,
                new_token_count=2000,
            )
        )
        presenter.print_todo_panel(TodoList())
        presenter.print_status(
            StatusInfo(
                model="test",
                vram_used=4.0,
                vram_total=8.0,
                tokens=1000,
                thinking_mode=False,
                context_used=2000,
                context_max=8000,
            )
        )
