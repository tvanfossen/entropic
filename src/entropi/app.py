"""
Application orchestrator.

Coordinates all components and manages the application lifecycle.
"""

import asyncio
from pathlib import Path
from typing import Any

from rich.console import Console

from entropi import __version__
from entropi.config.schema import EntropyConfig
from entropi.core.base import Message
from entropi.core.commands import CommandContext, CommandRegistry
from entropi.core.context import ContextBuilder, ProjectContext
from entropi.core.engine import AgentEngine, AgentState, LoopConfig
from entropi.core.logging import get_logger
from entropi.inference.orchestrator import ModelOrchestrator
from entropi.mcp.manager import ServerManager
from entropi.storage.backend import SQLiteStorage
from entropi.ui.terminal import TerminalUI


class Application:
    """Main application orchestrator."""

    def __init__(
        self,
        config: EntropyConfig,
        project_dir: Path | None = None,
    ) -> None:
        """
        Initialize application.

        Args:
            config: Application configuration
            project_dir: Project directory
        """
        self.config = config
        self.project_dir = project_dir or Path.cwd()
        self.logger = get_logger("app")
        self.console = Console()

        # Components (initialized lazily)
        self._orchestrator: ModelOrchestrator | None = None
        self._mcp_manager: ServerManager | None = None
        self._storage: SQLiteStorage | None = None
        self._ui: TerminalUI | None = None
        self._engine: AgentEngine | None = None
        self._command_registry: CommandRegistry | None = None
        self._project_context: ProjectContext | None = None

        # Session state
        self._conversation_id: str | None = None
        self._messages: list[Message] = []
        self._thinking_mode: bool = config.thinking.enabled

    async def initialize(self) -> None:
        """Initialize all components with loading feedback."""
        from rich.status import Status

        self.logger.info("Initializing Entropi...")

        with Status("[bold blue]Starting Entropi...", console=self.console) as status:
            # Initialize model orchestrator
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

        self.logger.info("Entropi initialized")

    async def shutdown(self) -> None:
        """Shutdown all components."""
        self.logger.info("Shutting down...")

        # Shutdown in reverse order
        if self._mcp_manager:
            await self._mcp_manager.shutdown()

        if self._storage:
            await self._storage.close()

        if self._orchestrator:
            await self._orchestrator.shutdown()

        self.logger.info("Shutdown complete")

    async def run(self) -> None:
        """Run the interactive application."""
        try:
            await self.initialize()

            # Initialize terminal UI
            self._ui = TerminalUI(self.config)
            self._ui.set_interrupt_callback(self._handle_interrupt)

            # Print welcome
            models = self._orchestrator.get_available_models() if self._orchestrator else []
            self._ui.print_welcome(__version__, models)

            # Create new conversation
            if self._storage:
                self._conversation_id = await self._storage.create_conversation(
                    title="New Conversation",
                    project_path=str(self.project_dir),
                )

            # Main loop
            await self._interactive_loop()

        except KeyboardInterrupt:
            if self._ui:
                self._ui.print_warning("Interrupted")
        except Exception as e:
            self.logger.error(f"Application error: {e}")
            if self._ui:
                self._ui.print_error(str(e))
        finally:
            await self.shutdown()

    async def _interactive_loop(self) -> None:
        """Main interactive loop."""
        assert self._ui is not None
        assert self._command_registry is not None

        last_interrupt_time = 0.0

        while True:
            try:
                # Get user input
                user_input = await self._ui.get_input("> ")

                if user_input is None or not user_input.strip():
                    continue

                # Check for exit
                if user_input.strip().lower() in ("/exit", "/quit", "/q"):
                    self._ui.print_info("Goodbye!")
                    break

                # Check for slash commands
                if self._command_registry.is_command(user_input):
                    should_continue = await self._handle_command(user_input)
                    if not should_continue:
                        break
                    continue

                # Process user message
                await self._process_message(user_input)

            except EOFError:
                break
            except KeyboardInterrupt:
                import time
                now = time.time()
                if now - last_interrupt_time < 1.0:
                    # Double Ctrl+C within 1 second - exit
                    self._ui.print_info("Goodbye!")
                    break
                last_interrupt_time = now
                self._ui.print_warning("Press Ctrl+C again to exit")
                continue

    async def _handle_command(self, command_str: str) -> bool:
        """
        Handle a slash command.

        Args:
            command_str: Command string

        Returns:
            True to continue loop, False to exit
        """
        assert self._command_registry is not None
        assert self._ui is not None

        context = CommandContext(
            app=self,
            conversation_id=self._conversation_id,
            project_dir=self.project_dir,
            config=self.config,
        )

        result = await self._command_registry.execute(command_str, context)

        if result.message:
            if result.success:
                self._ui.print_info(result.message)
            else:
                self._ui.print_error(result.message)

        # Handle special actions
        if result.data:
            await self._handle_command_action(result.data)

        return result.should_continue

    async def _handle_command_action(self, data: dict[str, Any]) -> None:
        """Handle command action data."""
        action = data.get("action")

        if action == "clear_history":
            self._messages = []
            if self._storage and self._conversation_id:
                self._conversation_id = await self._storage.create_conversation(
                    title="New Conversation",
                    project_path=str(self.project_dir),
                )

        elif action == "show_status":
            await self._show_status()

        elif action == "set_thinking_mode":
            enabled = data.get("enabled", False)
            if self._orchestrator:
                success = await self._orchestrator.set_thinking_mode(enabled)
                if success:
                    self._thinking_mode = enabled
                    if self._ui:
                        mode = "enabled" if enabled else "disabled"
                        self._ui.print_info(f"Thinking mode {mode}")
                else:
                    if self._ui:
                        self._ui.print_error("Thinking model not configured")
            else:
                if self._ui:
                    self._ui.print_error("Orchestrator not available")

        elif action == "show_thinking_status":
            if self._ui:
                if self._thinking_mode:
                    self._ui.print_info("Thinking: ON (forced for all reasoning)")
                else:
                    self._ui.print_info("Thinking: AUTO (complex tasks only)")

        elif action == "switch_model":
            model = data.get("model")
            if model and self._orchestrator:
                self.config.models.default = model
                if self._ui:
                    self._ui.print_info(f"Switched to {model} model")

        elif action == "save_conversation":
            await self._save_conversation(data.get("name"))

        elif action == "load_conversation":
            await self._load_conversation(data.get("name"))

        # Session management actions
        elif action == "list_sessions":
            await self._list_sessions()

        elif action == "new_session":
            await self._new_session(data.get("name", "New session"))

        elif action == "switch_session":
            await self._switch_session(data.get("session_id"))

        elif action == "rename_session":
            await self._rename_session(data.get("name"))

        elif action == "delete_session":
            await self._delete_session(data.get("session_id"))

        elif action == "export_session":
            await self._export_session(data.get("session_id"))

    async def _process_message(self, user_input: str) -> None:
        """Process a user message through the agent loop."""
        assert self._ui is not None
        assert self._engine is not None

        # Build system prompt with project context
        system_prompt = None
        if self._project_context and self._project_context.has_context:
            system_prompt = self._project_context.get_system_prompt_addition()

        # Track streamed content - we buffer and display clean content
        full_content = ""
        displayed_length = 0
        streaming_active = False
        final_clean_content = ""  # Track clean content for final panel display
        in_think_block = False  # Track if we're inside a <think> block
        think_header_shown = False  # Track if we've shown the thinking header

        def on_chunk(chunk: str) -> None:
            nonlocal full_content, displayed_length, streaming_active, final_clean_content
            nonlocal in_think_block, think_header_shown

            if not streaming_active:
                streaming_active = True
                full_content = ""
                displayed_length = 0
                final_clean_content = ""
                in_think_block = False
                think_header_shown = False
                # Print assistant header for visual separation
                self.console.print(f"[bold {self._ui.theme.assistant_color}]Assistant[/]")

            full_content += chunk

            # Process content for display, handling think blocks with dim styling
            # We need to stream content char-by-char while tracking think block state

            # Stop markers - tool call patterns that halt streaming
            tool_markers = ["<tool_call>", "</tool_call>", '{"name":']

            # Find earliest tool marker to stop at
            stop_at = len(full_content)
            for marker in tool_markers:
                pos = full_content.find(marker)
                if pos != -1 and pos < stop_at:
                    stop_at = pos

            # Process content from displayed_length to stop_at
            # Handle think block transitions
            content_to_process = full_content[displayed_length:stop_at]

            output_buffer = ""
            i = 0
            while i < len(content_to_process):
                remaining = content_to_process[i:]

                # Check for <think> start tag
                if remaining.startswith("<think>"):
                    # Output any accumulated normal content first
                    if output_buffer:
                        self.console.print(output_buffer, end="", highlight=False)
                        final_clean_content += output_buffer
                        output_buffer = ""

                    # Enter think block with dim styling header
                    if not think_header_shown:
                        self.console.print("\n[dim]💭 [/]", end="")
                        think_header_shown = True
                    in_think_block = True
                    i += 7  # len("<think>")
                    continue

                # Check for </think> end tag
                if remaining.startswith("</think>"):
                    # Output any accumulated think content
                    if output_buffer:
                        self.console.print(f"[dim italic]{output_buffer}[/]", end="")
                        output_buffer = ""

                    # Exit think block - just newline, styling already closed above
                    self.console.print("\n", end="")
                    in_think_block = False
                    i += 8  # len("</think>")
                    continue

                # Check for partial tag at end (could be start of <think> or </think>)
                if i >= len(content_to_process) - 8:  # Near end, could be partial tag
                    potential_tag = remaining
                    if "<think>".startswith(potential_tag) or "</think>".startswith(potential_tag):
                        # Hold back this content, might be partial tag
                        break

                # Regular character - add to buffer
                output_buffer += content_to_process[i]
                i += 1

            # Output remaining buffer with appropriate styling
            if output_buffer:
                if in_think_block:
                    self.console.print(f"[dim italic]{output_buffer}[/]", end="")
                else:
                    self.console.print(output_buffer, end="", highlight=False)
                    final_clean_content += output_buffer

            # Update displayed_length to what we've actually processed
            displayed_length += i

        def on_tool_start(tool_call: Any) -> None:
            nonlocal streaming_active, full_content, displayed_length, final_clean_content
            # End streaming and show panel for any accumulated content
            if streaming_active and final_clean_content.strip():
                self.console.print()  # End streaming line
                # Don't show panel here - content already displayed inline
            streaming_active = False
            full_content = ""
            displayed_length = 0
            final_clean_content = ""
            self._ui.print_tool_start(tool_call.name, tool_call.arguments)

        def on_tool_complete(tool_call: Any, result: str, duration_ms: float) -> None:
            self._ui.print_tool_complete(tool_call.name, result, duration_ms)

        def on_todo_update(todo_list: Any) -> None:
            self._ui.print_todo_panel(todo_list)

        def on_compaction(result: Any) -> None:
            self._ui.print_compaction_notice(result)

        self._engine.set_callbacks(
            on_state_change=lambda s: self._ui.update_state(s) if self._ui else None,
            on_tool_call=self._handle_tool_approval,
            on_stream_chunk=on_chunk,
            on_tool_start=on_tool_start,
            on_tool_complete=on_tool_complete,
            on_todo_update=on_todo_update,
            on_compaction=on_compaction,
        )

        # Run agent loop
        try:
            new_messages: list[Message] = []
            async for msg in self._engine.run(
                user_input,
                history=self._messages,
                system_prompt=system_prompt,
            ):
                new_messages.append(msg)

            # End streaming with visual separator
            if streaming_active:
                self.console.print()  # End streaming line
                self.console.print()  # Blank line for separation

            # Update conversation history
            self._messages.append(Message(role="user", content=user_input))
            self._messages.extend(new_messages)

            # Show context usage after each response
            if self._engine:
                context_used = self._engine._token_counter.count_messages(self._messages)
                context_max = self._engine._token_counter.max_tokens
                self._ui.print_context_usage(context_used, context_max)

            # Save to storage
            if self._storage and self._conversation_id:
                await self._storage.save_conversation(
                    self._conversation_id,
                    [Message(role="user", content=user_input)] + new_messages,
                )

        except Exception as e:
            if streaming_active:
                self.console.print()
            self._ui.print_error(f"Generation error: {e}")

    async def _handle_tool_approval(self, tool_call: Any) -> Any:
        """
        Handle tool approval request.

        If auto_approve is enabled, approve all tools.
        Otherwise, prompt the user for approval on sensitive tools.

        Returns ToolApproval enum for new behavior, or bool for legacy.
        """
        from entropi.core.engine import ToolApproval

        if self.config.permissions.auto_approve:
            return ToolApproval.ALLOW

        if not self._ui:
            return ToolApproval.ALLOW

        # Check if this is a sensitive tool
        is_sensitive = self._ui.is_sensitive_tool(tool_call.name, tool_call.arguments)

        # For non-sensitive tools, auto-approve
        if not is_sensitive:
            return ToolApproval.ALLOW

        # For sensitive tools, prompt the user
        result = await self._ui.prompt_tool_approval(
            tool_call.name,
            tool_call.arguments,
            is_sensitive=True,
        )

        # Handle feedback string as denial
        if isinstance(result, str):
            # User provided feedback, treat as denial
            return ToolApproval.DENY

        return result

    def _handle_interrupt(self) -> None:
        """Handle interrupt signal."""
        if self._engine:
            self._engine.interrupt()

    async def _show_status(self) -> None:
        """Show system status."""
        if not self._ui:
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

        self._ui.print_status(
            model=model,
            vram_used=vram_used,
            vram_total=vram_total,
            tokens=tokens,
            thinking_mode=self._thinking_mode,
            context_used=context_used,
            context_max=context_max,
        )

    async def _save_conversation(self, name: str | None) -> None:
        """Save current conversation."""
        if not self._storage or not self._conversation_id:
            if self._ui:
                self._ui.print_error("No conversation to save")
            return

        if name:
            await self._storage.update_conversation_title(self._conversation_id, name)

        if self._ui:
            self._ui.print_info(f"Conversation saved: {name or self._conversation_id}")

    async def _load_conversation(self, name: str) -> None:
        """Load a conversation by name/ID."""
        if not self._storage or not self._ui:
            return

        # Search for conversation
        conversations = await self._storage.list_conversations()
        match = None
        for conv in conversations:
            if conv["title"] == name or conv["id"] == name:
                match = conv
                break

        if not match:
            self._ui.print_error(f"Conversation not found: {name}")
            return

        # Load messages
        messages, _ = await self._storage.load_conversation(match["id"])
        self._messages = messages
        self._conversation_id = match["id"]
        self._ui.print_info(f"Loaded conversation: {match['title']}")

    async def _list_sessions(self) -> None:
        """List all sessions (conversations) for this project."""
        if not self._storage or not self._ui:
            return

        conversations = await self._storage.list_conversations()

        if not conversations:
            self._ui.print_info("No sessions found. Use /new to create one.")
            return

        lines = ["**Sessions:**\n"]
        for conv in conversations:
            marker = " (current)" if conv["id"] == self._conversation_id else ""
            msg_count = conv.get("message_count", 0)
            lines.append(f"  `{conv['id'][:8]}` - {conv['title']} ({msg_count} messages){marker}")

        self._ui.print_info("\n".join(lines))

    async def _new_session(self, name: str) -> None:
        """Create a new session and switch to it."""
        if not self._storage or not self._ui:
            return

        # Clear current messages
        self._messages = []

        # Create new conversation
        self._conversation_id = await self._storage.create_conversation(
            title=name,
            project_path=str(self.project_dir),
        )

        self._ui.print_info(f"Created new session: {name}")

    async def _switch_session(self, session_id: str | None) -> None:
        """Switch to an existing session."""
        if not self._storage or not self._ui or not session_id:
            if self._ui:
                self._ui.print_error("Session ID required")
            return

        # Find the conversation (support partial IDs)
        conversations = await self._storage.list_conversations()
        match = None
        for conv in conversations:
            if conv["id"].startswith(session_id):
                match = conv
                break

        if not match:
            self._ui.print_error(f"Session not found: {session_id}")
            return

        # Load messages
        messages, _ = await self._storage.load_conversation(match["id"])
        self._messages = messages
        self._conversation_id = match["id"]
        self._ui.print_info(f"Switched to: {match['title']}")

    async def _rename_session(self, name: str | None) -> None:
        """Rename the current session."""
        if not self._storage or not self._ui:
            return

        if not name:
            self._ui.print_error("Name required")
            return

        if not self._conversation_id:
            self._ui.print_error("No active session")
            return

        await self._storage.update_conversation_title(self._conversation_id, name)
        self._ui.print_info(f"Renamed to: {name}")

    async def _delete_session(self, session_id: str | None) -> None:
        """Delete a session."""
        if not self._storage or not self._ui or not session_id:
            if self._ui:
                self._ui.print_error("Session ID required")
            return

        # Don't allow deleting current session
        if self._conversation_id and self._conversation_id.startswith(session_id):
            self._ui.print_error("Cannot delete current session. Switch to another first.")
            return

        # Find the conversation
        conversations = await self._storage.list_conversations()
        match = None
        for conv in conversations:
            if conv["id"].startswith(session_id):
                match = conv
                break

        if not match:
            self._ui.print_error(f"Session not found: {session_id}")
            return

        await self._storage.delete_conversation(match["id"])
        self._ui.print_info(f"Deleted session: {match['title']}")

    async def _export_session(self, session_id: str | None) -> None:
        """Export a session to markdown."""
        if not self._storage or not self._ui:
            return

        # Use current session if no ID provided
        conv_id = session_id or self._conversation_id
        if not conv_id:
            self._ui.print_error("No session to export")
            return

        # Find conversation
        conversations = await self._storage.list_conversations()
        match = None
        for conv in conversations:
            if conv["id"].startswith(conv_id):
                match = conv
                break

        if not match:
            self._ui.print_error(f"Session not found: {conv_id}")
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

    async def single_turn(self, message: str, stream: bool = True) -> None:
        """
        Process a single message and exit.

        Uses the full AgentEngine with tool support for accurate responses.

        Args:
            message: User message
            stream: Whether to stream output (currently ignored, always streams)
        """
        try:
            await self.initialize()

            # Check if models are available
            if not self._orchestrator or not self._orchestrator.get_available_models():
                self.console.print(f"[dim]You: {message}[/dim]")
                self.console.print("\n[yellow]No models configured.[/yellow]")
                self.console.print(
                    "[dim]Configure models in ~/.entropi/config.yaml or "
                    ".entropi/config.yaml[/dim]"
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
                self.console.print(f"[green]Done[/green] {tool_call.name} [dim]({duration_ms:.0f}ms, {summary})[/dim]\n")

            self._engine.set_callbacks(
                on_stream_chunk=on_chunk,
                on_tool_start=on_tool_start,
                on_tool_complete=on_tool_complete,
            )

            # Run agent loop (tool results are shown via on_tool_complete callback)
            async for _ in self._engine.run(message):
                pass

            self.console.print()  # Newline after response

        finally:
            await self.shutdown()
