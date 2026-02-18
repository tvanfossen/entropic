"""Tests for BaseTool, ToolRegistry, and BaseMCPServer tool registration."""

import json
from typing import Any

import pytest
from entropi.core.tool_validation import ToolValidationError
from entropi.mcp.servers.base import BaseMCPServer, ServerResponse
from entropi.mcp.tools import BaseTool, ToolRegistry
from mcp.types import Tool

# -- Fixtures ----------------------------------------------------------------


@pytest.fixture()
def tools_dir(tmp_path):
    """Create a temp tools directory with a valid JSON tool definition."""
    server_dir = tmp_path / "test_server"
    server_dir.mkdir()
    tool_def = {
        "name": "greet",
        "description": "Says hello.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "name": {"type": "string", "description": "Who to greet"},
            },
            "required": ["name"],
        },
    }
    (server_dir / "greet.json").write_text(json.dumps(tool_def))
    return tmp_path


@pytest.fixture()
def second_tool_json(tools_dir):
    """Add a second tool definition to the fixture directory."""
    server_dir = tools_dir / "test_server"
    tool_def = {
        "name": "farewell",
        "description": "Says goodbye.",
        "inputSchema": {
            "type": "object",
            "properties": {},
        },
    }
    (server_dir / "farewell.json").write_text(json.dumps(tool_def))
    return tools_dir


# -- Concrete tool for testing -----------------------------------------------


class GreetTool(BaseTool):
    """Test tool that returns a greeting."""

    def __init__(self, tools_dir):
        super().__init__("greet", "test_server", tools_dir)

    async def execute(self, arguments: dict[str, Any]) -> str:
        return f"Hello, {arguments['name']}!"


class FarewellTool(BaseTool):
    """Test tool that returns a farewell."""

    def __init__(self, tools_dir):
        super().__init__("farewell", "test_server", tools_dir)

    async def execute(self, arguments: dict[str, Any]) -> str:
        return "Goodbye!"


class DirectiveTool(BaseTool):
    """Test tool that returns a ServerResponse with directives."""

    def __init__(self, tools_dir):
        super().__init__("greet", "test_server", tools_dir)

    async def execute(self, arguments: dict[str, Any]) -> ServerResponse:
        return ServerResponse(result="done", directives=[])


class InlineDictTool(BaseTool):
    """Test tool using an inline dict definition (no JSON file)."""

    def __init__(self):
        super().__init__(
            definition={
                "name": "ping",
                "description": "Returns pong.",
                "inputSchema": {"type": "object", "properties": {}, "required": []},
            }
        )

    async def execute(self, arguments: dict[str, Any]) -> str:
        return "pong"


class InlineToolObjectTool(BaseTool):
    """Test tool using a pre-built Tool object."""

    def __init__(self):
        super().__init__(
            definition=Tool(
                name="echo",
                description="Echoes input.",
                inputSchema={
                    "type": "object",
                    "properties": {"text": {"type": "string", "description": "Text to echo"}},
                    "required": ["text"],
                },
            )
        )

    async def execute(self, arguments: dict[str, Any]) -> str:
        return arguments.get("text", "")


# -- BaseTool tests ----------------------------------------------------------


class TestBaseTool:
    """Tests for BaseTool ABC."""

    def test_loads_definition(self, tools_dir):
        """Construction loads and validates the JSON definition."""
        tool = GreetTool(tools_dir)
        assert isinstance(tool.definition, Tool)

    def test_name_property(self, tools_dir):
        """name property returns the tool name from JSON."""
        tool = GreetTool(tools_dir)
        assert tool.name == "greet"

    def test_definition_has_schema(self, tools_dir):
        """definition includes the inputSchema from JSON."""
        tool = GreetTool(tools_dir)
        assert tool.definition.inputSchema["type"] == "object"
        assert "name" in tool.definition.inputSchema["properties"]

    def test_missing_json_raises(self, tmp_path):
        """FileNotFoundError when JSON file doesn't exist."""
        with pytest.raises(FileNotFoundError):
            GreetTool(tmp_path)

    def test_invalid_json_raises(self, tmp_path):
        """ToolValidationError when JSON is malformed."""
        server_dir = tmp_path / "test_server"
        server_dir.mkdir()
        bad_def = {"name": "", "inputSchema": {"type": "object", "properties": {}}}
        (server_dir / "greet.json").write_text(json.dumps(bad_def))
        with pytest.raises(ToolValidationError):
            GreetTool(tmp_path)

    @pytest.mark.anyio()
    async def test_execute(self, tools_dir):
        """execute() returns expected result."""
        tool = GreetTool(tools_dir)
        result = await tool.execute({"name": "World"})
        assert result == "Hello, World!"

    @pytest.mark.anyio()
    async def test_execute_server_response(self, tools_dir):
        """execute() can return ServerResponse."""
        tool = DirectiveTool(tools_dir)
        result = await tool.execute({"name": "test"})
        assert isinstance(result, ServerResponse)
        assert result.result == "done"


