"""Prompt templates for Entropi."""

from pathlib import Path

from entropi.core.logging import get_logger

logger = get_logger("prompts")

# Package data directory
_DATA_DIR = Path(__file__).parent.parent / "data" / "prompts"


def load_prompt(name: str, prompts_dir: Path | None = None) -> str:
    """
    Load a prompt template by name.

    Checks in order:
    1. User's prompts_dir (if provided and file exists)
    2. Bundled defaults in package data

    Args:
        name: Prompt name (e.g., "tool_usage" loads "tool_usage.md")
        prompts_dir: Optional user prompts directory

    Returns:
        Prompt content

    Raises:
        FileNotFoundError: If prompt not found in any location
    """
    filename = f"{name}.md"

    # Check user prompts dir first
    if prompts_dir:
        user_path = prompts_dir / filename
        if user_path.exists():
            logger.debug(f"Loading prompt '{name}' from {user_path}")
            return user_path.read_text()

    # Fall back to bundled defaults
    default_path = _DATA_DIR / filename
    if default_path.exists():
        logger.debug(f"Loading prompt '{name}' from bundled defaults")
        return default_path.read_text()

    raise FileNotFoundError(f"Prompt '{name}' not found in {prompts_dir} or {_DATA_DIR}")


def get_identity_prompt(prompts_dir: Path | None = None) -> str:
    """
    Get the identity prompt (who the assistant is and core behaviors).

    Args:
        prompts_dir: Optional user prompts directory

    Returns:
        Identity prompt content
    """
    try:
        return load_prompt("identity", prompts_dir)
    except FileNotFoundError:
        logger.warning("identity.md not found, using minimal default")
        return """You are Entropi, a local AI coding assistant.
You execute tools automatically - never ask the user to run commands you can run yourself."""


def get_tool_usage_prompt(prompts_dir: Path | None = None) -> str:
    """
    Get the tool usage prompt.

    Args:
        prompts_dir: Optional user prompts directory

    Returns:
        Tool usage prompt content
    """
    try:
        return load_prompt("tool_usage", prompts_dir)
    except FileNotFoundError:
        logger.warning("tool_usage.md not found, using minimal default")
        return """You have access to tools. To call a tool, output JSON:
{"name": "tool_name", "arguments": {...}}

IMPORTANT: Use tools to get real data. Do not guess or hallucinate file contents."""


def get_classification_prompt(message: str, prompts_dir: Path | None = None) -> str:
    """
    Get the classification prompt with the user message inserted.

    Args:
        message: User message to classify
        prompts_dir: Optional user prompts directory

    Returns:
        Classification prompt with message inserted
    """
    try:
        template = load_prompt("classification", prompts_dir)
        return template.format(message=message)
    except FileNotFoundError:
        logger.warning("classification.md not found, using minimal default")
        return f"""Classify: SIMPLE, CODE, or REASONING

SIMPLE = greetings, thanks (hello, hi, thanks, ok)
CODE = code tasks (fix, add, edit, debug)
REASONING = questions (what, how, why, explain)

"{message}" ="""
