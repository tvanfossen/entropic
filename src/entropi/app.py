"""
Application orchestrator.

Coordinates all components and manages the application lifecycle.
"""

import asyncio
import re
from pathlib import Path
from typing import Any

from rich.console import Console

from entropi import __version__
from entropi.config.schema import EntropyConfig
from entropi.core.base import Message
from entropi.core.commands import CommandContext, CommandRegistry
from entropi.core.context import ProjectContext
from entropi.core.engine import AgentEngine, EngineCallbacks, LoopConfig
from entropi.core.logging import get_logger
from entropi.core.queue import MessageQueue, MessageSource, QueuedMessage
from entropi.core.session import SessionManager
from entropi.core.tasks import TaskManager
from entropi.inference.orchestrator import ModelOrchestrator
from entropi.mcp.manager import ServerManager
from entropi.mcp.servers.external import ExternalMCPServer
from entropi.storage.backend import SQLiteStorage
from entropi.ui.presenter import Presenter, StatusInfo


class Application:
    """Main application orchestrator."""

    def __init__(
        self,
        config: EntropyConfig,
        project_dir: Path | None = None,
        presenter: Presenter | None = None,
        orchestrator: ModelOrchestrator | None = None,
    ) -> None:
        """
        Initialize application.

        Args:
            config: Application configuration
            project_dir: Project directory
            presenter: Optional presenter for UI (defaults to TUIPresenter)
            orchestrator: Optional pre-loaded orchestrator (for test reuse)
        """
        self.config = config
        self.project_dir = project_dir or Path.cwd()
        self.logger = get_logger("app")
        self.console = Console()

        # Components (initialized lazily)
        self._orchestrator: ModelOrchestrator | None = orchestrator
        self._orchestrator_owned = orchestrator is None  # Only shutdown if we created it
        self._mcp_manager: ServerManager | None = None
        self._storage: SQLiteStorage | None = None
        self._presenter: Presenter | None = presenter
        self._engine: AgentEngine | None = None
        self._command_registry: CommandRegistry | None = None
        self._project_context: ProjectContext | None = None

        # External MCP server components (for Claude Code integration)
        self._message_queue: MessageQueue | None = None
        self._task_manager: TaskManager | None = None
        self._session_manager: SessionManager | None = None
        self._external_mcp: ExternalMCPServer | None = None
        self._external_mcp_task: asyncio.Task[None] | None = None

        # Session state
        self._conversation_id: str | None = None
        self._messages: list[Message] = []
        self._thinking_mode: bool = config.thinking.enabled

    async def initialize(self) -> None:
        """Initialize all components with loading feedback."""
        from rich.status import Status

        self.logger.info("Initializing Entropi...")

        with Status("[bold blue]Starting Entropi...", console=self.console) as status:
            # Initialize model orchestrator (skip if injected)
            if self._orchestrator is None:
                status.update("[bold blue]Loading models...")
                self._orchestrator = ModelOrchestrator(self.config)
                await self._orchestrator.initialize()

            # Initialize MCP server manager
            status.update("[bold blue]Starting tool servers...")
            self._mcp_manager = ServerManager(self.config)
            await self._mcp_manager.initialize()

            # Initialize storage
            status.update("[bold blue]Connecting to database...")
            db_path = self.config.config_dir / "entropi.db"
            self._storage = SQLiteStorage(db_path)
            await self._storage.initialize()

            # Initialize project context
            status.update("[bold blue]Loading project context...")
            self._project_context = ProjectContext(self.project_dir)
            await self._project_context.load()

            # Initialize command registry
            status.update("[bold blue]Discovering commands...")
            self._command_registry = CommandRegistry(
                self.project_dir,
                self.config.config_dir / "commands",
            )
            await self._command_registry.discover()

            # Initialize agent engine
            status.update("[bold blue]Initializing agent...")
            loop_config = LoopConfig(
                max_iterations=15,
                max_consecutive_errors=3,
                stream_output=True,
                auto_approve_tools=self.config.permissions.auto_approve,
            )
            self._engine = AgentEngine(
                orchestrator=self._orchestrator,
                server_manager=self._mcp_manager,
                config=self.config,
                loop_config=loop_config,
            )

            # Initialize external MCP server for Claude Code integration
            if self.config.mcp.external.enabled:
                status.update("[bold blue]Starting external MCP server...")
                await self._initialize_external_mcp()

        self.logger.info("Entropi initialized")

    async def _initialize_external_mcp(self) -> None:
        """Initialize external MCP server for Claude Code integration."""
        # Create message queue
        self._message_queue = MessageQueue()

        # Create task manager
        self._task_manager = TaskManager()

        # Create session manager with its own database
        session_db = self.config.config_dir / "sessions.db"
        self._session_manager = SessionManager(session_db)

        # Create external MCP server
        self._external_mcp = ExternalMCPServer(
            config=self.config,
            message_queue=self._message_queue,
            task_manager=self._task_manager,
            session_manager=self._session_manager,
        )

        # Start the socket server in the background
        self._external_mcp_task = asyncio.create_task(
            self._external_mcp.start(),
            name="external-mcp-server",
        )

        self.logger.info(f"External MCP server starting on {self.config.mcp.external.socket_path}")

    @staticmethod
    def _strip_thinking_blocks(content: str) -> str:
        """Strip <think>...</think> blocks from content for MCP responses.

        Claude Code doesn't need the full thinking process - just the result.
        """
        # Remove <think>...</think> blocks (handles multiline)
        cleaned = re.sub(r"<think>.*?</think>", "", content, flags=re.DOTALL)
        # Clean up any resulting double newlines
        cleaned = re.sub(r"\n{3,}", "\n\n", cleaned)
        return cleaned.strip()

    async def _process_queued_message(self, queued_msg: QueuedMessage) -> None:
        """Process a message from the MCP queue through the agent loop.

        This runs as a Textual worker (started by presenter), so it has proper
        app context and can use direct presenter calls and modal dialogs.
        """
        assert self._engine is not None
        assert self._task_manager is not None
        assert self._presenter is not None

        # Capture presenter in local variable for closures
        presenter = self._presenter

        # Get the associated task
        task = self._task_manager.get_task(queued_msg.task_id) if queued_msg.task_id else None

        # Mark task as in-progress
        if task:
            self._task_manager.start_task(task.id)

        # Display with source indicator
        source_label = "[Claude Code] " if queued_msg.source == MessageSource.CLAUDE_CODE else ""
        presenter.add_message("user", f"{source_label}{queued_msg.content}")

        # Collect response for task completion
        response_content = ""

        def on_chunk(chunk: str) -> None:
            """Handle streaming chunk."""
            nonlocal response_content
            response_content += chunk
            presenter.on_stream_chunk(chunk)

        # Set up callbacks (same as _process_message, tool approval follows config)
        self._engine.set_callbacks(
            EngineCallbacks(
                on_state_change=lambda s: presenter.update_state(s),
                on_tool_call=self._handle_tool_approval,
                on_stream_chunk=on_chunk,
                on_tool_start=lambda tc: presenter.print_tool_start(tc.name, tc.arguments),
                on_tool_complete=lambda tc, r, d: presenter.print_tool_complete(tc.name, r, d),
                on_tier_selected=lambda t: presenter.set_tier(t),
            )
        )

        # Process through engine
        try:
            # Mark generation active
            presenter.start_generation()

            new_messages: list[Message] = []
            async for msg in self._engine.run(
                queued_msg.content,
                history=self._messages,
                task_id=queued_msg.task_id,
                source=queued_msg.source,
            ):
                new_messages.append(msg)
                # Add messages to shared conversation history
                if msg not in self._messages:
                    self._messages.append(msg)

            # Handle successful completion
            self._complete_queued_message(queued_msg, response_content, task, presenter)

        except Exception as e:
            self.logger.exception(f"Error in queued message processing: {e}")
            # Mark task as failed
            if queued_msg.task_id and self._task_manager:
                self._task_manager.fail_task(queued_msg.task_id, str(e))
            # Invoke callback with error
            if queued_msg.callback:
                try:
                    queued_msg.callback({"error": str(e)})
                except Exception:
                    pass
            raise
        finally:
            # Mark generation complete
            presenter.end_generation()

    def _complete_queued_message(
        self,
        queued_msg: QueuedMessage,
        response_content: str,
        task: Any,
        presenter: Presenter,
    ) -> None:
        """Handle successful completion of a queued message."""
        # Strip thinking blocks for MCP response (Claude Code doesn't need them)
        clean_response = self._strip_thinking_blocks(response_content)

        # Mark task as completed
        if task:
            self._task_manager.complete_task(task.id, clean_response)

        # Show context usage after each response
        if self._engine:
            context_used = self._engine._token_counter.count_messages(self._messages)
            context_max = self._engine._token_counter.max_tokens
            presenter.print_context_usage(context_used, context_max)

        # Invoke callback with result
        if queued_msg.callback:
            queued_msg.callback({"response": clean_response})

    async def shutdown(self) -> None:
        """Shutdown all components."""
        self.logger.info("Shutting down...")

        # Note: Queue consumer worker is managed by Textual and stops with the app

        # Stop external MCP server
        if self._external_mcp:
            await self._external_mcp.stop()
        if self._external_mcp_task:
            self._external_mcp_task.cancel()
            try:
                await self._external_mcp_task
            except asyncio.CancelledError:
                pass

        # Shutdown in reverse order
        if self._mcp_manager:
            await self._mcp_manager.shutdown()

        if self._storage:
            await self._storage.close()

        if self._orchestrator and self._orchestrator_owned:
            await self._orchestrator.shutdown()

        self.logger.info("Shutdown complete")

    async def run(self) -> None:
        """Run the interactive application using presenter."""
        try:
            await self.initialize()

            # Get available models
            models = self._orchestrator.get_available_models() if self._orchestrator else []

            # Create new conversation
            if self._storage:
                self._conversation_id = await self._storage.create_conversation(
                    title="New Conversation",
                    project_path=str(self.project_dir),
                )

            # Create default TUI presenter if none provided
            if self._presenter is None:
                from entropi.ui.tui_presenter import TUIPresenter

                self._presenter = TUIPresenter(
                    config=self.config,
                    version=__version__,
                    models=models,
                )

            # Wire up callbacks (presenter is guaranteed non-None after above block)
            assert self._presenter is not None
            presenter = self._presenter
            presenter.set_input_callback(self._handle_user_input)
            presenter.set_interrupt_callback(self._handle_interrupt)
            presenter.set_pause_callback(self._handle_pause)

            # Set voice mode callbacks to manage VRAM
            presenter.set_voice_callbacks(
                on_enter=self._on_voice_enter,
                on_exit=self._on_voice_exit,
            )

            # Set up queue consumer for MCP messages (runs as Textual worker)
            if self._message_queue is not None:
                presenter.set_queue_consumer(
                    queue=self._message_queue,
                    process_callback=self._process_queued_message,
                )

            # Run the presenter (this blocks until exit)
            await presenter.run_async()

        except KeyboardInterrupt:
            pass  # Normal exit
        except Exception as e:
            self.logger.exception(f"Application error: {e}")
            raise
        finally:
            await self.shutdown()

    async def _handle_user_input(self, user_input: str) -> None:
        """
        Handle user input from the Textual UI.

        This is called by the Textual app when user submits input.
        """
        assert self._presenter is not None
        assert self._command_registry is not None

        if not user_input.strip():
            return

        # Check for slash commands
        if self._command_registry.is_command(user_input):
            should_continue = await self._handle_command(user_input)
            if not should_continue:
                self._presenter.exit()
            return

        # Process user message
        await self._process_message(user_input)

    async def _handle_command(self, command_str: str) -> bool:
        """
        Handle a slash command.

        Args:
            command_str: Command string

        Returns:
            True to continue loop, False to exit
        """
        assert self._command_registry is not None
        assert self._presenter is not None

        context = CommandContext(
            app=self,
            conversation_id=self._conversation_id,
            project_dir=self.project_dir,
            config=self.config,
        )

        result = await self._command_registry.execute(command_str, context)

        if result.message:
            if result.success:
                self._presenter.print_info(result.message)
            else:
                self._presenter.print_error(result.message)

        # Handle special actions
        if result.data:
            await self._handle_command_action(result.data)

        return result.should_continue

    async def _handle_command_action(self, data: dict[str, Any]) -> None:
        """Handle command action data."""
        action = data.get("action")
        handlers: dict[str, Any] = {
            "clear_history": self._action_clear_history,
            "show_status": self._show_status,
            "set_thinking_mode": self._action_set_thinking_mode,
            "show_thinking_status": self._action_show_thinking_status,
            "switch_model": self._action_switch_model,
            "save_conversation": lambda d: self._save_conversation(d.get("name")),
            "load_conversation": self._action_load_conversation,
            "list_sessions": lambda _: self._list_sessions(),
            "new_session": lambda d: self._new_session(d.get("name", "New session")),
            "switch_session": lambda d: self._switch_session(d.get("session_id")),
            "rename_session": lambda d: self._rename_session(d.get("name")),
            "delete_session": lambda d: self._delete_session(d.get("session_id")),
            "export_session": lambda d: self._export_session(d.get("session_id")),
        }
        if action and (handler := handlers.get(action)):
            result = handler(data)
            if asyncio.iscoroutine(result):
                await result

    async def _action_clear_history(self, _data: dict[str, Any]) -> None:
        """Clear conversation history."""
        self._messages = []
        if self._storage and self._conversation_id:
            self._conversation_id = await self._storage.create_conversation(
                title="New Conversation",
                project_path=str(self.project_dir),
            )

    async def _action_set_thinking_mode(self, data: dict[str, Any]) -> None:
        """Set thinking mode enabled/disabled."""
        enabled = data.get("enabled", False)
        if not self._orchestrator:
            if self._presenter:
                self._presenter.print_error("Orchestrator not available")
            return
        success = await self._orchestrator.set_thinking_mode(enabled)
        if not self._presenter:
            return
        if success:
            self._thinking_mode = enabled
            mode = "enabled" if enabled else "disabled"
            self._presenter.print_info(f"Thinking mode {mode}")
        else:
            self._presenter.print_error("Thinking model not configured")

    def _action_show_thinking_status(self, _data: dict[str, Any]) -> None:
        """Show current thinking mode status."""
        if not self._presenter:
            return
        if self._thinking_mode:
            self._presenter.print_info("Thinking: ON (forced for all reasoning)")
        else:
            self._presenter.print_info("Thinking: AUTO (complex tasks only)")

    def _action_switch_model(self, data: dict[str, Any]) -> None:
        """Switch the default model."""
        model = data.get("model")
        if model and self._orchestrator and self._presenter:
            self.config.models.default = model
            self._presenter.print_info(f"Switched to {model} model")

    async def _action_load_conversation(self, data: dict[str, Any]) -> None:
        """Load a conversation by name."""
        name = data.get("name")
        if name:
            await self._load_conversation(name)

    async def _process_message(self, user_input: str) -> None:
        """Process a user message through the agent loop."""
        assert self._presenter is not None
        assert self._engine is not None

        # Capture presenter in local variable for closures
        presenter = self._presenter

        # Build system prompt with project context
        system_prompt = None
        if self._project_context and self._project_context.has_context:
            system_prompt = self._project_context.get_system_prompt_addition()

        def on_chunk(chunk: str) -> None:
            """Handle streaming chunk - pass directly to presenter."""
            presenter.on_stream_chunk(chunk)

        def on_tool_start(tool_call: Any) -> None:
            """Handle tool execution start."""
            presenter.print_tool_start(tool_call.name, tool_call.arguments)

        def on_tool_complete(tool_call: Any, result: str, duration_ms: float) -> None:
            """Handle tool execution complete."""
            presenter.print_tool_complete(tool_call.name, result, duration_ms)

        def on_todo_update(todo_list: Any) -> None:
            """Handle todo list update."""
            presenter.print_todo_panel(todo_list)

        def on_compaction(result: Any) -> None:
            """Handle context compaction."""
            presenter.print_compaction_notice(result)

        self._engine.set_callbacks(
            EngineCallbacks(
                on_state_change=lambda s: presenter.update_state(s),
                on_tool_call=self._handle_tool_approval,
                on_stream_chunk=on_chunk,
                on_tool_start=on_tool_start,
                on_tool_complete=on_tool_complete,
                on_todo_update=on_todo_update,
                on_compaction=on_compaction,
                on_pause_prompt=self._handle_pause_prompt,
                on_tier_selected=lambda t: presenter.set_tier(t),
            )
        )

        # Run agent loop
        try:
            # Mark generation active (enables Escape to pause)
            self._presenter.start_generation()

            # Add user message to history FIRST (before generation)
            # This ensures it's preserved even if interrupted
            self._messages.append(Message(role="user", content=user_input))

            new_messages: list[Message] = []
            async for msg in self._engine.run(
                user_input,
                history=self._messages[:-1],  # Pass history without the just-added user message
                system_prompt=system_prompt,
            ):
                new_messages.append(msg)
                # Add messages as they arrive (preserves partial state on interrupt)
                if msg not in self._messages:
                    self._messages.append(msg)

            # Show context usage after each response
            if self._engine:
                context_used = self._engine._token_counter.count_messages(self._messages)
                context_max = self._engine._token_counter.max_tokens
                self._presenter.print_context_usage(context_used, context_max)

            # Save to storage
            if self._storage and self._conversation_id:
                await self._storage.save_conversation(
                    self._conversation_id,
                    [Message(role="user", content=user_input)] + new_messages,
                )

        except Exception as e:
            self._presenter.print_error(f"Generation error: {e}")
        finally:
            # Mark generation complete
            self._presenter.end_generation()

    async def _handle_tool_approval(self, tool_call: Any) -> Any:
        """
        Handle tool approval request.

        If auto_approve is enabled, approve all tools.
        Otherwise, prompt the user for approval on sensitive tools.

        Returns ToolApproval enum for new behavior, or bool for legacy.
        """
        from entropi.core.engine import ToolApproval

        # Auto-approve if configured or no presenter or non-sensitive
        should_auto_approve = (
            self.config.permissions.auto_approve
            or not self._presenter
            or not self._presenter.is_sensitive_tool(tool_call.name, tool_call.arguments)
        )
        if should_auto_approve:
            return ToolApproval.ALLOW

        # For sensitive tools, prompt the user via modal
        return await self._presenter.prompt_tool_approval(
            tool_call.name,
            tool_call.arguments,
            is_sensitive=True,
        )

    def _handle_interrupt(self) -> None:
        """Handle interrupt signal (hard cancel)."""
        if self._engine:
            self._engine.interrupt()

    def _handle_pause(self) -> None:
        """Handle pause signal (Escape key during generation).

        Note: For now, pause acts as interrupt since the pause modal
        requires cross-thread coordination that causes event loop issues.
        TODO: Implement proper pause/inject with thread-safe modal display.
        """
        if self._engine:
            # For now, just interrupt - the pause modal causes event loop issues
            # because prompt_injection uses push_screen_wait which must run on
            # Textual's main thread, but engine.run() is in a worker thread
            self._engine.interrupt()

    async def _on_voice_enter(self) -> None:
        """Handle entering voice mode - unload chat models to free VRAM."""
        if self._orchestrator:
            self.logger.info("Voice mode: unloading chat models")
            await self._orchestrator.unload_all_models()

    async def _on_voice_exit(self) -> None:
        """Handle exiting voice mode - reload chat models."""
        if self._orchestrator:
            self.logger.info("Voice mode: reloading chat models")
            await self._orchestrator.reload_default_models()

    async def _handle_pause_prompt(self, partial_content: str) -> str | None:
        """
        Handle pause prompt - called by engine when generation is paused.

        Args:
            partial_content: The partial response generated so far

        Returns:
            User's injection text, empty string to resume, or None to cancel
        """
        if not self._presenter:
            return None

        return await self._presenter.prompt_injection(partial_content)

    async def _show_status(self, _data: dict[str, Any] | None = None) -> None:
        """Show system status."""
        if not self._presenter:
            return

        vram_used = 0.0
        vram_total = 0.0

        # Get model info
        model = self.config.models.default
        if self._thinking_mode:
            model = "thinking"

        # Get token count from current conversation
        tokens = sum(len(m.content) // 4 for m in self._messages)

        # Get context info from engine
        context_used = 0
        context_max = 16384
        if self._engine:
            context_used = self._engine._token_counter.count_messages(self._messages)
            context_max = self._engine._token_counter.max_tokens

        self._presenter.print_status(
            StatusInfo(
                model=model,
                vram_used=vram_used,
                vram_total=vram_total,
                tokens=tokens,
                thinking_mode=self._thinking_mode,
                context_used=context_used,
                context_max=context_max,
            )
        )

    async def _save_conversation(self, name: str | None) -> None:
        """Save current conversation."""
        if not self._storage or not self._conversation_id:
            if self._presenter:
                self._presenter.print_error("No conversation to save")
            return

        if name:
            await self._storage.update_conversation_title(self._conversation_id, name)

        if self._presenter:
            self._presenter.print_info(f"Conversation saved: {name or self._conversation_id}")

    async def _load_conversation(self, name: str) -> None:
        """Load a conversation by name/ID."""
        if not self._storage or not self._presenter:
            return

        # Search for conversation
        conversations = await self._storage.list_conversations()
        match = None
        for conv in conversations:
            if conv["title"] == name or conv["id"] == name:
                match = conv
                break

        if not match:
            self._presenter.print_error(f"Conversation not found: {name}")
            return

        # Load messages
        messages, _ = await self._storage.load_conversation(match["id"])
        self._messages = messages
        self._conversation_id = match["id"]
        self._presenter.print_info(f"Loaded conversation: {match['title']}")

    async def _list_sessions(self) -> None:
        """List all sessions (conversations) for this project."""
        if not self._storage or not self._presenter:
            return

        conversations = await self._storage.list_conversations()

        if not conversations:
            self._presenter.print_info("No sessions found. Use /new to create one.")
            return

        lines = ["**Sessions:**\n"]
        for conv in conversations:
            marker = " (current)" if conv["id"] == self._conversation_id else ""
            msg_count = conv.get("message_count", 0)
            lines.append(f"  `{conv['id'][:8]}` - {conv['title']} ({msg_count} messages){marker}")

        self._presenter.print_info("\n".join(lines))

    async def _new_session(self, name: str) -> None:
        """Create a new session and switch to it."""
        if not self._storage or not self._presenter:
            return

        # Clear current messages
        self._messages = []

        # Create new conversation
        self._conversation_id = await self._storage.create_conversation(
            title=name,
            project_path=str(self.project_dir),
        )

        self._presenter.print_info(f"Created new session: {name}")

    async def _switch_session(self, session_id: str | None) -> None:
        """Switch to an existing session."""
        if not self._storage or not self._presenter or not session_id:
            if self._presenter:
                self._presenter.print_error("Session ID required")
            return

        # Find the conversation (support partial IDs)
        conversations = await self._storage.list_conversations()
        match = None
        for conv in conversations:
            if conv["id"].startswith(session_id):
                match = conv
                break

        if not match:
            self._presenter.print_error(f"Session not found: {session_id}")
            return

        # Load messages
        messages, _ = await self._storage.load_conversation(match["id"])
        self._messages = messages
        self._conversation_id = match["id"]
        self._presenter.print_info(f"Switched to: {match['title']}")

    async def _rename_session(self, name: str | None) -> None:
        """Rename the current session."""
        if not self._storage or not self._presenter:
            return

        if not name:
            self._presenter.print_error("Name required")
            return

        if not self._conversation_id:
            self._presenter.print_error("No active session")
            return

        await self._storage.update_conversation_title(self._conversation_id, name)
        self._presenter.print_info(f"Renamed to: {name}")

    async def _delete_session(self, session_id: str | None) -> None:
        """Delete a session."""
        if not self._storage or not self._presenter or not session_id:
            if self._presenter:
                self._presenter.print_error("Session ID required")
            return

        # Don't allow deleting current session
        if self._conversation_id and self._conversation_id.startswith(session_id):
            self._presenter.print_error("Cannot delete current session. Switch to another first.")
            return

        # Find the conversation
        conversations = await self._storage.list_conversations()
        match = None
        for conv in conversations:
            if conv["id"].startswith(session_id):
                match = conv
                break

        if not match:
            self._presenter.print_error(f"Session not found: {session_id}")
            return

        await self._storage.delete_conversation(match["id"])
        self._presenter.print_info(f"Deleted session: {match['title']}")

    async def _export_session(self, session_id: str | None) -> None:
        """Export a session to markdown."""
        if not self._storage or not self._presenter:
            return

        # Use current session if no ID provided
        conv_id = session_id or self._conversation_id
        if not conv_id:
            self._presenter.print_error("No session to export")
            return

        # Find conversation
        conversations = await self._storage.list_conversations()
        match = None
        for conv in conversations:
            if conv["id"].startswith(conv_id):
                match = conv
                break

        if not match:
            self._presenter.print_error(f"Session not found: {conv_id}")
            return

        # Load messages
        messages, _ = await self._storage.load_conversation(match["id"])

        # Format as markdown
        lines = [f"# {match['title']}\n"]
        for msg in messages:
            role = msg.role.capitalize()
            lines.append(f"## {role}\n")
            lines.append(msg.content)
            lines.append("\n")

        markdown = "\n".join(lines)
        self.console.print(markdown)

    async def single_turn(self, message: str, stream: bool = True) -> None:  # noqa: ARG002
        """
        Process a single message and exit.

        Uses the full AgentEngine with tool support for accurate responses.

        Args:
            message: User message
            stream: Whether to stream output (currently ignored, always streams)
        """
        _ = stream  # Currently always streams
        try:
            await self.initialize()

            # Check if models are available
            if not self._orchestrator or not self._orchestrator.get_available_models():
                self.console.print(f"[dim]You: {message}[/dim]")
                self.console.print("\n[yellow]No models configured.[/yellow]")
                self.console.print(
                    "[dim]Configure models in ~/.entropi/config.yaml or .entropi/config.yaml[/dim]"
                )
                return

            self.console.print(f"[dim]You: {message}[/dim]\n")

            # Use the AgentEngine for full tool support
            assert self._engine is not None

            # Set up streaming callback
            self.console.print("[bold]A:[/bold] ", end="")
            current_content = ""

            def on_chunk(chunk: str) -> None:
                nonlocal current_content
                current_content += chunk
                self.console.print(chunk, end="")

            def on_tool_start(tool_call: Any) -> None:
                args_str = ", ".join(f"{k}={repr(v)[:30]}" for k, v in tool_call.arguments.items())
                if len(args_str) > 60:
                    args_str = args_str[:57] + "..."
                self.console.print(f"\n[dim]Executing {tool_call.name}({args_str})...[/dim]")

            def on_tool_complete(tool_call: Any, result: str, duration_ms: float) -> None:
                result_len = len(result)
                if result_len > 100:
                    summary = f"{result_len} chars"
                else:
                    summary = result[:50].replace("\n", " ")
                    if len(result) > 50:
                        summary += "..."
                self.console.print(
                    f"[green]Done[/green] {tool_call.name} [dim]({duration_ms:.0f}ms, {summary})[/dim]\n"
                )

            self._engine.set_callbacks(
                EngineCallbacks(
                    on_stream_chunk=on_chunk,
                    on_tool_start=on_tool_start,
                    on_tool_complete=on_tool_complete,
                )
            )

            # Run agent loop (tool results are shown via on_tool_complete callback)
            async for _ in self._engine.run(message):
                pass

            self.console.print()  # Newline after response

        finally:
            await self.shutdown()
