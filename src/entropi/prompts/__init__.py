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


def get_constitution_prompt(prompts_dir: Path | None = None) -> str:
    """
    Get the constitution prompt (shared principles across all tiers).

    Args:
        prompts_dir: Optional user prompts directory

    Returns:
        Constitution prompt content
    """
    return load_prompt("constitution", prompts_dir)


def get_tier_identity_prompt(tier: str, prompts_dir: Path | None = None) -> str:
    """
    Get the identity prompt for a specific model tier.

    Args:
        tier: Model tier (thinking, normal, code, simple)
        prompts_dir: Optional user prompts directory

    Returns:
        Tier-specific identity prompt
    """
    return load_prompt(f"identity_{tier}", prompts_dir)


def get_identity_prompt(tier: str, prompts_dir: Path | None = None) -> str:
    """
    Get the full identity prompt: constitution + tier-specific identity.

    Args:
        tier: Model tier (thinking, normal, code, simple)
        prompts_dir: Optional user prompts directory

    Returns:
        Combined identity prompt
    """
    constitution = get_constitution_prompt(prompts_dir)
    tier_identity = get_tier_identity_prompt(tier, prompts_dir)
    return f"{constitution}\n\n{tier_identity}"


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


def _tool_name_to_guidance_filename(tool_name: str) -> str:
    """Convert a dotted tool name to its guidance filename.

    Strips the server prefix and replaces dots with underscores.
    Examples: filesystem.read_file → read_file, bash.execute → bash_execute,
    system.handoff → system_handoff, entropi.todo_write → todo_write.
    """
    parts = tool_name.split(".", 1)
    base = parts[1] if len(parts) > 1 else parts[0]
    return base.replace(".", "_")


def get_per_tool_guidance(tool_names: list[str], prompts_dir: Path | None = None) -> str:
    """Load per-tool guidance for the given tool names only.

    For each tool name, looks up a corresponding guidance file in
    prompts/tools/. Only guidance for tools in the provided list is
    returned — tools not in the list are invisible to the model.

    Args:
        tool_names: Filtered list of allowed tool names
        prompts_dir: Optional user prompts directory

    Returns:
        Concatenated per-tool guidance sections (may be empty)
    """
    sections: list[str] = []
    for name in tool_names:
        filename = _tool_name_to_guidance_filename(name)
        try:
            content = load_prompt(f"tools/{filename}", prompts_dir)
            sections.append(f"### {name}\n{content.strip()}")
        except FileNotFoundError:
            logger.debug(f"No guidance file for tool '{name}' ({filename}.md)")
    if not sections:
        return ""
    return "## Tool Guidance\n\n" + "\n\n".join(sections)


def _extract_focus_points(identity_content: str, tier_name: str = "") -> str:
    """Extract Focus bullet points from an identity file.

    Parses the markdown to find the ``## Focus`` section and returns
    its bullet points joined as a comma-separated string.

    Raises ValueError if ## Focus section is missing — this section
    is required for router classification to work correctly.
    """
    lines = identity_content.strip().split("\n")
    in_focus = False
    points: list[str] = []
    for line in lines:
        if line.strip() == "## Focus":
            in_focus = True
            continue
        if in_focus:
            if line.startswith("##"):
                break
            stripped = line.strip().lstrip("- ").strip()
            if stripped:
                points.append(stripped.lower())
    if not points:
        label = f"identity_{tier_name}.md" if tier_name else "identity file"
        raise ValueError(
            f"Missing '## Focus' section in {label}. "
            f"This section is required for router classification."
        )
    return ", ".join(points)


def get_classification_prompt(
    message: str,
    prompts_dir: Path | None = None,
    history: list[str] | None = None,
) -> str:
    """
    Get the classification prompt with identity descriptions and history.

    Loads each tier's identity file to give the router model a description
    of what each tier handles, then includes recent conversation history
    for context-aware classification.

    Args:
        message: User message to classify
        prompts_dir: Optional user prompts directory
        history: Recent user messages (up to 5) for context

    Returns:
        Classification prompt with identities, history, and message
    """
    # Build tier descriptions from identity file Focus sections
    tier_map = {1: "simple", 2: "code", 3: "normal", 4: "thinking"}
    identity_lines = []
    for num, tier_name in tier_map.items():
        try:
            identity = get_tier_identity_prompt(tier_name, prompts_dir)
            focus = _extract_focus_points(identity, tier_name)
            identity_lines.append(f"{num} = {tier_name.upper()}: {focus}")
        except FileNotFoundError:
            identity_lines.append(f"{num} = {tier_name.upper()}")
    identities_text = "\n".join(identity_lines)

    # Build history context (compact format for small router model)
    if history:
        history_text = "Recent messages: " + " | ".join(history[-5:])
    else:
        history_text = ""

    try:
        template = load_prompt("classification", prompts_dir)
        prompt = template.format(
            identities=identities_text,
            history=history_text,
            message=message,
        )
        # Ensure prompt ends with "-> " (no trailing newline) so the
        # model immediately outputs the classification digit
        return prompt.rstrip() + " "
    except FileNotFoundError:
        logger.warning("classification.md not found, using minimal default")
        return f"""Classify into tier. Output ONLY the number 1, 2, 3, or 4.

{identities_text}

Message: "{message}"
Classification:"""
