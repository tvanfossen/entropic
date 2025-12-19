"""
Type hint analyzer.

Checks for presence and correctness of type hints.
"""

import ast

from entropi.config.schema import QualityRulesConfig
from entropi.quality.analyzers.base import AnalysisResult, CodeAnalyzer


class TypeHintAnalyzer(CodeAnalyzer):
    """Analyzes type hint coverage in Python code."""

    @property
    def name(self) -> str:
        return "type_hints"

    def analyze(self, code: str, rules: QualityRulesConfig) -> AnalysisResult:
        """Analyze code for type hint coverage."""
        result = AnalysisResult()

        if not rules.require_type_hints:
            return result

        try:
            tree = ast.parse(code)
        except SyntaxError:
            return result  # Syntax errors handled elsewhere

        for node in ast.walk(tree):
            if isinstance(node, ast.FunctionDef | ast.AsyncFunctionDef):
                self._check_function(node, rules, result)

        return result

    def _check_function(
        self,
        node: ast.FunctionDef | ast.AsyncFunctionDef,
        rules: QualityRulesConfig,
        result: AnalysisResult,
    ) -> None:
        """Check a function for type hints."""
        # Skip dunder methods
        if node.name.startswith("__") and node.name.endswith("__"):
            return

        # Check parameters
        for arg in node.args.args:
            # Skip self/cls
            if arg.arg in ("self", "cls"):
                continue

            if arg.annotation is None:
                result.add_violation(
                    "missing_type_hint",
                    f"Parameter '{arg.arg}' in function '{node.name}' has no type hint",
                    line=node.lineno,
                )

        # Check return type
        if rules.require_return_type and node.returns is None:
            # Skip __init__
            if node.name != "__init__":
                result.add_violation(
                    "missing_return_type",
                    f"Function '{node.name}' has no return type hint",
                    line=node.lineno,
                )
