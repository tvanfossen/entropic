# Constitutional AI Implementation for Entropi

## Overview

Constitutional AI (CAI) is an approach where AI systems follow a set of principles (a "constitution") that guides behavior. For Entropi, this would add a governance layer ensuring the assistant operates safely and ethically within user-defined boundaries.

## Implementation Approaches

### 1. Prompt-Based Constitution (Lightweight)

**How it works:** Include constitutional principles directly in the system prompt. The model self-regulates based on these principles.

**Pros:**
- Simple to implement
- No additional latency
- Easy to customize per-project

**Cons:**
- Relies entirely on model compliance
- Can be bypassed with clever prompting
- No enforcement mechanism

**Implementation location:** `src/entropi/data/prompts/constitution.md`

```markdown
# Constitutional Principles

## Safety
- Never execute destructive commands (rm -rf, DROP TABLE, etc.) without explicit confirmation
- Warn about operations that could cause data loss
- Refuse to write code designed to harm systems or users

## Privacy
- Do not read files matching patterns in .gitignore unless explicitly requested
- Never transmit code or data to external services
- Treat files containing credentials (*.env, *credentials*, *secret*) as sensitive

## Code Quality
- Follow existing project patterns and conventions
- Do not introduce known security vulnerabilities (SQL injection, XSS, etc.)
- Preserve existing functionality when making changes
```

---

### 2. Tool-Level Policy Enforcement (Current Foundation)

**How it works:** Policies are enforced at the tool execution layer. Already partially implemented via the permission system.

**Current implementation:**
- `src/entropi/mcp/manager.py` - Permission allow/deny lists
- `src/entropi/core/engine.py` - Tool approval flow

**Extension points:**

```python
# In mcp/manager.py or new policies.py

class ToolPolicy:
    """Policy that evaluates tool calls against constitutional rules."""

    def evaluate(self, tool_call: ToolCall) -> PolicyResult:
        """
        Evaluate a tool call against policies.

        Returns:
            PolicyResult with action (ALLOW, DENY, WARN) and reason
        """
        pass

class DestructiveCommandPolicy(ToolPolicy):
    """Detect and flag potentially destructive bash commands."""

    DESTRUCTIVE_PATTERNS = [
        r"rm\s+(-rf?|--recursive)",
        r"DROP\s+(TABLE|DATABASE)",
        r"DELETE\s+FROM.*WHERE\s+1\s*=\s*1",
        r">\s*/dev/sd[a-z]",
        r"mkfs\.",
        r"dd\s+if=",
    ]

    def evaluate(self, tool_call: ToolCall) -> PolicyResult:
        if tool_call.name != "bash.execute":
            return PolicyResult.ALLOW

        command = tool_call.arguments.get("command", "")
        for pattern in self.DESTRUCTIVE_PATTERNS:
            if re.search(pattern, command, re.IGNORECASE):
                return PolicyResult(
                    action=PolicyAction.WARN,
                    reason=f"Potentially destructive command detected: {pattern}",
                    requires_confirmation=True,
                )
        return PolicyResult.ALLOW
```

