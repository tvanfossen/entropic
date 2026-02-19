"""Tests for MCP tool definition validation."""

from entropic.core.tool_validation import ToolValidationError, validate_tool_definition


class TestValidateToolDefinition:
    """Tests for validate_tool_definition."""

    def test_valid_tool_passes(self) -> None:
        """Well-formed MCP tool definition accepted."""
        tool = {
            "name": "read_file",
            "description": "Read a file",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "path": {"type": "string", "description": "File path"},
                },
                "required": ["path"],
            },
        }
        assert validate_tool_definition(tool) == []

    def test_missing_name_rejected(self) -> None:
        tool = {
            "description": "A tool",
            "inputSchema": {"type": "object", "properties": {}},
        }
        errors = validate_tool_definition(tool)
        assert any("name" in e for e in errors)

    def test_empty_name_rejected(self) -> None:
        tool = {
            "name": "",
            "description": "A tool",
            "inputSchema": {"type": "object", "properties": {}},
        }
        errors = validate_tool_definition(tool)
        assert any("name" in e for e in errors)

    def test_missing_description_rejected(self) -> None:
        tool = {
            "name": "test",
            "inputSchema": {"type": "object", "properties": {}},
        }
        errors = validate_tool_definition(tool)
        assert any("description" in e for e in errors)

    def test_missing_input_schema_rejected(self) -> None:
        tool = {"name": "test", "description": "A tool"}
        errors = validate_tool_definition(tool)
        assert any("inputSchema" in e for e in errors)

    def test_wrong_schema_type_rejected(self) -> None:
        tool = {
            "name": "test",
            "description": "A tool",
            "inputSchema": {"type": "array", "properties": {}},
        }
        errors = validate_tool_definition(tool)
        assert any("type" in e and "object" in e for e in errors)

    def test_required_field_not_in_properties_rejected(self) -> None:
        tool = {
            "name": "test",
            "description": "A tool",
            "inputSchema": {
                "type": "object",
                "properties": {"a": {"type": "string"}},
                "required": ["a", "b"],
            },
        }
        errors = validate_tool_definition(tool)
        assert any("'b'" in e for e in errors)

    def test_nested_array_items_validated(self) -> None:
        """todo_write-style nested items schema validated."""
        tool = {
            "name": "todo_write",
            "description": "Manage todos",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "todos": {
                        "type": "array",
                        "items": {
                            "type": "object",
                            "properties": {
                                "content": {"type": "string"},
                            },
                            "required": ["content", "missing_field"],
                        },
                    },
                },
                "required": ["todos"],
            },
        }
        errors = validate_tool_definition(tool)
        assert any("missing_field" in e for e in errors)

    def test_nested_object_validated(self) -> None:
        tool = {
            "name": "test",
            "description": "A tool",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "config": {
                        "type": "object",
                        "properties": {
                            "key": {"type": "string"},
                        },
                        "required": ["key", "nonexistent"],
                    },
                },
            },
        }
        errors = validate_tool_definition(tool)
        assert any("nonexistent" in e for e in errors)

    def test_property_missing_type_rejected(self) -> None:
        tool = {
            "name": "test",
            "description": "A tool",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "bad_prop": {"description": "no type"},
                },
            },
        }
        errors = validate_tool_definition(tool)
        assert any("type" in e and "bad_prop" in e for e in errors)


class TestToolValidationError:
    """Tests for ToolValidationError exception."""

    def test_includes_tool_name(self) -> None:
        err = ToolValidationError("my_tool", ["error one", "error two"])
        assert "my_tool" in str(err)
        assert "error one" in str(err)
        assert "error two" in str(err)
        assert err.tool_name == "my_tool"
        assert err.errors == ["error one", "error two"]