class TestBaseToolInlineDefinition:
    """Tests for BaseTool with inline definitions (no JSON file)."""

    def test_inline_dict_creates_tool(self):
        """Inline dict definition creates a valid tool."""
        tool = InlineDictTool()
        assert tool.name == "ping"
        assert isinstance(tool.definition, Tool)

    def test_inline_tool_object(self):
        """Pre-built Tool object is used directly."""
        tool = InlineToolObjectTool()
        assert tool.name == "echo"
        assert "text" in tool.definition.inputSchema["properties"]

    @pytest.mark.anyio()
    async def test_inline_dict_executes(self):
        """Inline-defined tool executes normally."""
        tool = InlineDictTool()
        result = await tool.execute({})
        assert result == "pong"

    @pytest.mark.anyio()
    async def test_inline_tool_object_executes(self):
        """Tool-object-defined tool executes normally."""
        tool = InlineToolObjectTool()
        result = await tool.execute({"text": "hello"})
        assert result == "hello"

    def test_inline_invalid_dict_raises(self):
        """Invalid inline dict raises ToolValidationError."""

        class BadTool(BaseTool):
            def __init__(self):
                super().__init__(definition={"name": "", "inputSchema": "not-a-dict"})

            async def execute(self, arguments: dict[str, Any]) -> str:
                return ""

        with pytest.raises(ToolValidationError):
            BadTool()

    def test_no_definition_and_no_name_raises(self):
        """Neither definition nor tool_name raises ValueError."""

        class EmptyTool(BaseTool):
            def __init__(self):
                super().__init__()

            async def execute(self, arguments: dict[str, Any]) -> str:
                return ""

        with pytest.raises(ValueError, match="requires either tool_name or definition"):
            EmptyTool()

    def test_definition_takes_precedence_over_tool_name(self, tools_dir):
        """When both definition and tool_name given, definition wins."""

        class DualTool(BaseTool):
            def __init__(self, td):
                super().__init__(
                    tool_name="greet",
                    server_prefix="test_server",
                    tools_dir=td,
                    definition={
                        "name": "overridden",
                        "description": "Inline wins.",
                        "inputSchema": {"type": "object", "properties": {}, "required": []},
                    },
                )

            async def execute(self, arguments: dict[str, Any]) -> str:
                return ""

        tool = DualTool(tools_dir)
        assert tool.name == "overridden"


# -- ToolRegistry tests ------------------------------------------------------


class TestToolRegistry:
    """Tests for ToolRegistry."""

    def test_register_and_get_tools(self, tools_dir):
        """Registered tools appear in get_tools()."""
        registry = ToolRegistry()
        registry.register(GreetTool(tools_dir))
        tools = registry.get_tools()
        assert len(tools) == 1
        assert tools[0].name == "greet"

    def test_multiple_tools(self, second_tool_json):
        """Multiple tools registered and returned."""
        registry = ToolRegistry()
        registry.register(GreetTool(second_tool_json))
        registry.register(FarewellTool(second_tool_json))
        tools = registry.get_tools()
        assert len(tools) == 2
        names = {t.name for t in tools}
        assert names == {"greet", "farewell"}

    def test_empty_registry(self):
        """Empty registry returns empty list."""
        registry = ToolRegistry()
        assert registry.get_tools() == []

    @pytest.mark.anyio()
    async def test_dispatch(self, tools_dir):
        """dispatch() routes to the correct tool."""
        registry = ToolRegistry()
        registry.register(GreetTool(tools_dir))
        result = await registry.dispatch("greet", {"name": "Alice"})
        assert result == "Hello, Alice!"

    @pytest.mark.anyio()
    async def test_dispatch_unknown(self, tools_dir):
        """dispatch() returns error for unknown tool name."""
        registry = ToolRegistry()
        registry.register(GreetTool(tools_dir))
        result = await registry.dispatch("nonexistent", {})
        assert "Unknown tool" in result

    def test_duplicate_warns(self, tools_dir, caplog):
        """Registering a duplicate tool name logs a warning."""
        registry = ToolRegistry()
        registry.register(GreetTool(tools_dir))
        registry.register(GreetTool(tools_dir))
        assert "already registered" in caplog.text


# -- BaseMCPServer integration -----------------------------------------------


class RegistryServer(BaseMCPServer):
    """Server using register_tool() — no get_tools/execute_tool override."""

    def __init__(self, tools_dir):
        super().__init__("test")
        self.register_tool(GreetTool(tools_dir))


class OverrideServer(BaseMCPServer):
    """Server with manual overrides — legacy pattern."""

    def get_tools(self) -> list[Tool]:
        return [
            Tool(
                name="custom",
                description="Custom tool",
                inputSchema={"type": "object", "properties": {}},
            )
        ]

    async def execute_tool(self, name: str, arguments: dict[str, Any]) -> str:
        if name == "custom":
            return "custom result"
        return f"Unknown tool: {name}"


class TestBaseMCPServerRegistration:
    """Tests for BaseMCPServer tool registration integration."""

    def test_registry_get_tools(self, tools_dir):
        """Server using register_tool() returns tools via get_tools()."""
        server = RegistryServer(tools_dir)
        tools = server.get_tools()
        assert len(tools) == 1
        assert tools[0].name == "greet"

    @pytest.mark.anyio()
    async def test_registry_execute_tool(self, tools_dir):
        """Server using register_tool() dispatches via execute_tool()."""
        server = RegistryServer(tools_dir)
        result = await server.execute_tool("greet", {"name": "Bob"})
        assert result == "Hello, Bob!"

    @pytest.mark.anyio()
    async def test_registry_unknown_tool(self, tools_dir):
        """Server returns error for unregistered tool name."""
        server = RegistryServer(tools_dir)
        result = await server.execute_tool("missing", {})
        assert "Unknown tool" in result

    def test_override_get_tools(self):
        """Server with overridden get_tools() bypasses registry."""
        server = OverrideServer("override")
        tools = server.get_tools()
        assert len(tools) == 1
        assert tools[0].name == "custom"

    @pytest.mark.anyio()
    async def test_override_execute_tool(self):
        """Server with overridden execute_tool() bypasses registry."""
        server = OverrideServer("override")
        result = await server.execute_tool("custom", {})
        assert result == "custom result"
