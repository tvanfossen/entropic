"""Chess model tier definitions.

Three tiers collaborate on each move:
- suggest: Analyzes the position and proposes a candidate move
- validate: Reviews the suggestion for tactical/positional soundness
- execute: Plays the validated move on the board
"""

from entropi import ModelTier

suggest_tier = ModelTier(
    "suggest",
    focus=["analyzing chess positions and suggesting candidate moves"],
)

validate_tier = ModelTier(
    "validate",
    focus=["evaluating chess moves for tactical and positional soundness"],
)

execute_tier = ModelTier(
    "execute",
    focus=["executing validated chess moves on the board"],
)

ALL_TIERS = [suggest_tier, validate_tier, execute_tier]