**Pros:**
- Reliable enforcement (can't be bypassed by prompting)
- Low latency (pattern matching)
- Integrates with existing permission system

**Cons:**
- Limited to tool calls (doesn't evaluate text responses)
- Rule-based, may miss novel threats
- Requires manual rule maintenance

---

### 3. Output Critique Layer (Medium Overhead)

**How it works:** After the main model generates a response, a lightweight critique pass evaluates it against constitutional principles.

**Architecture:**

```
User Input → Main Model → Response → Critique Model → Final Output
                                          ↓
                                    (if violation)
                                          ↓
                                    Revision Request
```

**Implementation:**

```python
# In core/engine.py or new constitutional.py

class ConstitutionalCritique:
    """Evaluates model outputs against constitutional principles."""

    def __init__(self, orchestrator: ModelOrchestrator, principles: list[str]):
        self.orchestrator = orchestrator
        self.principles = principles

    async def critique(self, response: str, tool_calls: list[ToolCall]) -> CritiqueResult:
        """
        Evaluate response against constitutional principles.

        Uses the ROUTER model for fast, lightweight evaluation.
        """
        critique_prompt = self._build_critique_prompt(response, tool_calls)

        # Use small model for critique (fast, low overhead)
        result = await self.orchestrator.generate(
            [Message(role="user", content=critique_prompt)],
            tier=ModelTier.ROUTER,
            grammar=CRITIQUE_GRAMMAR,  # Constrained output: "OK" | "VIOLATION:reason"
        )

        return self._parse_critique(result.content)

    def _build_critique_prompt(self, response: str, tool_calls: list[ToolCall]) -> str:
        principles_text = "\n".join(f"- {p}" for p in self.principles)
        return f"""Evaluate this response against the principles. Output ONLY "OK" or "VIOLATION:reason".

Principles:
{principles_text}

Response to evaluate:
{response}

Tool calls: {[tc.name for tc in tool_calls]}

Evaluation:"""
```

**Integration point in engine.py:**

```python
async def _generate_response(self, ctx: LoopContext) -> tuple[str, list[ToolCall]]:
    # ... existing generation code ...

    # Constitutional critique (if enabled)
    if self.config.constitutional.enabled:
        critique = await self.constitutional.critique(content, tool_calls)
        if critique.has_violation:
            # Option 1: Auto-revise
            content, tool_calls = await self._revise_response(ctx, content, critique)
            # Option 2: Warn user
            self._on_constitutional_warning(critique)
            # Option 3: Block response
            raise ConstitutionalViolation(critique.reason)

    return content, tool_calls
```

**Pros:**
- Evaluates full responses, not just tool calls
- Can catch subtle issues
- Uses existing model infrastructure

**Cons:**
- Adds latency (extra model call per response)
- Small models may miss nuanced violations
- Requires careful prompt engineering

---

### 4. Full CAI Loop (High Overhead, Maximum Safety)

**How it works:** Generate → Critique → Revise loop, similar to Anthropic's original CAI approach.

**Architecture:**

```
User Input → Generate Response → Self-Critique → Revise → Output
                    ↑__________________|
                    (iterate until no violations)
```

**Implementation:**

```python
class ConstitutionalLoop:
    """Full Constitutional AI loop with self-critique and revision."""

    MAX_REVISIONS = 3

    async def generate_constitutional(
        self,
        ctx: LoopContext,
        principles: list[str],
    ) -> tuple[str, list[ToolCall]]:
        """Generate response with constitutional oversight."""

        for revision in range(self.MAX_REVISIONS):
            # Generate
            content, tool_calls = await self._generate_response(ctx)

            # Critique
            critique = await self._self_critique(content, tool_calls, principles)

            if not critique.has_violation:
                return content, tool_calls

            # Revise
            ctx.messages.append(Message(
                role="system",
                content=f"Your response violated principle: {critique.reason}. "
                        f"Please revise your response to comply."
            ))

        # Max revisions reached - return with warning
        logger.warning(f"Constitutional revision limit reached")
        return content, tool_calls
```

**Pros:**
- Most thorough approach
- Self-correcting behavior
- Aligned with academic CAI research

**Cons:**
- Significant latency (2-3x model calls)
- Higher compute cost
- May over-correct or get stuck in loops

---

## Recommended Implementation Strategy

### Phase 1: Tool-Level Policies (Immediate)

Extend the existing permission system with policy rules:

```yaml
# .entropi/config.yaml
policies:
  destructive_commands:
    enabled: true
    action: warn  # warn, block, or confirm
    patterns:
      - "rm -rf"
      - "DROP TABLE"

  sensitive_files:
    enabled: true
    action: confirm
    patterns:
      - "*.env"
      - "*credentials*"
      - "*secret*"
      - ".git/config"
```

**Files to modify:**
- `src/entropi/config/schema.py` - Add PoliciesConfig
- `src/entropi/mcp/manager.py` - Add policy evaluation
- `src/entropi/core/engine.py` - Hook policies into tool execution

### Phase 2: Prompt-Based Constitution (Short-term)

Add constitutional principles to the system prompt:

```
src/entropi/data/prompts/
├── identity.md
├── tool_usage.md
└── constitution.md  # NEW
```

Load and include in context building.

### Phase 3: Lightweight Critique (Medium-term)

Add optional critique pass using ROUTER model:

```yaml
# .entropi/config.yaml
constitutional:
  enabled: true
  critique_model: router  # Use small model for speed
  action_on_violation: warn  # warn, revise, or block
  principles:
    - "Do not execute destructive commands without confirmation"
    - "Do not read files containing credentials unless necessary"
    - "Follow existing project patterns"
```

### Phase 4: Configurable Safety Levels (Long-term)

```yaml
# .entropi/config.yaml
safety_level: standard  # minimal, standard, strict, paranoid

# Presets:
# minimal: Tool policies only, no critique
# standard: Tool policies + prompt constitution
# strict: Tool policies + prompt + lightweight critique
# paranoid: Full CAI loop with revisions
```

---

## Schema Design

```python
# src/entropi/config/schema.py

class PolicyRule(BaseModel):
    """A single policy rule."""
    name: str
    enabled: bool = True
    action: Literal["allow", "warn", "confirm", "block"] = "warn"
    patterns: list[str] = Field(default_factory=list)
    description: str = ""

class PoliciesConfig(BaseModel):
    """Tool-level policy configuration."""
    destructive_commands: PolicyRule = Field(
        default_factory=lambda: PolicyRule(
            name="destructive_commands",
            action="confirm",
            patterns=["rm -rf", "DROP TABLE", "DELETE FROM"]
        )
    )
    sensitive_files: PolicyRule = Field(
        default_factory=lambda: PolicyRule(
            name="sensitive_files",
            action="confirm",
            patterns=["*.env", "*secret*", "*credential*"]
        )
    )
    custom: list[PolicyRule] = Field(default_factory=list)

class ConstitutionalConfig(BaseModel):
    """Constitutional AI configuration."""
    enabled: bool = False
    critique_enabled: bool = False
    critique_model: Literal["router", "normal"] = "router"
    action_on_violation: Literal["warn", "revise", "block"] = "warn"
    max_revisions: int = 2
    principles: list[str] = Field(default_factory=lambda: [
        "Do not execute commands that could cause data loss without confirmation",
        "Do not read or expose credentials, secrets, or API keys",
        "Follow the project's existing coding patterns and conventions",
        "Do not introduce known security vulnerabilities",
    ])

class EntropyConfig(BaseModel):
    # ... existing fields ...
    policies: PoliciesConfig = Field(default_factory=PoliciesConfig)
    constitutional: ConstitutionalConfig = Field(default_factory=ConstitutionalConfig)
```

---

## Key Considerations

### Performance
- Tool-level policies add ~1ms per tool call
- Prompt constitution adds ~0ms (part of existing prompt)
- Critique pass adds ~500-2000ms depending on model
- Full CAI loop adds ~1000-5000ms per response

### User Experience
- Warnings should be informative but not intrusive
- Allow users to override with explicit confirmation
- Learn from user choices (like the permission system)

### Customization
- Per-project constitutions via `.entropi/constitution.md`
- Safety levels for different contexts (production vs development)
- Ability to disable for trusted operations

### Model Limitations
- Small models (ROUTER tier) may miss subtle violations
- Large models are more accurate but slower
- Grammar constraints help with classification tasks

---

## Next Steps

1. **Implement Phase 1** - Tool-level policies (extend permission system)
2. **Add constitution.md** - Prompt-based principles
3. **Evaluate** - Test with real usage patterns
4. **Iterate** - Add critique layer if needed based on observed issues
