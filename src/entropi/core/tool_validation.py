"""
MCP tool definition validation.

Validates tool definitions against the MCP tool format before injection
into the system prompt or passing to the inference engine. Fails loudly
on invalid definitions — no silent fallbacks.
"""

from typing import Any


class ToolValidationError(Exception):
    """Raised when a tool definition fails MCP format validation."""

    def __init__(self, tool_name: str, errors: list[str]) -> None:
        self.tool_name = tool_name
        self.errors = errors
        msg = f"Invalid tool definition '{tool_name}':\n" + "\n".join(f"  - {e}" for e in errors)
        super().__init__(msg)


def validate_tool_definition(tool: dict[str, Any]) -> list[str]:
    """Validate a tool definition against MCP tool format.

    Args:
        tool: Tool definition dict with name, description, inputSchema.

    Returns:
        List of error strings. Empty list means valid.
    """
    errors: list[str] = []

    name = tool.get("name")
    if not name or not isinstance(name, str):
        errors.append("'name' must be a non-empty string")

    description = tool.get("description")
    if not description or not isinstance(description, str):
        errors.append("'description' must be a non-empty string")

    schema = tool.get("inputSchema")
    if not isinstance(schema, dict):
        errors.append("'inputSchema' must be an object")
        return errors

    if schema.get("type") != "object":
        errors.append("inputSchema.type must be 'object'")

    properties = schema.get("properties")
    if not isinstance(properties, dict):
        errors.append("inputSchema.properties must be an object")
        return errors

    _validate_required_fields(errors, schema.get("required", []), properties)
    _validate_properties(errors, properties, prefix="")

    return errors


def _validate_required_fields(
    errors: list[str],
    required: Any,
    properties: dict[str, Any],
    path_prefix: str = "",
) -> None:
    """Validate that all required fields exist in properties."""
    if not isinstance(required, list):
        label = f"property '{path_prefix}'" if path_prefix else "inputSchema"
        errors.append(f"{label}.required must be an array")
        return
    for field in required:
        if field not in properties:
            label = f"property '{path_prefix}': " if path_prefix else ""
            errors.append(f"{label}required field '{field}' not in properties")


def _validate_properties(
    errors: list[str],
    properties: dict[str, Any],
    prefix: str,
) -> None:
    """Recursively validate schema properties."""
    for name, details in properties.items():
        path = f"{prefix}{name}" if not prefix else f"{prefix}.{name}"

        if not isinstance(details, dict):
            errors.append(f"property '{path}' must be an object")
            continue

        prop_type = details.get("type")
        if not prop_type:
            errors.append(f"property '{path}' missing 'type'")

        if prop_type == "array":
            _validate_array_items(errors, details, path)

        if prop_type == "object" and "properties" in details:
            _validate_nested_object(errors, details, path)


def _validate_array_items(
    errors: list[str],
    details: dict[str, Any],
    path: str,
) -> None:
    """Validate array items schema."""
    if "items" not in details:
        return

    items = details["items"]
    if not isinstance(items, dict):
        errors.append(f"property '{path}'.items must be an object")
        return

    if items.get("type") != "object" or "properties" not in items:
        return

    item_props = items["properties"]
    _validate_required_fields(errors, items.get("required", []), item_props, f"'{path}'.items")
    _validate_properties(errors, item_props, prefix=f"{path}[].")


def _validate_nested_object(
    errors: list[str],
    details: dict[str, Any],
    path: str,
) -> None:
    """Validate nested object properties."""
    nested_props = details["properties"]
    _validate_required_fields(errors, details.get("required", []), nested_props, path)
    _validate_properties(errors, nested_props, prefix=f"{path}.")
